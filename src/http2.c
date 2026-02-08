#include "http2.h"
#include "connection.h"
#include "worker.h"
#include "router.h"
#include "static_files.h"
#include "slot_manager.h"
#include "endpoints.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <nghttp2/nghttp2.h>
#include <event2/buffer.h>

/* Default HTTP/2 settings */
#define H2_MAX_CONCURRENT_STREAMS 100
#define H2_INITIAL_WINDOW_SIZE (1 << 20)  /* 1MB */

/* Connection-level flow control window (16MB) */
#define H2_CONNECTION_WINDOW_SIZE (16 * 1024 * 1024)

/* Body source for response data provider */
typedef struct {
    unsigned char *data;  /* Owned copy of the body data */
    size_t len;
    size_t pos;
} H2BodySource;

/* Per-stream request ID counter (per-process) */
static uint32_t h2_request_counter = 0;

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

    /* Per-stream request tracking */
    gettimeofday(&stream->start_time, NULL);
    snprintf(stream->request_id, sizeof(stream->request_id), "%d-%lx-%xs%d",
             h2->worker->worker_id,
             (unsigned long)(stream->start_time.tv_sec * 1000000 + stream->start_time.tv_usec),
             h2_request_counter++, stream_id);

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

    /* Free response body source and its owned data */
    if (stream->body_source) {
        H2BodySource *bs = (H2BodySource *)stream->body_source;
        free(bs->data);
        free(bs);
        stream->body_source = NULL;
    }

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
 * Downgrade HTTP/2 stream from large/huge tier to normal.
 * Called after request is fully received to release expensive slots ASAP.
 */
static void h2_downgrade_tier_to_normal(H2Connection *h2, H2Stream *stream)
{
    if (!stream->slot_acquired || stream->tier == TIER_NORMAL) {
        return;
    }

    /* Release the expensive slot */
    slot_manager_release(&h2->worker->slots, stream->tier);

    /* Acquire normal slot for response phase */
    if (slot_manager_acquire(&h2->worker->slots, TIER_NORMAL)) {
        log_debug("HTTP/2: Downgraded stream %d from %s to normal tier",
                  stream->stream_id, tier_name(stream->tier));
        stream->tier = TIER_NORMAL;
    } else {
        /* Can't get normal slot - try to re-acquire the expensive one */
        if (!slot_manager_acquire(&h2->worker->slots, stream->tier)) {
            stream->slot_acquired = false;
        }
    }
}

/*
 * Process a complete HTTP/2 stream request.
 * Unified routing handler called from both HEADERS and DATA END_STREAM paths.
 */
static void h2_process_stream_request(Connection *conn, H2Stream *stream)
{
    H2Connection *h2 = conn->h2;
    WorkerProcess *worker = h2->worker;
    RouteType route = route_request(stream->path, stream->path_len);
    StaticFile *file;
    int status_code = 200;
    const char *content_type = "text/plain";
    size_t body_len = 0;

    switch (route) {
        case ROUTE_HEALTH: {
            char body[1024];
            int len = generate_health_body(worker, body, sizeof(body));
            status_code = 200;
            content_type = "application/json";
            body_len = len;
            h2_send_response(conn, stream->stream_id, status_code, content_type,
                             (const unsigned char *)body, body_len);
            break;
        }
        case ROUTE_READY:
            status_code = worker->draining ? 503 : 200;
            h2_send_response(conn, stream->stream_id, status_code, "text/plain",
                             (const unsigned char *)"", 0);
            break;
        case ROUTE_ALIVE:
            status_code = 200;
            h2_send_response(conn, stream->stream_id, 200, "text/plain",
                             (const unsigned char *)"", 0);
            break;
        case ROUTE_METRICS: {
            char body[8192];
            int len = generate_metrics_body(worker, body, sizeof(body));
            status_code = 200;
            content_type = "text/plain; version=0.0.4; charset=utf-8";
            body_len = len;
            h2_send_response(conn, stream->stream_id, status_code, content_type,
                             (const unsigned char *)body, body_len);
            break;
        }
        case ROUTE_ACME_CHALLENGE: {
            int acme_len = serve_acme_challenge_h2(conn, stream->stream_id,
                                        stream->path, stream->path_len,
                                        stream->request_id);
            if (acme_len > 0) {
                status_code = 200;
                body_len = acme_len;
            } else {
                status_code = 404;
                body_len = 9;  /* "Not Found" */
            }
            break;
        }
        case ROUTE_BROADCAST:
            file = &worker->static_files.broadcast;
            body_len = file->length;
            h2_send_response(conn, stream->stream_id, status_code, file->content_type,
                             (const unsigned char *)file->content, file->length);
            break;
        case ROUTE_RESULT:
            file = &worker->static_files.result;
            body_len = file->length;
            h2_send_response(conn, stream->stream_id, status_code, file->content_type,
                             (const unsigned char *)file->content, file->length);
            break;
        case ROUTE_ERROR:
        default:
            file = &worker->static_files.error;
            status_code = 400;
            body_len = file->length;
            h2_send_response(conn, stream->stream_id, status_code, file->content_type,
                             (const unsigned char *)file->content, file->length);
            break;
    }

    /* Track response in stream */
    stream->response_status = status_code;
    stream->response_bytes = body_len;

    /* Tier downgrade: release expensive slot ASAP after request is processed */
    h2_downgrade_tier_to_normal(h2, stream);
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
            /* Headers frame with END_STREAM - request complete (no body) */
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                H2Stream *stream = h2_stream_find(h2, frame->hd.stream_id);
                if (stream) {
                    stream->state = H2_STREAM_HALF_CLOSED_REMOTE;
                    h2_process_stream_request(conn, stream);
                }
            }
            break;

        case NGHTTP2_DATA:
            /* Data frame with END_STREAM - request with body complete */
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                H2Stream *stream = h2_stream_find(h2, frame->hd.stream_id);
                if (stream) {
                    stream->state = H2_STREAM_HALF_CLOSED_REMOTE;
                    h2_process_stream_request(conn, stream);
                }
            }
            break;

        default:
            break;
    }

    return 0;
}

