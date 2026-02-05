#ifndef TCP_OPTS_H
#define TCP_OPTS_H

/*
 * TCP socket optimization helpers.
 *
 * TCP_NODELAY: Disables Nagle's algorithm.
 *   - Eliminates 40-500ms latency from Nagle + Delayed ACK interaction
 *   - Always enable for HTTP request/response protocols
 *
 * TCP_CORK: Accumulates data until uncorked.
 *   - Use when building multi-part responses (headers + body)
 *   - Headers and body sent in same TCP segment
 *   - MUST uncork to flush data (or 200ms delay)
 */

/*
 * Enable TCP_NODELAY on socket.
 * Call immediately after accept(), before any I/O.
 * Returns 0 on success, -1 on error.
 */
int tcp_nodelay_enable(int fd);

/*
 * Enable TCP_CORK - start accumulating data.
 * Returns 0 on success, -1 on error.
 */
int tcp_cork_enable(int fd);

/*
 * Disable TCP_CORK - flush accumulated data.
 * CRITICAL: Always call this after writing response!
 * Returns 0 on success, -1 on error.
 */
int tcp_cork_disable(int fd);

/*
 * Convenience: cork, then auto-uncork when scope exits.
 * Usage:
 *   tcp_cork_enable(fd);
 *   // ... write headers and body ...
 *   tcp_cork_disable(fd);  // flush
 */

#endif /* TCP_OPTS_H */
