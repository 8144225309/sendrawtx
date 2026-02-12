#include "connection.h"
#include "worker.h"
#include "tcp_opts.h"
#include "router.h"
#include "static_files.h"
#include "slot_manager.h"
#include "http2.h"
#include "tls.h"
#include "hex.h"
#include "endpoints.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <event2/buffer.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/* Read timeout in seconds */
#define READ_TIMEOUT_SEC 30

/* Slowloris protection settings */
#define THROUGHPUT_CHECK_INTERVAL_SEC 5   /* Check throughput every N seconds */
#define MIN_BYTES_PER_CHECK 100           /* Minimum bytes expected per check interval */
#define MAX_REQUEST_TIME_SEC 120          /* Maximum time to complete request */

/* Forward declarations */
static void conn_read_cb(struct bufferevent *bev, void *ctx);
static void conn_write_cb(struct bufferevent *bev, void *ctx);
static void conn_event_cb(struct bufferevent *bev, short events, void *ctx);
static int parse_request_headers(Connection *conn, const unsigned char *headers, size_t len);
static int try_promote_tier(Connection *conn, size_t new_size);
static int validate_path_early(Connection *conn, const unsigned char *data, size_t len);

/*
 * Early validation of path data as it arrives.
 * For transaction broadcasts (long paths), validates hex characters.
 * Returns 0 if valid, -1 if invalid.
 */
static int validate_path_early(Connection *conn, const unsigned char *data, size_t len)
{
    /* Skip if already validated or failed */
    if (conn->validation_failed) {
        return -1;
    }

    /* Look for the path in the request line */
    /* Format: "GET /path HTTP/1.1\r\n" */

    /* Find first space (after method) */
    const unsigned char *path_start = memchr(data, ' ', len);
    if (!path_start) {
        return 0;  /* Not enough data yet */
    }
    path_start++;  /* Skip space */

    /* Security: bounds check after increment */
    const unsigned char *data_end = data + len;
    if (path_start >= data_end) {
        return 0;  /* Not enough data yet */
    }

    /* Skip the leading slash */
    if (*path_start == '/') {
        path_start++;
        /* Security: bounds check after increment */
        if (path_start >= data_end) {
            return 0;  /* Not enough data yet */
        }
    }

    /* Find end of path (space before HTTP version or \r\n) */
    const unsigned char *path_end = NULL;
    for (const unsigned char *p = path_start; p < data_end; p++) {
        if (*p == ' ' || *p == '\r' || *p == '\n') {
            path_end = p;
            break;
        }
    }

    if (!path_end) {
        /* Path not complete yet - validate what we have */
        path_end = data_end;
    }

    /* For paths that look like transaction hex (long paths), validate characters */
    size_t path_len = path_end - path_start;

    /* Short paths (< 64 chars) could be /tx/txid or other routes - don't validate */
    if (path_len < 64) {
        return 0;
    }

    /* Long paths should be hex transaction data - validate each character */
    for (const unsigned char *p = path_start; p < path_end; p++) {
        /* Allow 'tx/' prefix */
        if (p == path_start && path_len > 3 &&
            p[0] == 't' && p[1] == 'x' && p[2] == '/') {
            p += 2;  /* Skip 'tx/' */
            continue;
        }

        if (!is_hex_char(*p)) {
            log_warn("Invalid character in path from %s: '%c' (0x%02x) at position %zu",
                     log_format_ip(conn->client_ip), *p, (unsigned char)*p, (size_t)(p - path_start));
            conn->validation_failed = true;
            return -1;
        }
    }

    conn->path_validated = true;
    return 0;
}

/*
 * Try to promote connection to higher tier based on buffer size.
 * Returns 0 on success (or no promotion needed), -1 if promotion failed (no slots).
 */
static int try_promote_tier(Connection *conn, size_t new_size)
{
    WorkerProcess *worker = conn->worker;
    Config *cfg = worker->config;

    /* Determine required tier for this size */
    RequestTier required_tier = size_to_tier(new_size, cfg);

    /* Already at or above required tier? */
    if (conn->current_tier >= required_tier) {
        return 0;
    }

    /* Try to promote */
    if (!slot_manager_promote(&worker->slots, conn->current_tier, required_tier)) {
        log_warn("Cannot promote %s from %s to %s tier - no slots available",
                 log_format_ip(conn->client_ip), tier_name(conn->current_tier), tier_name(required_tier));
        return -1;
    }

    log_info("Promoted %s from %s to %s tier (size %zu)",
             log_format_ip(conn->client_ip), tier_name(conn->current_tier), tier_name(required_tier), new_size);
    conn->current_tier = required_tier;
    return 0;
}

/*
 * Downgrade connection from large/huge tier to normal.
 * Called after request is fully received to release expensive slots ASAP.
 * This allows huge slots to be freed before the response is sent.
 */
static void downgrade_tier_to_normal(Connection *conn)
{
    WorkerProcess *worker = conn->worker;

    /* Only downgrade if we're above normal tier */
    if (!conn->slot_held || conn->current_tier == TIER_NORMAL) {
        return;
    }

    /* Release the expensive slot */
    slot_manager_release(&worker->slots, conn->current_tier);

    /* Acquire normal slot for response phase */
    if (slot_manager_acquire(&worker->slots, TIER_NORMAL)) {
        log_debug("Downgraded %s from %s to normal tier (request complete)",
                  log_format_ip(conn->client_ip), tier_name(conn->current_tier));
        conn->current_tier = TIER_NORMAL;
    } else {
        /* Can't get normal slot - keep the expensive one for now */
        if (!slot_manager_acquire(&worker->slots, conn->current_tier)) {
            /* Lost both slots - mark as not held */
            conn->slot_held = false;
        }
    }
}