/*
 * Stream close callback - log access and release slot.
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
        /* Access logging for HTTP/2 streams */
        if (stream->response_status > 0) {
            WorkerProcess *worker = h2->worker;
            struct timeval now;
            gettimeofday(&now, NULL);
            double duration_ms = (now.tv_sec - stream->start_time.tv_sec) * 1000.0 +
                                 (now.tv_usec - stream->start_time.tv_usec) / 1000.0;
            double duration_sec = duration_ms / 1000.0;

            update_latency_histogram(worker, duration_sec);
            update_status_counters(worker, stream->response_status);
            update_method_counters(worker, stream->method);

            log_request_access(conn->client_ip,
                               stream->method ? stream->method : "???",
                               stream->path ? stream->path : "/",
                               stream->response_status,
                               stream->response_bytes,
                               duration_ms,
                               stream->request_id);
        }

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
        h2->worker->h2_rst_stream_total++;
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;  /* Rejects this stream only (RST_STREAM), not the session */
    }

    /* Create stream */
    H2Stream *stream = h2_stream_new(h2, frame->hd.stream_id);
    if (!stream) {
        slot_manager_release(&h2->worker->slots, TIER_NORMAL);
        h2->worker->h2_rst_stream_total++;
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    stream->slot_acquired = true;
    stream->tier = TIER_NORMAL;

    return 0;
}

/*
 * Header callback - process :path to determine tier and validate hex.
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

        /* Hex path validation for long paths (same as HTTP/1.1 validate_path_early) */
        if (stream->path && stream->path_len > 1) {
            /* Skip leading slash for validation */
            const char *path_content = stream->path + 1;
            size_t content_len = stream->path_len - 1;

            if (validate_hex_path(path_content, content_len) < 0) {
                log_warn("HTTP/2: Invalid hex in path from %s on stream %d",
                         log_format_ip(conn->client_ip), stream->stream_id);
                nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                          stream->stream_id, NGHTTP2_REFUSED_STREAM);
                h2->worker->h2_rst_stream_total++;
                h2->worker->errors_parse++;
                return 0;
            }
        }

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

    /* === nghttp2 Security Hardening === */
    nghttp2_option *option;
    if (nghttp2_option_new(&option) != 0) {
        nghttp2_session_callbacks_del(callbacks);
        free(h2);
        return -1;
    }

    /* CVE-2023-44487: Rapid Reset attack protection.
     * Limit stream reset rate to 1000 resets per 33 second window. */
#ifdef NGHTTP2_VERSION_NUM
#if NGHTTP2_VERSION_NUM >= 0x013b00  /* 1.59.0 */
    nghttp2_option_set_stream_reset_rate_limit(option, 1000, 33);
#endif
#endif

    /* CVE-2024-28182: CONTINUATION flood protection.
     * Limit CONTINUATION frames per HEADERS sequence. */
#ifdef NGHTTP2_VERSION_NUM
#if NGHTTP2_VERSION_NUM >= 0x013c00  /* 1.60.0 */
    nghttp2_option_set_max_continuations(option, 8);
#endif
#endif

    /* SETTINGS flood protection */
