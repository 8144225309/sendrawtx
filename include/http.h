#ifndef HTTP_H
#define HTTP_H

#include "buffer.h"
#include <stddef.h>

typedef struct {
    char method[16];      /* GET, POST, etc. */
    char *path;           /* Pointer into buffer */
    size_t path_len;
    char version[16];     /* HTTP/1.1 */
} HttpRequest;

typedef enum {
    PARSE_OK = 0,
    PARSE_INCOMPLETE = -1,
    PARSE_INVALID = -2
} ParseResult;

/*
 * Parse HTTP request line from buffer.
 * Does not allocate - points into existing buffer.
 *
 * Expected format: "METHOD /path HTTP/version\r\n"
 * or: "METHOD /path HTTP/version\n"
 *
 * Returns:
 *   PARSE_OK on success
 *   PARSE_INCOMPLETE if no complete line found
 *   PARSE_INVALID if line is malformed
 */
ParseResult http_parse_request_line(Buffer *buf, HttpRequest *req);

/*
 * Send an HTTP response.
 * Status code determines response line.
 * Body can be NULL for empty response.
 */
void http_send_response(int fd, int status, const char *body);

/*
 * Send an HTTP redirect response.
 */
void http_send_redirect(int fd, const char *location);

/*
 * Get status text for HTTP status code.
 */
const char *http_status_text(int status);

#endif /* HTTP_H */