/*
 * Initialize a connection's fields, set up callbacks, and add to worker list.
 * Shared by connection_new() and connection_new_with_bev().
 * The caller must have already set conn->bev before calling this.
 * Returns 0 on success, -1 on error.
 */
static int connection_init_common(Connection *conn, struct WorkerProcess *worker,
                                  struct sockaddr *addr)
{
    struct timeval read_timeout = {READ_TIMEOUT_SEC, 0};

    conn->worker = worker;
    conn->state = CONN_STATE_READING_HEADERS;
    conn->protocol = PROTO_HTTP_1_1;
    conn->current_tier = TIER_NORMAL;  /* Start in normal tier */
    conn->path = NULL;
    conn->path_len = 0;
    conn->path_validated = false;
    conn->validation_failed = false;
    conn->headers_scanned = 0;
    conn->keep_alive = true;  /* Default to keep-alive for HTTP/1.1 */
    conn->slot_held = true;   /* Slot acquired in accept callback */
    conn->requests_on_connection = 0;
    conn->ssl = NULL;
    conn->tls_handshake_done = false;
    conn->h2 = NULL;
    clock_gettime(CLOCK_MONOTONIC, &conn->start_time);

    /* Slowloris protection - initialize throughput tracking */
    conn->last_progress_time = conn->start_time;
    conn->bytes_at_last_check = 0;

    /* Generate request ID: worker_id-timestamp-counter */
    static uint32_t request_counter = 0;
    snprintf(conn->request_id, sizeof(conn->request_id), "%d-%lx-%x",
             worker->worker_id,
             (unsigned long)(conn->start_time.tv_sec * 1000000 + conn->start_time.tv_nsec / 1000),
             request_counter++);

    /* Initialize response tracking */
    conn->response_status = 0;
    conn->response_bytes = 0;

    /* Extract client IP */
    if (addr->sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &sin->sin_addr, conn->client_ip, sizeof(conn->client_ip));
        conn->client_port = ntohs(sin->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, conn->client_ip, sizeof(conn->client_ip));
        conn->client_port = ntohs(sin6->sin6_port);
    } else {
        strncpy(conn->client_ip, "unknown", sizeof(conn->client_ip));
    }

    /* Set callbacks */
    bufferevent_setcb(conn->bev, conn_read_cb, conn_write_cb, conn_event_cb, conn);

    /* Set read timeout */
    bufferevent_set_timeouts(conn->bev, &read_timeout, NULL);

    /* Enable reading */
    bufferevent_enable(conn->bev, EV_READ);

    /* Add to worker's connection list */
    conn->next = worker->connections;
    conn->prev = NULL;
    if (worker->connections) {
        worker->connections->prev = conn;
    }
    worker->connections = conn;

    return 0;
}

/*
 * Create new connection from accepted socket.
 * v6: No more request_buffer allocation - we use evbuffer directly.
 */
Connection *connection_new(struct WorkerProcess *worker, evutil_socket_t fd,
                           struct sockaddr *addr, int addrlen)
{
    Connection *conn;

    (void)addrlen;

    conn = calloc(1, sizeof(Connection));
    if (!conn) {
        log_error("Failed to allocate connection");
        return NULL;
    }

    /* Create bufferevent - v6: no separate buffer allocation */
    conn->bev = bufferevent_socket_new(worker->base, fd,
                                        BEV_OPT_CLOSE_ON_FREE);
    if (!conn->bev) {
        log_error("Failed to create bufferevent");
        free(conn);
        return NULL;
    }

    if (connection_init_common(conn, worker, addr) < 0) {
        bufferevent_free(conn->bev);
        free(conn);
        return NULL;
    }

    return conn;
}

/*
 * Create new connection from a pre-created bufferevent.
 * Used by TLS accept path where the bufferevent is already created
 * (bufferevent_openssl_socket_new). Ensures TLS connections get
 * identical initialization to plain HTTP, including slot_held = true,
 * keep_alive = true, and request_id generation.
 */
Connection *connection_new_with_bev(struct WorkerProcess *worker,
                                     struct bufferevent *bev,
                                     struct sockaddr *addr, int addrlen)
{
    Connection *conn;

    (void)addrlen;

    conn = calloc(1, sizeof(Connection));
    if (!conn) {
        log_error("Failed to allocate connection");
        return NULL;
    }

    conn->bev = bev;

    if (connection_init_common(conn, worker, addr) < 0) {
        free(conn);
        return NULL;
    }

    return conn;
}

/*
 * Free connection and clean up resources.
 * v6: No more request_buffer to free.
 */
void connection_free(Connection *conn)
{
    if (!conn) return;

    WorkerProcess *worker = conn->worker;

    /* Remove from worker's connection list */
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        worker->connections = conn->next;
    }
    if (conn->next) {
        conn->next->prev = conn->prev;
    }

    /* Free HTTP/2 session */
    if (conn->h2) {
        h2_connection_free(conn->h2);
        conn->h2 = NULL;
    }

    /* Free bufferevent (closes socket, and for TLS also frees SSL) */
    if (conn->bev) {
        bufferevent_free(conn->bev);
    }
    /* Note: SSL is freed by bufferevent_free when using bufferevent_openssl */
    conn->ssl = NULL;

    /* Free dynamic path */
    if (conn->path) {
        free(conn->path);
    }

    /* Release slot at correct tier (only if held) */
    if (conn->slot_held) {
        slot_manager_release(&worker->slots, conn->current_tier);
        conn->slot_held = false;
    }

    /* Update worker stats */
    worker->active_connections--;

    /* Check if we should exit (draining mode) */
    worker_check_drain(worker);

    free(conn);
}

