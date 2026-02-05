#include "http2.h"
#include "connection.h"
#include "worker.h"
#include "router.h"
#include "static_files.h"
#include "slot_manager.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <nghttp2/nghttp2.h>
#include <event2/buffer.h>

/* Default HTTP/2 settings */
#define H2_MAX_CONCURRENT_STREAMS 100
#define H2_INITIAL_WINDOW_SIZE (1 << 20)  /* 1MB */

/* Forward declarations of nghttp2 callbacks */
static ssize_t h2_send_callback(nghttp2_session *session,
                                const uint8_t *data, size_t length,
                                int flags, void *user_data);
static int h2_on_frame_recv_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data);
static int h2_on_stream_close_callback(nghttp2_session *session,
                                       int32_t stream_id,
                                       uint32_t error_code,
                                       void *user_data);
static int h2_on_begin_headers_callback(nghttp2_session *session,
                                        const nghttp2_frame *frame,
                                        void *user_data);
static int h2_on_header_callback(nghttp2_session *session,
                                 const nghttp2_frame *frame,
                                 const uint8_t *name, size_t namelen,
                                 const uint8_t *value, size_t valuelen,
                                 uint8_t flags, void *user_data);
static int h2_on_data_chunk_recv_callback(nghttp2_session *session,
                                          uint8_t flags, int32_t stream_id,
                                          const uint8_t *data, size_t len,
                                          void *user_data);

/*
 * Create a new HTTP/2 stream.
 */
H2Stream *h2_stream_new(H2Connection *h2, int32_t stream_id)
{
    H2Stream *stream = calloc(1, sizeof(H2Stream));
    if (!stream) {
        return NULL;
    }

    stream->stream_id = stream_id;
    stream->state = H2_STREAM_OPEN;
    stream->tier = TIER_NORMAL;
    stream->slot_acquired = false;

    /* Add to list */
    stream->next = h2->streams;
    stream->prev = NULL;
    if (h2->streams) {
        h2->streams->prev = stream;
    }
    h2->streams = stream;
    h2->stream_count++;

    /* Update worker metrics */
    h2->worker->h2_streams_total++;
    h2->worker->h2_streams_active++;

    return stream;
}

/*
 * Find stream by ID.
 */
H2Stream *h2_stream_find(H2Connection *h2, int32_t stream_id)
{
    for (H2Stream *s = h2->streams; s; s = s->next) {
        if (s->stream_id == stream_id) {
            return s;
        }
    }
    return NULL;
}

/*
 * Free a stream and release its slot.
 */
void h2_stream_free(H2Connection *h2, H2Stream *stream)
{
    if (!stream) return;

    /* Release slot if acquired */
    if (stream->slot_acquired) {
        slot_manager_release(&h2->worker->slots, stream->tier);
        stream->slot_acquired = false;
    }

    /* Remove from list */
    if (stream->prev) {
        stream->prev->next = stream->next;
    } else {
        h2->streams = stream->next;
    }
    if (stream->next) {
        stream->next->prev = stream->prev;
    }
    h2->stream_count--;

    /* Update worker metrics */
    if (h2->worker->h2_streams_active > 0) {
        h2->worker->h2_streams_active--;
    }

    /* Free allocated strings */
    free(stream->method);
    free(stream->path);
    free(stream->authority);
    free(stream->scheme);

    /* Free response body source if allocated */
    free(stream->body_source);

    free(stream);
}

/*
 * Send callback - called by nghttp2 when it has data to send.
 */
