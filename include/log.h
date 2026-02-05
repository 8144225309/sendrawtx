#ifndef LOG_H
#define LOG_H

#include <stddef.h>  /* for size_t */

/*
 * Simple logging utilities.
 * PRIVACY: Never log IP addresses.
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
 * Set access logging mode.
 * enabled: 1 = log access entries, 0 = don't log
 */
void log_set_access_enabled(int enabled);

#endif /* LOG_H */