/*
 * Parse the request line and headers from a contiguous buffer.
 * Extracts method and path. Path is dynamically allocated for large URLs.
 * Returns 0 on success, -1 on error.
 */
static int parse_request_headers(Connection *conn, const unsigned char *headers, size_t len)
{
    const unsigned char *line_end;
    const unsigned char *space1, *space2;
    size_t method_len, path_len;

    /* Find end of first line */
    line_end = memmem(headers, len, "\r\n", 2);
    if (!line_end) {
        return -1;
    }

    /* Find method (GET, POST, etc.) */
    space1 = memchr(headers, ' ', line_end - headers);
    if (!space1) {
        return -1;
    }

    method_len = space1 - headers;
    if (method_len >= sizeof(conn->method)) {
        log_warn("HTTP method too long (%zu bytes) from %s", method_len, log_format_ip(conn->client_ip));
        return -1;
    }
    memcpy(conn->method, headers, method_len);
    conn->method[method_len] = '\0';

    /* Find path */
    space2 = memchr(space1 + 1, ' ', line_end - space1 - 1);
    if (!space2) {
        /* HTTP/0.9 style - no version, path goes to end of line */
        path_len = line_end - (space1 + 1);
    } else {
        path_len = space2 - (space1 + 1);
    }

    /* Dynamically allocate path for large URLs */
    conn->path = malloc(path_len + 1);
    if (!conn->path) {
        log_error("Failed to allocate path buffer");
        return -1;
    }

    memcpy(conn->path, space1 + 1, path_len);
    conn->path[path_len] = '\0';
    conn->path_len = path_len;

    /* Parse Content-Length header if present */
    const char *cl = "Content-Length:";
    const unsigned char *found = NULL;
    const unsigned char *headers_end = headers + len;

    /* Case-insensitive search for Content-Length */
    for (size_t i = 0; i + 15 < len; i++) {
        if ((headers[i] == 'C' || headers[i] == 'c') &&
            strncasecmp((const char *)headers + i, cl, 15) == 0) {
            found = headers + i + 15;
            break;
        }
    }

    if (found) {
        /* Skip whitespace - with bounds check to prevent buffer overread */
        while (found < headers_end && (*found == ' ' || *found == '\t')) {
            found++;
        }
        if (found < headers_end) {
            /* Security: reject negative numbers - strtoul("-1") silently
             * converts to ULONG_MAX without setting errno */
            if (*found == '-' || *found == '+') {
                log_warn("Invalid Content-Length (sign prefix) from %s",
                         log_format_ip(conn->client_ip));
                conn->content_length = 0;
            } else {
                /* Security: proper strtoul() with error checking */
                char *endptr = NULL;
                errno = 0;
                unsigned long val = strtoul((const char *)found, &endptr, 10);

                if (errno == ERANGE || endptr == (const char *)found) {
                    log_warn("Invalid Content-Length header from %s",
                             log_format_ip(conn->client_ip));
                    conn->content_length = 0;
                } else {
                    conn->content_length = (size_t)val;
                }
            }
        } else {
            conn->content_length = 0;
        }
    } else {
        conn->content_length = 0;
    }

    /* Parse Connection header for keep-alive */
    const char *conn_header = "Connection:";
    const unsigned char *conn_found = NULL;

    for (size_t i = 0; i + 11 < len; i++) {
        if ((headers[i] == 'C' || headers[i] == 'c') &&
            strncasecmp((const char *)headers + i, conn_header, 11) == 0) {
            conn_found = headers + i + 11;
            break;
        }
    }

    if (conn_found) {
        /* Skip whitespace - with bounds check to prevent buffer overread */
        while (conn_found < headers_end && (*conn_found == ' ' || *conn_found == '\t')) {
            conn_found++;
        }
        /* Check for "close" - ensure enough bytes remain */
        if (conn_found + 5 <= headers_end &&
            strncasecmp((const char *)conn_found, "close", 5) == 0) {
            conn->keep_alive = false;
        } else if (conn_found + 10 <= headers_end &&
                   strncasecmp((const char *)conn_found, "keep-alive", 10) == 0) {
            conn->keep_alive = true;
        }
    }
    /* Default is keep-alive for HTTP/1.1, set in connection_new */

    return 0;
}

/*
 * Serve a static file with TCP_CORK optimization.
 * Cork ensures headers + body go in same TCP segment.
 * Includes Cache-Control header based on config.
 * Supports keep-alive connections.
 */