static ssize_t h2_send_callback(nghttp2_session *session,
                                const uint8_t *data, size_t length,
                                int flags, void *user_data)
{
    Connection *conn = (Connection *)user_data;
    struct evbuffer *output = bufferevent_get_output(conn->bev);
    (void)session;
    (void)flags;

    if (evbuffer_add(output, data, length) != 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return (ssize_t)length;
}

/*
 * Frame received callback.
 */
static int h2_on_frame_recv_callback(nghttp2_session *session,
                                     const nghttp2_frame *frame,
                                     void *user_data)
{
    Connection *conn = (Connection *)user_data;
    H2Connection *h2 = conn->h2;
    (void)session;

    switch (frame->hd.type) {
        case NGHTTP2_HEADERS:
            /* Headers frame with END_STREAM - request complete */
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                H2Stream *stream = h2_stream_find(h2, frame->hd.stream_id);
                if (stream) {
                    stream->state = H2_STREAM_HALF_CLOSED_REMOTE;
                    /* Process the request */
                    /* Route and respond */
                    WorkerProcess *worker = h2->worker;
                    RouteType route = route_request(stream->path, stream->path_len);
                    StaticFile *file;
                    int status_code = 200;

                    switch (route) {
                        case ROUTE_HEALTH: {
                            char body[512];
                            int body_len = snprintf(body, sizeof(body),
                                "{\"status\":\"healthy\",\"worker_id\":%d,\"protocol\":\"h2\"}",
                                worker->worker_id);
                            h2_send_response(conn, frame->hd.stream_id, 200, "application/json",
                                             (const unsigned char *)body, body_len);
                            break;
                        }
                        case ROUTE_READY:
                            h2_send_response(conn, frame->hd.stream_id,
                                             worker->draining ? 503 : 200, "text/plain",
                                             (const unsigned char *)"", 0);
                            break;
                        case ROUTE_ALIVE:
                            h2_send_response(conn, frame->hd.stream_id, 200, "text/plain",
                                             (const unsigned char *)"", 0);
                            break;
                        case ROUTE_METRICS: {
                            char body[256];
                            int body_len = snprintf(body, sizeof(body),
                                "# HTTP/2 metrics\nrawrelay_h2_active{worker=\"%d\"} 1\n",
                                worker->worker_id);
                            h2_send_response(conn, frame->hd.stream_id, 200, "text/plain",
                                             (const unsigned char *)body, body_len);
                            break;
                        }
                        case ROUTE_BROADCAST:
                            file = &worker->static_files.broadcast;
                            h2_send_response(conn, frame->hd.stream_id, status_code, file->content_type,
                                             (const unsigned char *)file->content, file->length);
                            break;
                        case ROUTE_RESULT:
                            file = &worker->static_files.result;
                            h2_send_response(conn, frame->hd.stream_id, status_code, file->content_type,
                                             (const unsigned char *)file->content, file->length);
                            break;
                        case ROUTE_ERROR:
                        default:
                            file = &worker->static_files.error;
                            status_code = 400;
                            h2_send_response(conn, frame->hd.stream_id, status_code, file->content_type,
                                             (const unsigned char *)file->content, file->length);
                            break;
                    }
                }
            }
            break;

        case NGHTTP2_DATA:
            /* Data frame with END_STREAM */
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                H2Stream *stream = h2_stream_find(h2, frame->hd.stream_id);
                if (stream) {
                    stream->state = H2_STREAM_HALF_CLOSED_REMOTE;
                    /* Process with body - similar to HEADERS handler */
                    WorkerProcess *worker = h2->worker;
                    RouteType route = route_request(stream->path, stream->path_len);
                    StaticFile *file;
                    int status_code = 200;

                    switch (route) {
                        case ROUTE_HEALTH: {
                            char body[512];
                            int body_len = snprintf(body, sizeof(body),
                                "{\"status\":\"healthy\",\"worker_id\":%d,\"protocol\":\"h2\"}",
                                worker->worker_id);
                            h2_send_response(conn, frame->hd.stream_id, 200, "application/json",
                                             (const unsigned char *)body, body_len);
                            break;
                        }
                        case ROUTE_READY:
                            h2_send_response(conn, frame->hd.stream_id,
                                             worker->draining ? 503 : 200, "text/plain",
                                             (const unsigned char *)"", 0);
                            break;
                        case ROUTE_ALIVE:
                            h2_send_response(conn, frame->hd.stream_id, 200, "text/plain",
                                             (const unsigned char *)"", 0);
                            break;
                        case ROUTE_METRICS: {
                            char body[256];
                            int body_len = snprintf(body, sizeof(body),
                                "# HTTP/2 metrics\nrawrelay_h2_active{worker=\"%d\"} 1\n",
                                worker->worker_id);
                            h2_send_response(conn, frame->hd.stream_id, 200, "text/plain",
                                             (const unsigned char *)body, body_len);
                            break;
                        }
                        case ROUTE_BROADCAST:
                            file = &worker->static_files.broadcast;
                            h2_send_response(conn, frame->hd.stream_id, status_code, file->content_type,
                                             (const unsigned char *)file->content, file->length);
                            break;
                        case ROUTE_RESULT:
                            file = &worker->static_files.result;
                            h2_send_response(conn, frame->hd.stream_id, status_code, file->content_type,
                                             (const unsigned char *)file->content, file->length);
                            break;
                        case ROUTE_ERROR:
                        default:
                            file = &worker->static_files.error;
                            status_code = 400;
                            h2_send_response(conn, frame->hd.stream_id, status_code, file->content_type,
                                             (const unsigned char *)file->content, file->length);
                            break;
                    }
                }
            }
            break;

        default:
            break;
    }

    return 0;
}

