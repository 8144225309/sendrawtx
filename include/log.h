#ifndef LOG_H
#define LOG_H

#include <stddef.h>  /* for size_t */

/*
 * Simple logging utilities.
 * Default mode: minimal logging with IPs hidden.
 * Verbose mode: full logging with IPs shown (for debugging).
 */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3
} LogLevel;

/*
 * Initialize logging system.
 * level: minimum level to log
 */
void log_init(LogLevel level);

/*
 * Set log level at runtime.
 */
void log_set_level(LogLevel level);

/*
 * Set JSON logging mode.
 * json_mode: 1 = JSON format, 0 = text format
 */
void log_set_json_mode(int json_mode);

/*
 * Log functions - printf-style format.
 */
void log_debug(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

/*
 * Get current process identity for log prefix.
 * Returns "master" or "worker[N]"
 */
void log_set_identity(const char *identity);

/*
 * Log an HTTP access entry.
 * Outputs in Combined Log Format (text mode) or JSON (json mode).
 *
 * @param client_ip     Client IP address
 * @param method        HTTP method (GET, POST, etc.)
 * @param path          Request path
 * @param status        HTTP status code
 * @param bytes_sent    Response body size
 * @param duration_ms   Request duration in milliseconds
 * @param request_id    Unique request ID
 */
void log_access(const char *client_ip, const char *method, const char *path,
                int status, size_t bytes_sent, double duration_ms,
                const char *request_id);

/*
 * Set verbose mode.
 * When verbose=1: access logging enabled, log level DEBUG, full IPs shown.
 * When verbose=0: access logging disabled, log level INFO, IPs anonymized.
 */
void log_set_verbose(int verbose);

/*
 * Check if verbose mode is enabled.
 * Returns: 1 if verbose, 0 if minimal
 */
int log_is_verbose(void);

/*
 * Format an IP address for logging.
 * In verbose mode: returns full IP.
 * In minimal mode: returns anonymized IP (e.g., "192.x.x.x").
 * Note: Returns pointer to static buffer - not thread-safe across calls.
 */
const char *log_format_ip(const char *ip);

#endif /* LOG_H */