static void serve_static_file(Connection *conn, StaticFile *file, int status_code,
                              const char *status_text)
{
    struct evbuffer *output = bufferevent_get_output(conn->bev);
    int fd = bufferevent_getfd(conn->bev);
    int cache_max_age = conn->worker->config->cache_max_age;

    conn->state = CONN_STATE_WRITING_RESPONSE;

    /* Cork - accumulate headers + body */
    tcp_cork_enable(fd);

    /* Write HTTP headers */
    evbuffer_add_printf(output,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n",
        status_code, status_text,
        file->content_type, file->length);

    /* Add Cache-Control header if caching is enabled */
    if (cache_max_age > 0) {
        evbuffer_add_printf(output,
            "Cache-Control: public, max-age=%d\r\n",
            cache_max_age);
    } else {
        evbuffer_add_printf(output,
            "Cache-Control: no-store\r\n");
    }

    /* Connection header - support keep-alive */
    if (conn->keep_alive) {
        evbuffer_add_printf(output, "Connection: keep-alive\r\n");
    } else {
        evbuffer_add_printf(output, "Connection: close\r\n");
    }

    /* X-Request-ID header for tracing */
    evbuffer_add_printf(output, "X-Request-ID: %s\r\n", conn->request_id);

    /* End headers */
    evbuffer_add_printf(output, "\r\n");

    /* Write body */
    evbuffer_add(output, file->content, file->length);

    /* Uncork - flush as optimal TCP segments */
    tcp_cork_disable(fd);

    /* Track response for access logging */
    conn->response_status = status_code;
    conn->response_bytes = file->length;

    /* Update state based on keep-alive */
    if (conn->keep_alive) {
        conn->state = CONN_STATE_WRITING_RESPONSE;
    } else {
        conn->state = CONN_STATE_CLOSING;
    }
    bufferevent_enable(conn->bev, EV_WRITE);
}

/*
 * Serve /health endpoint - detailed health check with JSON response.
 */
static void serve_health(Connection *conn)
{
    WorkerProcess *worker = conn->worker;
    struct evbuffer *output = bufferevent_get_output(conn->bev);
    char body[1024];

    conn->state = CONN_STATE_WRITING_RESPONSE;

    int body_len = generate_health_body(worker, body, sizeof(body));

    evbuffer_add_printf(output,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "X-Request-ID: %s\r\n"
        "Connection: %s\r\n"
        "\r\n%s",
        body_len, conn->request_id,
        conn->keep_alive ? "keep-alive" : "close", body);

    conn->response_status = 200;
    conn->response_bytes = body_len;
    conn->state = conn->keep_alive ? CONN_STATE_WRITING_RESPONSE : CONN_STATE_CLOSING;
    bufferevent_enable(conn->bev, EV_WRITE);
}

/*
 * Serve /ready endpoint - readiness probe.
 * Returns 200 if accepting connections, 503 if draining.
 */
static void serve_ready(Connection *conn)
{
    WorkerProcess *worker = conn->worker;
    struct evbuffer *output = bufferevent_get_output(conn->bev);
    int status = worker->draining ? 503 : 200;
    const char *status_text = worker->draining ? "Service Unavailable" : "OK";

    conn->state = CONN_STATE_WRITING_RESPONSE;

    evbuffer_add_printf(output,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 0\r\n"
        "Cache-Control: no-store\r\n"
        "X-Request-ID: %s\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status, status_text, conn->request_id,
        conn->keep_alive ? "keep-alive" : "close");

    conn->response_status = status;
    conn->response_bytes = 0;
    conn->state = conn->keep_alive ? CONN_STATE_WRITING_RESPONSE : CONN_STATE_CLOSING;
    bufferevent_enable(conn->bev, EV_WRITE);
}


/*
* Serve /version endpoint - returns version info in JSON.
* Returns 200 if healthy.
*/
static void serve_version(Connection *conn)
{
    struct evbuffer *output = bufferevent_get_output(conn->bev);
    char body[256];
    int body_len = snprintf(body, sizeof(body), "{\"version\":\"0.1.0\"}");

    conn->state = CONN_STATE_WRITING_RESPONSE;

    evbuffer_add_printf(output,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "X-Request-ID: %s\r\n"
        "Connection: %s\r\n"
        "\r\n%s",
        body_len, conn->request_id,
        conn->keep_alive ? "keep-alive" : "close", body);

    conn->response_status = 200;
    conn->response_bytes = body_len;
    conn->state = conn->keep_alive ? CONN_STATE_WRITING_RESPONSE : CONN_STATE_CLOSING;
    bufferevent_enable(conn->bev, EV_WRITE);
}

/*
 * Serve /alive endpoint - liveness probe.
 * Always returns 200 if the process is running.
 */
static void serve_alive(Connection *conn)
{
    struct evbuffer *output = bufferevent_get_output(conn->bev);

    conn->state = CONN_STATE_WRITING_RESPONSE;

    evbuffer_add_printf(output,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 0\r\n"
        "Cache-Control: no-store\r\n"
        "X-Request-ID: %s\r\n"
        "Connection: %s\r\n"
        "\r\n",
        conn->request_id,
        conn->keep_alive ? "keep-alive" : "close");

    conn->response_status = 200;
    conn->response_bytes = 0;
    conn->state = conn->keep_alive ? CONN_STATE_WRITING_RESPONSE : CONN_STATE_CLOSING;
    bufferevent_enable(conn->bev, EV_WRITE);
}

/*
 * Serve ACME HTTP-01 challenge response.
 * Path format: /.well-known/acme-challenge/{token}
 *
 * Security: Only serves files from the configured acme_challenge_dir.
 * Rejects any path traversal attempts (../).
 */
