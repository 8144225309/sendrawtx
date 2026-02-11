#include "router.h"
#include "hex.h"
#include <string.h>

/* Minimum raw transaction hex length (82 bytes = 164 chars) */
#define MIN_TX_HEX_LENGTH 164

/* Transaction ID length (32 bytes = 64 chars) */
#define TXID_HEX_LENGTH 64

RouteType route_request(const char *path, size_t path_len)
{
    /* Skip leading slash */
    if (path_len == 0 || path[0] != '/') {
        return ROUTE_ERROR;
    }

    const char *content = path + 1;
    size_t content_len = path_len - 1;

    /* Empty path after slash = home page */
    if (content_len == 0) {
        return ROUTE_HOME;
    }

    /* Check for observability endpoints first */
    if (content_len == 6 && strncmp(content, "health", 6) == 0) {
        return ROUTE_HEALTH;
    }
    if (content_len == 5 && strncmp(content, "ready", 5) == 0) {
        return ROUTE_READY;
    }
    if (content_len == 7 && strncmp(content, "version", 7) == 0) {
        return ROUTE_VERSION;
    }
    if (content_len == 5 && strncmp(content, "alive", 5) == 0) {
        return ROUTE_ALIVE;
    }
    if (content_len == 7 && strncmp(content, "metrics", 7) == 0) {
        return ROUTE_METRICS;
    }

    /* Check for static page routes */
    if (content_len == 4 && strncmp(content, "docs", 4) == 0) {
        return ROUTE_DOCS;
    }
    if (content_len == 6 && strncmp(content, "status", 6) == 0) {
        return ROUTE_STATUS;
    }
    if (content_len == 5 && strncmp(content, "logos", 5) == 0) {
        return ROUTE_LOGOS;
    }

    /* Check for ACME HTTP-01 challenge path */
    /* Format: /.well-known/acme-challenge/{token} */
    if (content_len > 27 && strncmp(content, ".well-known/acme-challenge/", 27) == 0) {
        return ROUTE_ACME_CHALLENGE;
    }

    /* Check for /tx/{txid} pattern */
    if (content_len > 3 && strncmp(content, "tx/", 3) == 0) {
        const char *txid = content + 3;
        size_t txid_len = content_len - 3;

        /* Must be exactly 64 hex chars */
        if (txid_len == TXID_HEX_LENGTH && is_all_hex(txid, txid_len)) {
            return ROUTE_RESULT;
        }
        return ROUTE_ERROR;
    }

    /* Check if content is all hex */
    if (!is_all_hex(content, content_len)) {
        return ROUTE_ERROR;
    }

    /* Must be even length for valid hex bytes */
    if (content_len % 2 != 0) {
        return ROUTE_ERROR;
    }

    /* Exactly 64 hex chars = bare txid */
    if (content_len == TXID_HEX_LENGTH) {
        return ROUTE_RESULT;
    }

    /* More than 64 hex chars = raw transaction (need at least MIN_TX_HEX_LENGTH) */
    if (content_len >= MIN_TX_HEX_LENGTH) {
        return ROUTE_BROADCAST;
    }

    /* Between 64 and MIN_TX_HEX_LENGTH is invalid */
    return ROUTE_ERROR;
}
