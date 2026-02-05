#ifndef TLS_H
#define TLS_H

#include "config.h"
#include <stdbool.h>
#include <time.h>

/*
 * TLS context for server-side SSL connections.
 * Wraps OpenSSL SSL_CTX with ALPN support for HTTP/2 negotiation.
 */

/* Forward declarations to avoid OpenSSL header dependency */
struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;
typedef struct ssl_st SSL;

/*
 * TLS context - one per worker.
 */
typedef struct TLSContext {
    SSL_CTX *ctx;           /* OpenSSL SSL_CTX */
    bool http2_enabled;     /* Enable HTTP/2 via ALPN */
    time_t cert_expiry;     /* Certificate expiry timestamp */
} TLSContext;

/*
 * Get certificate expiry timestamp (Unix time).
 * Returns 0 if TLS is not enabled or no certificate loaded.
 */
time_t tls_get_cert_expiry(TLSContext *tls);

/*
 * Initialize TLS context with certificate and key files.
 * Sets up ALPN callback for h2/http1.1 negotiation.
 * Returns 0 on success, -1 on error.
 */
int tls_context_init(TLSContext *tls, const Config *config);

/*
 * Free TLS context resources.
 */
void tls_context_free(TLSContext *tls);

/*
 * Create a new SSL object from the context.
 * Returns NULL on error.
 */
SSL *tls_create_ssl(TLSContext *tls);

/*
 * Get the negotiated ALPN protocol.
 * Returns "h2", "http/1.1", or NULL if no ALPN.
 */
const char *tls_get_alpn_protocol(SSL *ssl);

/*
 * Check if HTTP/2 was negotiated via ALPN.
 */
bool tls_is_http2(SSL *ssl);

/*
 * Reload TLS certificate and key from config paths.
 * Used for ACME certificate renewal (SIGUSR2 trigger).
 * Returns 0 on success, -1 on error.
 */
int tls_context_reload(TLSContext *tls, const Config *config);

#endif /* TLS_H */