static void serve_acme_challenge(Connection *conn)
{
    WorkerProcess *worker = conn->worker;
    struct evbuffer *output = bufferevent_get_output(conn->bev);
    const char *acme_dir = worker->config->acme_challenge_dir;

    conn->state = CONN_STATE_WRITING_RESPONSE;

    /* Security: Reject if ACME challenge directory is not configured */
    if (!acme_dir || acme_dir[0] == '\0') {
        log_warn("ACME: Challenge directory not configured");
        goto not_found;
    }

    /* Extract token from path: /.well-known/acme-challenge/{token} */
    const size_t prefix_len = 27;  /* ".well-known/acme-challenge/" */

    if (conn->path_len < prefix_len + 2 || conn->path[0] != '/') {
        log_warn("ACME: Invalid path format from %s", log_format_ip(conn->client_ip));
        goto not_found;
    }

    const char *token = conn->path + 1 + prefix_len;
    size_t token_len = conn->path_len - 1 - prefix_len;

    /* Security: Reject path traversal attempts */
    if (strstr(token, "..") || strchr(token, '/') || strchr(token, '\\')) {
        log_warn("ACME: Path traversal attempt from %s: %s",
                 log_format_ip(conn->client_ip), conn->path);
        goto not_found;
    }

    /* Security: Token should be base64url encoded (alphanumeric, -, _) */
    for (size_t i = 0; i < token_len; i++) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            log_warn("ACME: Invalid token character from %s: '%c'",
                     log_format_ip(conn->client_ip), c);
            goto not_found;
        }
    }

    /* Build full path to challenge file */
    char filepath[512];
    int n = snprintf(filepath, sizeof(filepath), "%s/%.*s",
                     acme_dir, (int)token_len, token);
    if (n < 0 || (size_t)n >= sizeof(filepath)) {
        log_warn("ACME: Path too long from %s", log_format_ip(conn->client_ip));
        goto not_found;
    }

    /* Open and read the challenge file */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        log_warn("ACME: Challenge file not found: %s (from %s)",
                 filepath, log_format_ip(conn->client_ip));
        goto not_found;
    }

    /* Get file size */
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        log_warn("ACME: Cannot stat challenge file: %s", filepath);
        close(fd);
        goto not_found;
    }

    /* Sanity check - ACME tokens are typically < 256 bytes */
    if (st.st_size > 4096) {
        log_warn("ACME: Challenge file too large: %s (%ld bytes)",
                 filepath, (long)st.st_size);
        close(fd);
        goto not_found;
    }

    /* Read file content */
    char content[4097];
    ssize_t bytes_read = read(fd, content, st.st_size);
    close(fd);

    if (bytes_read < 0 || bytes_read != st.st_size) {
        log_warn("ACME: Failed to read challenge file: %s", filepath);
        goto not_found;
    }

    log_info("ACME: Serving challenge for token %.*s to %s",
             (int)token_len, token, log_format_ip(conn->client_ip));

    /* Send response - ACME expects text/plain */
    evbuffer_add_printf(output,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zd\r\n"
        "Cache-Control: no-store\r\n"
        "X-Request-ID: %s\r\n"
        "Connection: %s\r\n"
        "\r\n",
        bytes_read, conn->request_id,
        conn->keep_alive ? "keep-alive" : "close");
    evbuffer_add(output, content, bytes_read);

    conn->response_status = 200;
    conn->response_bytes = bytes_read;
    conn->state = conn->keep_alive ? CONN_STATE_WRITING_RESPONSE : CONN_STATE_CLOSING;
    bufferevent_enable(conn->bev, EV_WRITE);
    return;

not_found:
    evbuffer_add_printf(output,
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 9\r\n"
        "Cache-Control: no-store\r\n"
        "X-Request-ID: %s\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Not Found",
        conn->request_id);

    conn->response_status = 404;
    conn->response_bytes = 9;
    conn->state = CONN_STATE_CLOSING;
    bufferevent_enable(conn->bev, EV_WRITE);
}

/*
 * Serve /metrics endpoint - Prometheus text exposition format.
 */
static void serve_metrics(Connection *conn)
{
    WorkerProcess *worker = conn->worker;
    struct evbuffer *output = bufferevent_get_output(conn->bev);
    char body[16384];

    conn->state = CONN_STATE_WRITING_RESPONSE;

    int body_len = generate_metrics_body(worker, body, sizeof(body));

    evbuffer_add_printf(output,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "X-Request-ID: %s\r\n"
        "Connection: close\r\n"
        "\r\n%s",
        body_len, conn->request_id, body);

    conn->response_status = 200;
    conn->response_bytes = body_len;
    conn->state = CONN_STATE_CLOSING;
    bufferevent_enable(conn->bev, EV_WRITE);
}

/*
 * Process complete request and generate response.
 */
static void process_request(Connection *conn)
{
    WorkerProcess *worker = conn->worker;
    StaticFile *file;
    int status_code = 200;
    const char *status_text = "OK";

    conn->state = CONN_STATE_PROCESSING;

    /* Route the request based on path */
    RouteType route = route_request(conn->path, conn->path_len);
    update_endpoint_counter(worker, route);

    /* Handle observability endpoints */
    switch (route) {
        case ROUTE_HEALTH:
            serve_health(conn);
            return;
        case ROUTE_READY:
            serve_ready(conn);
            return;
        case ROUTE_VERSION:
            serve_version(conn);
            return;
        case ROUTE_ALIVE:
            serve_alive(conn);
            return;
        case ROUTE_METRICS:
            serve_metrics(conn);
            return;
        case ROUTE_ACME_CHALLENGE:
            serve_acme_challenge(conn);
            return;
        default:
            break;
    }

    /* Select file based on route */
    switch (route) {
        case ROUTE_HOME:
            file = &worker->static_files.index;
            break;
        case ROUTE_BROADCAST:
            file = &worker->static_files.broadcast;
            break;
        case ROUTE_RESULT:
            file = &worker->static_files.result;
            break;
        case ROUTE_DOCS:
            file = &worker->static_files.docs;
            break;
        case ROUTE_STATUS:
            file = &worker->static_files.status;
            break;
        case ROUTE_LOGOS:
            file = &worker->static_files.logos;
            break;
        case ROUTE_ERROR:
        default:
            file = &worker->static_files.error;
            status_code = 404;
            status_text = "Not Found";
            break;
    }

    /* Serve the static file */
    serve_static_file(conn, file, status_code, status_text);
}

