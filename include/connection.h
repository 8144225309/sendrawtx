#ifndef CONNECTION_H
#define CONNECTION_H

#include "config.h"
#include "reader.h"  /* For RequestTier */
#include <stdint.h>
#include <stdbool.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <netinet/in.h>

/*
 * Connection states for HTTP request handling.
 */
typedef enum {
    CONN_STATE_READING_HEADERS,   /* Reading HTTP request headers */
    CONN_STATE_READING_BODY,      /* Reading request body (if any) */
    CONN_STATE_PROCESSING,        /* Processing request */
    CONN_STATE_WRITING_RESPONSE,  /* Writing HTTP response */
    CONN_STATE_CLOSING            /* Connection closing */
} ConnState;

/*
 * Protocol type for this connection.
 */
typedef enum {
    PROTO_HTTP_1_1 = 0,           /* HTTP/1.1 (default) */
    PROTO_HTTP_2   = 1            /* HTTP/2 (via ALPN) */
} ProtocolType;

/*
 * Forward declarations.
 */
struct WorkerProcess;
struct H2Connection;

/*
 * Connection structure - one per client connection.
 * Managed via bufferevent for async I/O.
 *
 * v6: Uses evbuffer directly (no redundant copying).
 * Supports large URLs (up to max_buffer_size, typically 16MB).
 * Uses tiered slots (normal/large/huge) based on request size.
 */
typedef struct Connection {
    /* Libevent */
    struct bufferevent *bev;
    struct WorkerProcess *worker;

    /* State machine */
    ConnState state;

    /* Protocol (HTTP/1.1 or HTTP/2) */
    ProtocolType protocol;

    /* Slot tier - tracks which slot pool this connection uses */
    RequestTier current_tier;

    /* Client info */
    char client_ip[64];
    uint16_t client_port;

    /* Request parsing - v6: no more request_buffer/request_len/request_capacity
     * We now use evbuffer_search/pullup/drain directly on bufferevent's input */
    size_t headers_scanned;      /* How much of evbuffer we've scanned for \r\n\r\n */
    size_t headers_end;          /* Offset where headers end (once found) */
    size_t content_length;       /* From Content-Length header */
    size_t body_received;        /* Bytes of body received so far */

    /* Request info (parsed from headers) */
    char method[16];
    char *path;                  /* Dynamically allocated for large URLs */
    size_t path_len;

    /* Early validation state */
    bool path_validated;         /* Have we started hex validation? */
    bool validation_failed;      /* Did early validation fail? */

    /* Keep-alive support (Phase 4) */
    bool keep_alive;             /* Connection supports keep-alive */
    bool slot_held;              /* Currently holding a request slot */
    int requests_on_connection;  /* Number of requests processed on this connection */

    /* TLS support (Phase 2) */
    void *ssl;                   /* SSL* - opaque to avoid header dependency */
    bool tls_handshake_done;

    /* HTTP/2 support (Phase 3) */
    struct H2Connection *h2;     /* HTTP/2 session state */

    /* Timing */
    struct timeval start_time;

    /* Slowloris protection - throughput tracking */
    struct timeval last_progress_time;  /* Last time we checked throughput */
    size_t bytes_at_last_check;         /* Bytes received at last check */

    /* Request tracking (Phase 5) */
    char request_id[32];                /* Unique request ID for tracing */
    int response_status;                /* HTTP status code of response */
    size_t response_bytes;              /* Response body size */

    /* Link for connection tracking (intrusive list) */
    struct Connection *next;
    struct Connection *prev;
} Connection;

/*
 * Create a new connection from accepted socket.
 * Takes ownership of fd.
 */
Connection *connection_new(struct WorkerProcess *worker, evutil_socket_t fd,
                           struct sockaddr *addr, int addrlen);

/*
 * Create a new connection from a pre-created bufferevent.
 * Used by TLS accept path where bufferevent_openssl_socket_new()
 * has already created the bufferevent.
 * Does NOT take ownership of bev on failure (caller must free).
 */
Connection *connection_new_with_bev(struct WorkerProcess *worker,
                                     struct bufferevent *bev,
                                     struct sockaddr *addr, int addrlen);

/*
 * Close and free a connection.
 */
void connection_free(Connection *conn);

/*
 * Send HTTP response and close connection.
 */
void connection_send_response(Connection *conn, int status_code,
                              const char *status_text, const char *body);

/*
 * Send error response (4xx, 5xx).
 */
void connection_send_error(Connection *conn, int status_code,
                           const char *status_text);

/*
 * Set up callbacks for a connection.
 * Used by TLS accept to configure bufferevent callbacks.
 */
void connection_set_callbacks(Connection *conn);

#endif /* CONNECTION_H */