/*
 * Stream close callback - release slot.
 */
static int h2_on_stream_close_callback(nghttp2_session *session,
                                       int32_t stream_id,
                                       uint32_t error_code,
                                       void *user_data)
{
    Connection *conn = (Connection *)user_data;
    H2Connection *h2 = conn->h2;
    (void)session;
    (void)error_code;

    H2Stream *stream = h2_stream_find(h2, stream_id);
    if (stream) {
        stream->state = H2_STREAM_CLOSED;
        h2_stream_free(h2, stream);
    }

    return 0;
}

/*
 * Begin headers callback - new stream starting, acquire slot.
 */
static int h2_on_begin_headers_callback(nghttp2_session *session,
                                        const nghttp2_frame *frame,
                                        void *user_data)
{
    Connection *conn = (Connection *)user_data;
    H2Connection *h2 = conn->h2;
    (void)session;

    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    /* Try to acquire a NORMAL tier slot for the new stream */
    if (!slot_manager_acquire(&h2->worker->slots, TIER_NORMAL)) {
        log_warn("HTTP/2: Cannot accept stream %d - no slots available",
                 frame->hd.stream_id);
        return NGHTTP2_ERR_CALLBACK_FAILURE;  /* Will trigger RST_STREAM */
    }

    /* Create stream */
    H2Stream *stream = h2_stream_new(h2, frame->hd.stream_id);
    if (!stream) {
        slot_manager_release(&h2->worker->slots, TIER_NORMAL);
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    stream->slot_acquired = true;
    stream->tier = TIER_NORMAL;

    return 0;
}

/*
 * Header callback - process :path to determine tier.
 * This is where we know the request size BEFORE processing!
 */
static int h2_on_header_callback(nghttp2_session *session,
                                 const nghttp2_frame *frame,
                                 const uint8_t *name, size_t namelen,
                                 const uint8_t *value, size_t valuelen,
                                 uint8_t flags, void *user_data)
{
    Connection *conn = (Connection *)user_data;
    H2Connection *h2 = conn->h2;
    (void)session;
    (void)flags;

    if (frame->hd.type != NGHTTP2_HEADERS) {
        return 0;
    }

    H2Stream *stream = h2_stream_find(h2, frame->hd.stream_id);
    if (!stream) {
        return 0;
    }

    /* Store headers */
    if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
        stream->method = strndup((const char *)value, valuelen);
    } else if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
        stream->path = strndup((const char *)value, valuelen);
        stream->path_len = valuelen;

        /* THE KEY WIN: Size known BEFORE allocating! */
        /* Try to promote tier based on path length */
        RequestTier required = size_to_tier(valuelen, h2->worker->config);
        if (required > stream->tier) {
            if (!slot_manager_promote(&h2->worker->slots, stream->tier, required)) {
                log_warn("HTTP/2: Cannot promote stream %d from %s to %s tier",
                         stream->stream_id, tier_name(stream->tier), tier_name(required));
                /* Reject the stream with REFUSED_STREAM */
                nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                          stream->stream_id, NGHTTP2_REFUSED_STREAM);
                h2->worker->h2_rst_stream_total++;
                return 0;
            }
            stream->tier = required;
            log_debug("HTTP/2: Promoted stream %d to %s tier (path len %zu)",
                      stream->stream_id, tier_name(required), valuelen);
        }
    } else if (namelen == 10 && memcmp(name, ":authority", 10) == 0) {
        stream->authority = strndup((const char *)value, valuelen);
    } else if (namelen == 7 && memcmp(name, ":scheme", 7) == 0) {
        stream->scheme = strndup((const char *)value, valuelen);
    } else if (namelen == 14 && memcmp(name, "content-length", 14) == 0) {
        stream->content_length = (size_t)strtoul((const char *)value, NULL, 10);
    }

    return 0;
}

