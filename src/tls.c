#include "tls.h"
#include "log.h"

#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

/* ALPN protocol identifiers */
static const unsigned char alpn_h2[] = { 2, 'h', '2' };
static const unsigned char alpn_http11[] = { 8, 'h', 't', 't', 'p', '/', '1', '.', '1' };

/*
 * ALPN selection callback.
 * Called by OpenSSL during TLS handshake to select protocol.
 * Prefers h2 if available, falls back to http/1.1.
 */
static int tls_alpn_select_cb(SSL *ssl,
                              const unsigned char **out,
                              unsigned char *outlen,
                              const unsigned char *in,
                              unsigned int inlen,
                              void *arg)
{
    TLSContext *tls = (TLSContext *)arg;
    (void)ssl;

    /* Parse client's ALPN list */
    const unsigned char *p = in;
    const unsigned char *end = in + inlen;

    bool client_has_h2 = false;
    bool client_has_http11 = false;

    while (p < end) {
        unsigned char len = *p++;
        if (p + len > end) break;

        if (len == 2 && memcmp(p, "h2", 2) == 0) {
            client_has_h2 = true;
        } else if (len == 8 && memcmp(p, "http/1.1", 8) == 0) {
            client_has_http11 = true;
        }
        p += len;
    }

    /* Select protocol: prefer h2 if enabled and client supports it */
    if (tls->http2_enabled && client_has_h2) {
        *out = alpn_h2 + 1;  /* Skip length byte */
        *outlen = 2;
        log_debug("ALPN: Selected h2");
        return SSL_TLSEXT_ERR_OK;
    }

    if (client_has_http11) {
        *out = alpn_http11 + 1;  /* Skip length byte */
        *outlen = 8;
        log_debug("ALPN: Selected http/1.1");
        return SSL_TLSEXT_ERR_OK;
    }

    /* No matching protocol - let OpenSSL handle */
    log_debug("ALPN: No matching protocol");
    return SSL_TLSEXT_ERR_NOACK;
}

/*
 * Initialize TLS context.
 */
int tls_context_init(TLSContext *tls, const Config *config)
{
    memset(tls, 0, sizeof(*tls));
    tls->http2_enabled = config->http2_enabled;

    /* Initialize OpenSSL */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    /* Create SSL context */
    const SSL_METHOD *method = TLS_server_method();
    tls->ctx = SSL_CTX_new(method);
    if (!tls->ctx) {
        log_error("Failed to create SSL_CTX: %s",
                  ERR_error_string(ERR_get_error(), NULL));
        return -1;
    }

    /* Set minimum TLS version to 1.2 */
    SSL_CTX_set_min_proto_version(tls->ctx, TLS1_2_VERSION);

    /* Load certificate chain */
    if (SSL_CTX_use_certificate_chain_file(tls->ctx, config->tls_cert_file) != 1) {
        log_error("Failed to load certificate from %s: %s",
                  config->tls_cert_file, ERR_error_string(ERR_get_error(), NULL));
        SSL_CTX_free(tls->ctx);
        tls->ctx = NULL;
        return -1;
    }

    /* Load private key */
    if (SSL_CTX_use_PrivateKey_file(tls->ctx, config->tls_key_file, SSL_FILETYPE_PEM) != 1) {
        log_error("Failed to load private key from %s: %s",
                  config->tls_key_file, ERR_error_string(ERR_get_error(), NULL));
        SSL_CTX_free(tls->ctx);
        tls->ctx = NULL;
        return -1;
    }

    /* Verify private key matches certificate */
    if (SSL_CTX_check_private_key(tls->ctx) != 1) {
        log_error("Private key does not match certificate: %s",
                  ERR_error_string(ERR_get_error(), NULL));
        SSL_CTX_free(tls->ctx);
        tls->ctx = NULL;
        return -1;
    }

    /* Extract certificate expiry time using ASN1_TIME_diff
     * This calculates the difference between now and cert expiry */
    tls->cert_expiry = 0;
    X509 *cert = SSL_CTX_get0_certificate(tls->ctx);
    if (cert) {
        const ASN1_TIME *not_after = X509_get0_notAfter(cert);
        if (not_after) {
            int day_diff = 0, sec_diff = 0;
            /* ASN1_TIME_diff with NULL compares against current time
             * Returns 1 on success, 0 on failure */
            if (ASN1_TIME_diff(&day_diff, &sec_diff, NULL, not_after) == 1) {
                time_t now = time(NULL);
                tls->cert_expiry = now + ((time_t)day_diff * 86400) + sec_diff;
                log_info("TLS certificate expires in %d days", day_diff);
            } else {
                log_warn("Could not determine certificate expiry");
            }
        }
    }

    /* Set up ALPN callback for h2/http1.1 negotiation */
    SSL_CTX_set_alpn_select_cb(tls->ctx, tls_alpn_select_cb, tls);

    /* Set session caching for better performance */
    SSL_CTX_set_session_cache_mode(tls->ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_session_id_context(tls->ctx, (const unsigned char *)"rawrelay", 8);

    /* Disable TLS compression (CRIME attack mitigation) */
    SSL_CTX_set_options(tls->ctx, SSL_OP_NO_COMPRESSION);

    /* Enable server preference for cipher ordering */
    SSL_CTX_set_options(tls->ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);

    log_info("TLS context initialized (HTTP/2: %s)",
             tls->http2_enabled ? "enabled" : "disabled");

    return 0;
}

/*
 * Free TLS context.
 */
void tls_context_free(TLSContext *tls)
{
    if (tls->ctx) {
        SSL_CTX_free(tls->ctx);
        tls->ctx = NULL;
    }
}

/*
 * Create a new SSL object.
 */
SSL *tls_create_ssl(TLSContext *tls)
{
    if (!tls || !tls->ctx) {
        return NULL;
    }

    return SSL_new(tls->ctx);
}

/*
 * Get negotiated ALPN protocol.
 */
const char *tls_get_alpn_protocol(SSL *ssl)
{
    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;

    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);

    if (!alpn || alpn_len == 0) {
        return NULL;
    }

    /* Return static strings for known protocols */
    if (alpn_len == 2 && memcmp(alpn, "h2", 2) == 0) {
        return "h2";
    }
    if (alpn_len == 8 && memcmp(alpn, "http/1.1", 8) == 0) {
        return "http/1.1";
    }

    return NULL;
}