/*
 * Read callback - called when data is available.
 * v6: Uses evbuffer_search/pullup/drain - no redundant copying!
 *
 * Key changes from v5:
 * - No malloc/realloc of request_buffer
 * - Search for \r\n\r\n directly in evbuffer
 * - Use evbuffer_pullup for contiguous view only when parsing
 * - Drain consumed data after processing
 * - Branch on protocol for HTTP/2 handling
 */
static void conn_read_cb(struct bufferevent *bev, void *ctx)
{
    Connection *conn = ctx;
    WorkerProcess *worker = conn->worker;
    Config *cfg = worker->config;
    struct evbuffer *input = bufferevent_get_input(bev);
    size_t available = evbuffer_get_length(input);

    /* Check for HTTP/2 after TLS handshake */
    if (conn->ssl && !conn->tls_handshake_done) {
        SSL *ssl = (SSL *)conn->ssl;
        if (SSL_is_init_finished(ssl)) {
            conn->tls_handshake_done = true;

            /* Track TLS handshake metrics */
            worker->tls_handshakes_total++;
            int tls_version = SSL_version(ssl);
            if (tls_version == TLS1_3_VERSION) {
                worker->tls_protocol_tls13++;
            } else if (tls_version == TLS1_2_VERSION) {
                worker->tls_protocol_tls12++;
            }

            /* Check ALPN result */
            if (tls_is_http2(ssl)) {
                log_debug("HTTP/2 negotiated via ALPN for %s", log_format_ip(conn->client_ip));

                /* Initialize HTTP/2 session */
                if (h2_connection_init(conn) < 0) {
                    log_error("Failed to initialize HTTP/2 for %s", log_format_ip(conn->client_ip));
                    connection_free(conn);
                    return;
                }

                /* Send HTTP/2 connection preface */
                if (h2_send_pending(conn) < 0) {
                    connection_free(conn);
                    return;
                }
            }
        }
    }

    /* Slowloris protection - applies to ALL protocols (HTTP/1.1 and HTTP/2) */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Check 1: Maximum total connection time */
    double total_elapsed = (now.tv_sec - conn->start_time.tv_sec) +
                           (now.tv_nsec - conn->start_time.tv_nsec) / 1e9;
    if (total_elapsed > MAX_REQUEST_TIME_SEC) {
        log_warn("Slowloris: Connection exceeded max time (%.1fs) from %s [%s]",
                 total_elapsed, log_format_ip(conn->client_ip),
                 conn->protocol == PROTO_HTTP_2 ? "HTTP/2" : "HTTP/1.1");
        worker->slowloris_kills++;
        connection_free(conn);
        return;
    }

    /* Check 2: Minimum throughput (every THROUGHPUT_CHECK_INTERVAL_SEC) */
    double check_elapsed = (now.tv_sec - conn->last_progress_time.tv_sec) +
                           (now.tv_nsec - conn->last_progress_time.tv_nsec) / 1e9;
    if (check_elapsed >= THROUGHPUT_CHECK_INTERVAL_SEC) {
        size_t bytes_this_period = available - conn->bytes_at_last_check;
        if (available < conn->bytes_at_last_check ||
            bytes_this_period < MIN_BYTES_PER_CHECK) {
            log_warn("Slowloris: Throughput too low (%zu bytes in %.1fs) from %s [%s]",
                     bytes_this_period, check_elapsed, log_format_ip(conn->client_ip),
                     conn->protocol == PROTO_HTTP_2 ? "HTTP/2" : "HTTP/1.1");
            worker->slowloris_kills++;
            connection_free(conn);
            return;
        }
        /* Reset check window */
        conn->last_progress_time = now;
        conn->bytes_at_last_check = available;
    }

    /* Handle HTTP/2 connections */
    if (conn->protocol == PROTO_HTTP_2 && conn->h2) {
        /* Re-read available in case it changed during HTTP/2 init */
        available = evbuffer_get_length(input);
        if (available > 0) {
            unsigned char *data = evbuffer_pullup(input, available);
            if (data) {
                if (h2_process_input(conn, data, available) < 0) {
                    connection_free(conn);
                    return;
                }
                evbuffer_drain(input, available);
                /* Reset throughput tracking after drain to prevent false slowloris kills.
                 * Without this, bytes_at_last_check holds the pre-drain value,
                 * causing available < bytes_at_last_check on the next read. */
                conn->bytes_at_last_check = 0;
            }
        }
        return;
    }

    /* HTTP/1.1 handling continues below */
    /* Note: Slowloris checks already done above for all protocols */

    if (conn->state == CONN_STATE_READING_HEADERS) {
        /* Check against configured max buffer size */
        if (available > cfg->max_buffer_size) {
            log_warn("Request exceeds max buffer size (%zu bytes) from %s",
                     cfg->max_buffer_size, log_format_ip(conn->client_ip));
            connection_send_error(conn, 413, "Request Entity Too Large");
            return;
        }

        /* Try to promote tier if needed BEFORE processing more data */
        if (try_promote_tier(conn, available) < 0) {
            /* No slots available in higher tier - reject request */
            worker->slot_promotion_failures++;
            connection_send_error(conn, 503, "Service Unavailable");
            return;
        }

        /* Search for end of headers (\r\n\r\n) WITHOUT copying
         * Start search from where we left off (headers_scanned) */
        struct evbuffer_ptr start_ptr;
        if (conn->headers_scanned > 0) {
            evbuffer_ptr_set(input, &start_ptr, conn->headers_scanned, EVBUFFER_PTR_SET);
        } else {
            evbuffer_ptr_set(input, &start_ptr, 0, EVBUFFER_PTR_SET);
        }

        struct evbuffer_ptr found = evbuffer_search(input, "\r\n\r\n", 4, &start_ptr);

        if (found.pos < 0) {
            /* Not found yet - remember how much we've scanned */
            conn->headers_scanned = available > 3 ? available - 3 : 0;

            /* Early validation - pullup first line to validate path */
            if (available > 0) {
                unsigned char *data = evbuffer_pullup(input, available);
                if (data && validate_path_early(conn, data, available) < 0) {
                    worker->errors_parse++;
                    connection_send_error(conn, 400, "Bad Request - Invalid Characters");
                    return;
                }
            }
            return;  /* Wait for more data */
        }

        /* Found \r\n\r\n - headers are complete! */
        size_t headers_len = found.pos + 4;  /* Include the \r\n\r\n */
        conn->headers_end = headers_len;

        /* Get contiguous view of headers (single pullup, zero-copy if already contiguous) */
        unsigned char *headers = evbuffer_pullup(input, headers_len);
        if (!headers) {
            log_error("Failed to pullup headers");
            connection_send_error(conn, 500, "Internal Server Error");
            return;
        }

        /* Final validation of complete headers */
        if (validate_path_early(conn, headers, headers_len) < 0) {
            worker->errors_parse++;
            connection_send_error(conn, 400, "Bad Request - Invalid Characters");
            return;
        }

        /* Parse request line and headers */
        if (parse_request_headers(conn, headers, headers_len) < 0) {
            worker->errors_parse++;
            connection_send_error(conn, 400, "Bad Request");
            return;
        }

        /* Security: Check Content-Length against max_buffer_size */
        if (conn->content_length > cfg->max_buffer_size) {
            log_warn("Content-Length %zu exceeds max_buffer_size %zu from %s",
                     conn->content_length, cfg->max_buffer_size,
                     log_format_ip(conn->client_ip));
            connection_send_error(conn, 413, "Payload Too Large");
            return;
        }

        /* Drain the headers from evbuffer */
        evbuffer_drain(input, headers_len);

        /* Check for body */
        size_t remaining = evbuffer_get_length(input);
        conn->body_received = remaining;

        if (conn->content_length > 0 && conn->body_received < conn->content_length) {
            /* Need to read more body */
            conn->state = CONN_STATE_READING_BODY;

            /* Drain any body data already received */
            if (remaining > 0) {
                evbuffer_drain(input, remaining);
            }
        } else {
            /* Complete request - process it */
            if (remaining > 0) {
                /* Note: HTTP pipelining not supported - extra data is discarded.
                 * This could be a pipelined request or trailing garbage. */
                log_debug("Discarding %zu bytes after request from %s (pipelining unsupported)",
                          remaining, log_format_ip(conn->client_ip));
                evbuffer_drain(input, remaining);
            }
            /* Release large/huge slot ASAP - only needed for receiving */
            downgrade_tier_to_normal(conn);
            process_request(conn);
        }
    } else if (conn->state == CONN_STATE_READING_BODY) {
        /* Read body data */
        size_t remaining = conn->content_length - conn->body_received;
        size_t to_drain = (available < remaining) ? available : remaining;

        /* For now, just drain the body - we don't process it yet */
        evbuffer_drain(input, to_drain);
        conn->body_received += to_drain;

        if (conn->body_received >= conn->content_length) {
            /* Body complete - process request */
            /* Release large/huge slot ASAP - only needed for receiving */
            downgrade_tier_to_normal(conn);
            process_request(conn);
        }
    }
}