/*
 * Data chunk received callback.
 */
static int h2_on_data_chunk_recv_callback(nghttp2_session *session,
                                          uint8_t flags, int32_t stream_id,
                                          const uint8_t *data, size_t len,
                                          void *user_data)
{
    Connection *conn = (Connection *)user_data;
    H2Connection *h2 = conn->h2;
    (void)session;
    (void)flags;
    (void)data;  /* We don't process body data for now */

    H2Stream *stream = h2_stream_find(h2, stream_id);
    if (stream) {
        stream->body_received += len;
    }

    return 0;
}

/*
 * Initialize HTTP/2 session on a connection.
 */
int h2_connection_init(Connection *conn)
{
    H2Connection *h2 = calloc(1, sizeof(H2Connection));
    if (!h2) {
        log_error("Failed to allocate H2Connection");
        return -1;
    }

    h2->conn = conn;
    h2->worker = conn->worker;
    h2->max_concurrent_streams = H2_MAX_CONCURRENT_STREAMS;
    h2->initial_window_size = H2_INITIAL_WINDOW_SIZE;

    /* Set up nghttp2 callbacks */
    nghttp2_session_callbacks *callbacks;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        free(h2);
        return -1;
    }

    nghttp2_session_callbacks_set_send_callback(callbacks, h2_send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, h2_on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, h2_on_stream_close_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, h2_on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, h2_on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, h2_on_data_chunk_recv_callback);

    /* Create server session */
    int rv = nghttp2_session_server_new(&h2->session, callbacks, conn);
    nghttp2_session_callbacks_del(callbacks);

    if (rv != 0) {
        log_error("Failed to create nghttp2 session: %s", nghttp2_strerror(rv));
        free(h2);
        return -1;
    }

    /* Send SETTINGS frame */
    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, h2->max_concurrent_streams},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, h2->initial_window_size}
    };

    rv = nghttp2_submit_settings(h2->session, NGHTTP2_FLAG_NONE,
                                  settings, sizeof(settings) / sizeof(settings[0]));
    if (rv != 0) {
        log_error("Failed to submit SETTINGS: %s", nghttp2_strerror(rv));
        nghttp2_session_del(h2->session);
        free(h2);
        return -1;
    }

    conn->h2 = h2;
    conn->protocol = PROTO_HTTP_2;

    log_debug("HTTP/2 session initialized for %s", conn->client_ip);

    return 0;
}

/*
 * Free HTTP/2 session.
 */
void h2_connection_free(H2Connection *h2)
{
    if (!h2) return;

    /* Free all streams */
    while (h2->streams) {
        h2_stream_free(h2, h2->streams);
    }

    /* Free nghttp2 session */
    if (h2->session) {
        nghttp2_session_del(h2->session);
    }

    free(h2);
}

/*
 * Process incoming data.
 */
int h2_process_input(Connection *conn, const unsigned char *data, size_t len)
{
    H2Connection *h2 = conn->h2;
    if (!h2 || !h2->session) {
        return -1;
    }

    ssize_t rv = nghttp2_session_mem_recv(h2->session, data, len);
    if (rv < 0) {
        log_error("HTTP/2: nghttp2_session_mem_recv failed: %s",
                  nghttp2_strerror((int)rv));
        return -1;
    }

    /* Send any pending data */
    return h2_send_pending(conn);
}

