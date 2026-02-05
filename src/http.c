/*
 * DEPRECATED: This file contains unused legacy functions.
 * Production code uses connection.c's built-in response handling instead.
 * Safe to delete this file and http.h, or keep for reference/testing.
 */

#include "http.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

const char *http_status_text(int status)
{
    switch (status) {
        case 200: return "OK";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

/*
 * Check if string is a valid HTTP method.
 */
static int is_valid_method(const char *method)
{
    static const char *valid_methods[] = {
        "GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS", "PATCH", "CONNECT", "TRACE"
    };

    for (size_t i = 0; i < sizeof(valid_methods) / sizeof(valid_methods[0]); i++) {
        if (strcmp(method, valid_methods[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Check if string looks like valid HTTP version.
 */
static int is_valid_version(const char *version)
{
    /* Accept HTTP/1.0 or HTTP/1.1 */
    if (strncmp(version, "HTTP/", 5) != 0) {
        return 0;
    }

    /* Check for 1.0 or 1.1 */
    const char *ver_num = version + 5;
    if (strcmp(ver_num, "1.0") == 0 || strcmp(ver_num, "1.1") == 0) {
        return 1;
    }

    return 0;
}

ParseResult http_parse_request_line(Buffer *buf, HttpRequest *req)
{
    char *line_end;
    char *p;
    char *method_end;
    char *path_start;
    char *path_end;
    char *version_start;
    size_t method_len;
    size_t version_len;

    if (!buf || !req || !buf->data) {
        return PARSE_INVALID;
    }

    /* Find end of line */
    line_end = NULL;
    for (size_t i = 0; i < buf->len; i++) {
        if (buf->data[i] == '\n') {
            line_end = buf->data + i;
            break;
        }
    }

    if (!line_end) {
        return PARSE_INCOMPLETE;
    }

    /* Initialize request structure */
    memset(req, 0, sizeof(HttpRequest));

    /* Parse method (first token) */
    p = buf->data;

    /* Skip leading whitespace */
    while (p < line_end && isspace((unsigned char)*p)) {
        p++;
    }

    if (p >= line_end) {
        return PARSE_INVALID;
    }

    /* Find end of method */
    method_end = p;
    while (method_end < line_end && !isspace((unsigned char)*method_end)) {
        method_end++;
    }

    method_len = method_end - p;
    if (method_len == 0 || method_len >= sizeof(req->method)) {
        return PARSE_INVALID;
    }

    memcpy(req->method, p, method_len);
    req->method[method_len] = '\0';

    if (!is_valid_method(req->method)) {
        return PARSE_INVALID;
    }

    /* Skip whitespace after method */
    p = method_end;
    while (p < line_end && isspace((unsigned char)*p)) {
        p++;
    }

    if (p >= line_end) {
        return PARSE_INVALID;
    }

    /* Parse path */
    path_start = p;
    path_end = p;

    /* Find end of path (next whitespace or end of line) */
    while (path_end < line_end && !isspace((unsigned char)*path_end)) {
        path_end++;
    }

    if (path_end == path_start) {
        return PARSE_INVALID;
    }

    req->path = path_start;
    req->path_len = path_end - path_start;

    /* Skip whitespace after path */
    p = path_end;
    while (p < line_end && isspace((unsigned char)*p)) {
        p++;
    }

    if (p >= line_end) {
        return PARSE_INVALID;
    }

    /* Parse version */
    version_start = p;

    /* Find end of version (before \r or \n) */
    while (p < line_end && *p != '\r' && *p != '\n') {
        p++;
    }

    /* Trim trailing whitespace from version */
    while (p > version_start && isspace((unsigned char)*(p - 1))) {
        p--;
    }

    version_len = p - version_start;
    if (version_len == 0 || version_len >= sizeof(req->version)) {
        return PARSE_INVALID;
    }

    memcpy(req->version, version_start, version_len);
    req->version[version_len] = '\0';

    if (!is_valid_version(req->version)) {
        return PARSE_INVALID;
    }

    return PARSE_OK;
}

void http_send_response(int fd, int status, const char *body)
{
    char header[512];
    size_t body_len = body ? strlen(body) : 0;
    int header_len;
    ssize_t ret;

    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, http_status_text(status), body_len);

    if (header_len > 0 && header_len < (int)sizeof(header)) {
        ret = write(fd, header, header_len);
        (void)ret; /* Best effort - ignore errors for now */
    }

    if (body && body_len > 0) {
        ret = write(fd, body, body_len);
        (void)ret; /* Best effort - ignore errors for now */
    }
}

void http_send_redirect(int fd, const char *location)
{
    char response[1024];
    int len;
    ssize_t ret;

    len = snprintf(response, sizeof(response),
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n",
        location ? location : "/");

    if (len > 0 && len < (int)sizeof(response)) {
        ret = write(fd, response, len);
        (void)ret; /* Best effort - ignore errors for now */
    }
}