/*
 * Reset connection state for next keep-alive request.
 */
static void connection_reset_for_keepalive(Connection *conn)
{
    /* Free path from previous request */
    if (conn->path) {
        free(conn->path);
        conn->path = NULL;
    }
    conn->path_len = 0;

    /* Reset parsing state */
    conn->headers_scanned = 0;
    conn->headers_end = 0;
    conn->content_length = 0;
    conn->body_received = 0;
    conn->method[0] = '\0';
    conn->path_validated = false;
    conn->validation_failed = false;

    /* Release current tier slot and reset to normal */
    if (conn->slot_held && conn->current_tier != TIER_NORMAL) {
        slot_manager_release(&conn->worker->slots, conn->current_tier);
        conn->current_tier = TIER_NORMAL;
        /* Re-acquire normal slot for next request */
        if (!slot_manager_acquire(&conn->worker->slots, TIER_NORMAL)) {
            /* No slot available - close connection */
            conn->keep_alive = false;
            conn->slot_held = false;
            return;
        }
    }

    /* Reset response tracking for next request */
    conn->response_status = 0;
    conn->response_bytes = 0;

    /* Reset timing for next request */
    clock_gettime(CLOCK_MONOTONIC, &conn->start_time);
    conn->last_progress_time = conn->start_time;
    conn->bytes_at_last_check = 0;

    /* Generate new request ID for next request */
    static uint32_t keepalive_counter = 0;
    snprintf(conn->request_id, sizeof(conn->request_id), "%d-%lx-%x",
             conn->worker->worker_id,
             (unsigned long)(conn->start_time.tv_sec * 1000000 + conn->start_time.tv_nsec / 1000),
             keepalive_counter++);

    /* Ready for next request */
    conn->state = CONN_STATE_READING_HEADERS;
    conn->requests_on_connection++;
}