/*
 * Send pending output data.
 */
int h2_send_pending(Connection *conn)
{
    H2Connection *h2 = conn->h2;
    if (!h2 || !h2->session) {
        return -1;
    }

    int rv = nghttp2_session_send(h2->session);
    if (rv != 0) {
        log_error("HTTP/2: nghttp2_session_send failed: %s",
                  nghttp2_strerror(rv));
        return -1;
    }

    return 0;
}

/* Body source for response data provider */
typedef struct {
    const unsigned char *data;
    size_t len;
    size_t pos;
} H2BodySource;

/* Callback for reading response body */
static ssize_t h2_body_read_callback(nghttp2_session *session,
                                     int32_t stream_id,
                                     uint8_t *buf, size_t length,
                                     uint32_t *data_flags,
                                     nghttp2_data_source *source,
                                     void *user_data)
{
    H2BodySource *bs = (H2BodySource *)source->ptr;
    (void)session;
    (void)stream_id;
    (void)user_data;

    size_t remaining = bs->len - bs->pos;
    size_t to_copy = remaining < length ? remaining : length;

    memcpy(buf, bs->data + bs->pos, to_copy);
    bs->pos += to_copy;

    if (bs->pos >= bs->len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (ssize_t)to_copy;
}

/*
 * Send HTTP/2 response.
 */
int h2_send_response(Connection *conn, int32_t stream_id,
                     int status_code, const char *content_type,
                     const unsigned char *body, size_t body_len)
{
    H2Connection *h2 = conn->h2;
    if (!h2 || !h2->session) {
        return -1;
    }

    /* Build status string */
    char status_str[16];
    snprintf(status_str, sizeof(status_str), "%d", status_code);

    /* Build content-length string */
    char content_length_str[32];
    snprintf(content_length_str, sizeof(content_length_str), "%zu", body_len);

    /* Headers */
    nghttp2_nv headers[] = {
        {(uint8_t *)":status", (uint8_t *)status_str, 7, strlen(status_str), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-type", (uint8_t *)content_type, 12, strlen(content_type), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-length", (uint8_t *)content_length_str, 14, strlen(content_length_str), NGHTTP2_NV_FLAG_NONE}
    };

    /* Data provider for body - allocate on heap so it survives async send */
    H2BodySource *body_source = malloc(sizeof(H2BodySource));
    if (!body_source) {
        return -1;
    }
    body_source->data = body;
    body_source->len = body_len;
    body_source->pos = 0;

    nghttp2_data_provider data_prd;
    data_prd.source.ptr = body_source;
    data_prd.read_callback = h2_body_read_callback;

    int rv = nghttp2_submit_response(h2->session, stream_id,
                                      headers, sizeof(headers) / sizeof(headers[0]),
                                      &data_prd);
    if (rv != 0) {
        log_error("HTTP/2: Failed to submit response: %s", nghttp2_strerror(rv));
        free(body_source);
        return -1;
    }

    /* Track body_source in stream for proper cleanup */
    H2Stream *stream = h2_stream_find(h2, stream_id);
    if (stream) {
        /* Free any previous body_source (shouldn't happen, but be safe) */
        free(stream->body_source);
        stream->body_source = body_source;
    }

    return h2_send_pending(conn);
}

/*
 * Send error response (RST_STREAM).
 */
int h2_send_error(Connection *conn, int32_t stream_id, uint32_t error_code)
{
    H2Connection *h2 = conn->h2;
    if (!h2 || !h2->session) {
        return -1;
    }

    int rv = nghttp2_submit_rst_stream(h2->session, NGHTTP2_FLAG_NONE,
                                        stream_id, error_code);
    if (rv != 0) {
        log_error("HTTP/2: Failed to submit RST_STREAM: %s", nghttp2_strerror(rv));
        return -1;
    }

    /* Track RST_STREAM metrics */
    h2->worker->h2_rst_stream_total++;

    return h2_send_pending(conn);
}