/*
 * Check if HTTP/2 was negotiated.
 */
bool tls_is_http2(SSL *ssl)
{
    const char *proto = tls_get_alpn_protocol(ssl);
    return proto && strcmp(proto, "h2") == 0;
}

/*
 * Get certificate expiry timestamp.
 */
time_t tls_get_cert_expiry(TLSContext *tls)
{
    if (!tls) {
        return 0;
    }
    return tls->cert_expiry;
}

/*
 * Reload TLS certificate and key.
 * Creates a new SSL_CTX and swaps it atomically.
 * Existing connections continue with their old SSL objects.
 */
int tls_context_reload(TLSContext *tls, const Config *config)
{
    if (!tls || !config) {
        return -1;
    }

    log_info("Reloading TLS certificates from %s and %s",
             config->tls_cert_file, config->tls_key_file);

    /* Create new SSL context */
    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *new_ctx = SSL_CTX_new(method);
    if (!new_ctx) {
        log_error("Failed to create new SSL_CTX: %s",
                  ERR_error_string(ERR_get_error(), NULL));
        return -1;
    }

    /* Set minimum TLS version to 1.2 */
    SSL_CTX_set_min_proto_version(new_ctx, TLS1_2_VERSION);

    /* Load certificate chain */
    if (SSL_CTX_use_certificate_chain_file(new_ctx, config->tls_cert_file) != 1) {
        log_error("Failed to reload certificate from %s: %s",
                  config->tls_cert_file, ERR_error_string(ERR_get_error(), NULL));
        SSL_CTX_free(new_ctx);
        return -1;
    }

    /* Load private key */
    if (SSL_CTX_use_PrivateKey_file(new_ctx, config->tls_key_file, SSL_FILETYPE_PEM) != 1) {
        log_error("Failed to reload private key from %s: %s",
                  config->tls_key_file, ERR_error_string(ERR_get_error(), NULL));
        SSL_CTX_free(new_ctx);
        return -1;
    }

    /* Verify private key matches certificate */
    if (SSL_CTX_check_private_key(new_ctx) != 1) {
        log_error("Private key does not match certificate: %s",
                  ERR_error_string(ERR_get_error(), NULL));
        SSL_CTX_free(new_ctx);
        return -1;
    }

    /* Extract certificate expiry time */
    time_t new_expiry = 0;
    X509 *cert = SSL_CTX_get0_certificate(new_ctx);
    if (cert) {
        const ASN1_TIME *not_after = X509_get0_notAfter(cert);
        if (not_after) {
            int day_diff = 0, sec_diff = 0;
            if (ASN1_TIME_diff(&day_diff, &sec_diff, NULL, not_after) == 1) {
                time_t now = time(NULL);
                new_expiry = now + ((time_t)day_diff * 86400) + sec_diff;
                log_info("New certificate expires in %d days", day_diff);
            }
        }
    }

    /* Set up ALPN callback */
    SSL_CTX_set_alpn_select_cb(new_ctx, tls_alpn_select_cb, tls);

    /* Set session caching */
    SSL_CTX_set_session_cache_mode(new_ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_session_id_context(new_ctx, (const unsigned char *)"rawrelay", 8);

    /* Disable TLS compression and enable server cipher preference */
    SSL_CTX_set_options(new_ctx, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_options(new_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);

    /* Atomically swap the context */
    SSL_CTX *old_ctx = tls->ctx;
    tls->ctx = new_ctx;
    tls->cert_expiry = new_expiry;

    /* Free old context */
    if (old_ctx) {
        SSL_CTX_free(old_ctx);
    }

    log_info("TLS certificate reload complete");
    return 0;
}