/*
 * Log access entry for completed request.
 * Uses shared counter/logging functions from endpoints.c.
 */
static void log_request_complete(Connection *conn)
{
    if (conn->response_status == 0) {
        return;  /* No response was sent */
    }

    WorkerProcess *worker = conn->worker;

    /* Calculate duration */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double duration_ms = (now.tv_sec - conn->start_time.tv_sec) * 1000.0 +
                         (now.tv_nsec - conn->start_time.tv_nsec) / 1e6;
    double duration_sec = duration_ms / 1000.0;

    /* Update metrics */
    worker->requests_processed++;
    update_latency_histogram(worker, duration_sec);
    update_status_counters(worker, conn->response_status);
    update_method_counters(worker, conn->method);
    worker->response_bytes_total += conn->response_bytes;
    if (conn->requests_on_connection > 0) {
        worker->keepalive_reuses++;
    }

    /* Log access */
    log_access(conn->client_ip,
               conn->method[0] ? conn->method : "???",
               conn->path ? conn->path : "/",
               conn->response_status,
               conn->response_bytes,
               duration_ms,
               conn->request_id);
}

/*
 * Write callback - called when output buffer is drained.
 */
static void conn_write_cb(struct bufferevent *bev, void *ctx)
{
    Connection *conn = ctx;
    struct evbuffer *output = bufferevent_get_output(bev);

    /* Check if output is fully drained */
    if (evbuffer_get_length(output) > 0) {
        return;  /* Still writing */
    }

    /* Log access when response is fully written */
    log_request_complete(conn);

    if (conn->state == CONN_STATE_CLOSING) {
        /* Response fully written, close connection */
        connection_free(conn);
        return;
    }

    if (conn->state == CONN_STATE_WRITING_RESPONSE && conn->keep_alive) {
        /* Keep-alive: reset for next request */
        connection_reset_for_keepalive(conn);
        if (!conn->slot_held) {
            /* Failed to get slot for next request */
            connection_free(conn);
            return;
        }
        bufferevent_enable(bev, EV_READ);
    }
}

/*
 * Event callback - handles errors and EOF.
 */
static void conn_event_cb(struct bufferevent *bev, short events, void *ctx)
{
    Connection *conn = ctx;
    WorkerProcess *worker = conn->worker;
    (void)bev;

    if (events & BEV_EVENT_CONNECTED) {
        /* TLS handshake completed - don't free, continue processing */
        return;
    }

    if (events & BEV_EVENT_TIMEOUT) {
        log_warn("Connection timeout from %s", log_format_ip(conn->client_ip));
        worker->errors_timeout++;
    } else if (events & BEV_EVENT_ERROR) {
        int err = EVUTIL_SOCKET_ERROR();
        if (err != 0) {
            log_warn("Connection error from %s: %s",
                     log_format_ip(conn->client_ip), evutil_socket_error_to_string(err));
        }
        /* Log SSL errors if this is a TLS connection */
        if (conn->ssl) {
            unsigned long ssl_err;
            bool had_tls_error = false;
            while ((ssl_err = ERR_get_error()) != 0) {
                char buf[256];
                ERR_error_string_n(ssl_err, buf, sizeof(buf));
                log_warn("SSL error: %s", buf);
                had_tls_error = true;
            }
            if (had_tls_error) {
                worker->tls_handshake_errors++;
                worker->errors_tls++;
            }
        }
    }
    /* BEV_EVENT_EOF is normal - client closed connection */

    connection_free(conn);
}

/*
 * Send HTTP response.
 * Respects keep-alive setting - connection stays open if keep_alive is true.
 */
void connection_send_response(Connection *conn, int status_code,
                              const char *status_text, const char *body)
{
    struct evbuffer *output = bufferevent_get_output(conn->bev);
    size_t body_len = body ? strlen(body) : 0;

    conn->state = CONN_STATE_WRITING_RESPONSE;

    /* Write headers - respect keep-alive setting */
    evbuffer_add_printf(output,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "X-Request-ID: %s\r\n"
        "\r\n",
        status_code, status_text, body_len,
        conn->keep_alive ? "keep-alive" : "close",
        conn->request_id);

    /* Write body */
    if (body && body_len > 0) {
        evbuffer_add(output, body, body_len);
    }

    /* Track response for access logging */
    conn->response_status = status_code;
    conn->response_bytes = body_len;

    /* Set state based on keep-alive */
    if (conn->keep_alive) {
        conn->state = CONN_STATE_WRITING_RESPONSE;
    } else {
        conn->state = CONN_STATE_CLOSING;
    }
    bufferevent_enable(conn->bev, EV_WRITE);
}

/*
 * Send error response.
 */
void connection_send_error(Connection *conn, int status_code,
                           const char *status_text)
{
    /* Force close on errors - don't keep broken connections alive */
    conn->keep_alive = false;

    char body[256];
    snprintf(body, sizeof(body), "Error %d: %s\n", status_code, status_text);
    connection_send_response(conn, status_code, status_text, body);
}

