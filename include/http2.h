#ifndef HTTP2_H
#define HTTP2_H

#include "config.h"
#include "reader.h"  /* For RequestTier */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <event2/bufferevent.h>

/* Forward declarations */
struct Connection;
struct WorkerProcess;
struct nghttp2_session;

/*
 * HTTP/2 stream state.
 * Each HTTP/2 stream represents a single request/response pair.
 */
typedef enum {
    H2_STREAM_IDLE,
    H2_STREAM_OPEN,
    H2_STREAM_HALF_CLOSED_REMOTE,  /* Client sent END_STREAM */
    H2_STREAM_HALF_CLOSED_LOCAL,   /* We sent END_STREAM */
    H2_STREAM_CLOSED
} H2StreamState;

/*
 * HTTP/2 stream - one per request within a connection.
 * Tracks per-stream slot allocation for proper resource management.
 */
typedef struct H2Stream {
    int32_t stream_id;
    H2StreamState state;

    /* Slot tier for this stream */
    RequestTier tier;
    bool slot_acquired;

    /* Request info */
    char *method;
    char *path;
    size_t path_len;
    char *authority;
    char *scheme;

    /* Request body */
    size_t content_length;
    size_t body_received;

    /* Response body source (for cleanup) */
    void *body_source;

    /* Linked list of streams */
    struct H2Stream *next;
    struct H2Stream *prev;
} H2Stream;

/*
 * HTTP/2 connection state.
 * Manages nghttp2 session and all streams on this connection.
 */
typedef struct H2Connection {
    struct nghttp2_session *session;
    struct Connection *conn;          /* Parent connection */
    struct WorkerProcess *worker;

    /* Active streams */
    H2Stream *streams;
    int stream_count;

    /* Settings */
    uint32_t max_concurrent_streams;
    uint32_t initial_window_size;
} H2Connection;

/*
 * Initialize HTTP/2 session on a connection.
 * Called after TLS handshake when ALPN selects h2.
 * Returns 0 on success, -1 on error.
 */
int h2_connection_init(struct Connection *conn);

/*
 * Free HTTP/2 session resources.
 */
void h2_connection_free(H2Connection *h2);

/*
 * Process incoming data from the connection.
 * Feeds data to nghttp2 for parsing.
 * Returns 0 on success, -1 on error (should close connection).
 */
int h2_process_input(struct Connection *conn, const unsigned char *data, size_t len);

/*
 * Send pending output data.
 * Called after nghttp2 generates frames.
 * Returns 0 on success, -1 on error.
 */
int h2_send_pending(struct Connection *conn);

/*
 * Create a new stream.
 */
H2Stream *h2_stream_new(H2Connection *h2, int32_t stream_id);

/*
 * Find stream by ID.
 */
H2Stream *h2_stream_find(H2Connection *h2, int32_t stream_id);

/*
 * Free a stream.
 */
void h2_stream_free(H2Connection *h2, H2Stream *stream);

/*
 * Send HTTP/2 response on a stream.
 */
int h2_send_response(struct Connection *conn, int32_t stream_id,
                     int status_code, const char *content_type,
                     const unsigned char *body, size_t body_len);

/*
 * Send HTTP/2 error response (RST_STREAM with error code).
 */
int h2_send_error(struct Connection *conn, int32_t stream_id, uint32_t error_code);

#endif /* HTTP2_H */