#ifdef NGHTTP2_VERSION_NUM
#if NGHTTP2_VERSION_NUM >= 0x012600  /* 1.38.0 */
    nghttp2_option_set_max_settings(option, 32);
#endif
#endif

    /* Ping/SETTINGS ACK flood protection */
#ifdef NGHTTP2_VERSION_NUM
#if NGHTTP2_VERSION_NUM >= 0x012600  /* 1.38.0 */
    nghttp2_option_set_max_outbound_ack(option, 1000);
#endif
#endif

    /* Create server session with security options */
    int rv = nghttp2_session_server_new2(&h2->session, callbacks, conn, option);
    nghttp2_session_callbacks_del(callbacks);
    nghttp2_option_del(option);

    if (rv != 0) {
        log_error("Failed to create nghttp2 session: %s", nghttp2_strerror(rv));
        free(h2);
        return -1;
    }

    /* Send SETTINGS frame with security-relevant settings */
    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, h2->max_concurrent_streams},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, h2->initial_window_size},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, (uint32_t)(conn->worker->config->max_buffer_size + 4096)},
        {NGHTTP2_SETTINGS_ENABLE_PUSH, 0}
    };

    rv = nghttp2_submit_settings(h2->session, NGHTTP2_FLAG_NONE,
                                  settings, sizeof(settings) / sizeof(settings[0]));
    if (rv != 0) {
        log_error("Failed to submit SETTINGS: %s", nghttp2_strerror(rv));
        nghttp2_session_del(h2->session);
        free(h2);
        return -1;
    }

    /* Set connection-level flow control window.
     * Prevents the connection window from bottlenecking when multiple
     * streams are active with large responses. */
    rv = nghttp2_session_set_local_window_size(h2->session, NGHTTP2_FLAG_NONE,
                                                0, H2_CONNECTION_WINDOW_SIZE);
    if (rv != 0) {
        log_debug("HTTP/2: Failed to set connection window size: %s",
                  nghttp2_strerror(rv));
        /* Non-fatal - continue with default window */
    }

    conn->h2 = h2;
    conn->protocol = PROTO_HTTP_2;

    log_debug("HTTP/2 session initialized for %s", log_format_ip(conn->client_ip));

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

    if (to_copy > 0)
        memcpy(buf, bs->data + bs->pos, to_copy);
    bs->pos += to_copy;

    if (bs->pos >= bs->len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (ssize_t)to_copy;
}

/*
 * Send HTTP/2 response with full headers.
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

    /* Determine cache-control based on route */
    const char *cache_control = "no-store";
    int cache_max_age = conn->worker->config->cache_max_age;
    char cache_control_buf[64];
    /* Static file routes use configurable caching */
    if (status_code == 200 && cache_max_age > 0 &&
        strcmp(content_type, "text/html; charset=utf-8") == 0) {
        snprintf(cache_control_buf, sizeof(cache_control_buf),
                 "public, max-age=%d", cache_max_age);
        cache_control = cache_control_buf;
    }

    /* Get per-stream request ID */
    const char *request_id = "";
    H2Stream *stream = h2_stream_find(h2, stream_id);
    if (stream) {
        request_id = stream->request_id;
    }

    /* Headers */
    nghttp2_nv headers[] = {
        {(uint8_t *)":status", (uint8_t *)status_str,
         7, strlen(status_str), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-type", (uint8_t *)content_type,
         12, strlen(content_type), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-length", (uint8_t *)content_length_str,
         14, strlen(content_length_str), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"x-request-id", (uint8_t *)request_id,
         12, strlen(request_id), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"cache-control", (uint8_t *)cache_control,
         13, strlen(cache_control), NGHTTP2_NV_FLAG_NONE}
    };

    /* Data provider for body - copy body data so it survives async send.
     * Callers often pass stack-allocated buffers (e.g. char body[8192])
     * which go out of scope before nghttp2 reads the data. */
    H2BodySource *body_source = malloc(sizeof(H2BodySource));
    if (!body_source) {
        return -1;
    }
    if (body_len > 0) {
        body_source->data = malloc(body_len);
        if (!body_source->data) {
            free(body_source);
            return -1;
        }
        memcpy(body_source->data, body, body_len);
    } else {
        body_source->data = NULL;
    }
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
        free(body_source->data);
        free(body_source);
        return -1;
    }

    /* Track body_source in stream for proper cleanup */
    if (stream) {
        /* Free any previous body_source (shouldn't happen, but be safe) */
        if (stream->body_source) {
            H2BodySource *old_bs = (H2BodySource *)stream->body_source;
            free(old_bs->data);
            free(old_bs);
        }
        stream->body_source = body_source;
    }

    return h2_send_pending(conn);
}
