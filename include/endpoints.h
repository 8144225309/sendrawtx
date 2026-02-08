#ifndef ENDPOINTS_H
#define ENDPOINTS_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
struct WorkerProcess;
struct Connection;

/*
 * Generate /health JSON response body.
 * Writes into caller-provided buffer.
 * Returns number of bytes written (excluding NUL), or -1 on error.
 */
int generate_health_body(struct WorkerProcess *worker, char *buf, size_t bufsize);

/*
 * Generate /metrics Prometheus response body.
 * Writes into caller-provided buffer.
 * Returns number of bytes written (excluding NUL), or -1 on error.
 */
int generate_metrics_body(struct WorkerProcess *worker, char *buf, size_t bufsize);

/*
 * Serve ACME HTTP-01 challenge for HTTP/2.
 * Reads the challenge file and sends the response via h2_send_response().
 * The path must start with "/.well-known/acme-challenge/".
 *
 * @param conn       Connection (for h2_send_response and client_ip logging)
 * @param stream_id  HTTP/2 stream ID
 * @param path       Full request path (e.g., "/.well-known/acme-challenge/token")
 * @param path_len   Length of path
 * @param request_id Request ID for X-Request-ID header
 * @return body length on success (>0), -1 on error (404 sent)
 */
int serve_acme_challenge_h2(struct Connection *conn, int32_t stream_id,
                            const char *path, size_t path_len,
                            const char *request_id);

/*
 * Validate hex characters in a path buffer.
 * For paths > 64 chars that look like transaction hex, validates each character.
 * Allows "tx/" prefix.
 *
 * @param path     Path content after leading slash (e.g., "abcdef..." or "tx/abcdef...")
 * @param path_len Length of path content
 * @return 0 if valid (or too short to validate), -1 if invalid hex found
 */
int validate_hex_path(const char *path, size_t path_len);

/*
 * Update latency histogram bucket based on duration in seconds.
 */
void update_latency_histogram(struct WorkerProcess *worker, double duration_sec);

/*
 * Update HTTP status code counters.
 */
void update_status_counters(struct WorkerProcess *worker, int status);

/*
 * Update HTTP method counters.
 */
void update_method_counters(struct WorkerProcess *worker, const char *method);

/*
 * Log access entry for a completed request.
 */
void log_request_access(const char *client_ip, const char *method,
                        const char *path, int status, size_t bytes_sent,
                        double duration_ms, const char *request_id);

#endif /* ENDPOINTS_H */
