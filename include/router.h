#ifndef ROUTER_H
#define ROUTER_H

#include <stddef.h>

/*
 * Route types for request routing.
 *
 * ROUTE_HOME:      / (empty path) → index.html
 * ROUTE_BROADCAST: Raw transaction hex (>64 chars) → broadcast.html
 * ROUTE_RESULT:    Transaction ID lookup → result.html
 * ROUTE_ERROR:     Invalid request → error.html
 */
typedef enum {
    ROUTE_HOME,            /* / → home page */
    ROUTE_BROADCAST,       /* Raw tx hex → broadcast page */
    ROUTE_RESULT,          /* Txid lookup → result page */
    ROUTE_ERROR,           /* Invalid → error page */
    ROUTE_HEALTH,          /* /health → JSON health status */
    ROUTE_READY,           /* /ready → readiness probe */
    ROUTE_ALIVE,           /* /alive → liveness probe */
    ROUTE_METRICS,         /* /metrics → Prometheus metrics */
    ROUTE_ACME_CHALLENGE   /* /.well-known/acme-challenge/{token} */
} RouteType;

/*
 * Determine route for a request path.
 *
 * Routing logic:
 * - /tx/{64-hex-chars} → ROUTE_RESULT
 * - /{64-hex-chars} → ROUTE_RESULT (bare txid)
 * - /{>64-hex-chars} → ROUTE_BROADCAST (raw tx)
 * - Everything else → ROUTE_ERROR
 *
 * @param path     Request path (e.g., "/tx/abc123...")
 * @param path_len Length of path string
 * @return RouteType indicating which page to serve
 */
RouteType route_request(const char *path, size_t path_len);

#endif /* ROUTER_H */
