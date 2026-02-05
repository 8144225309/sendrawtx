#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

typedef struct {
    /* Buffer settings */
    size_t initial_buffer_size;    /* Default: 4096 */
    size_t max_buffer_size;        /* Default: 16MB */

    /* Tier thresholds */
    size_t tier_large_threshold;   /* Default: 64KB */
    size_t tier_huge_threshold;    /* Default: 1MB */

    /* Server settings */
    int listen_port;               /* Default: 8080 */
    int max_connections;           /* Default: 100 */
    int read_timeout_sec;          /* Default: 30 */

    /* Static files settings */
    char static_dir[256];          /* Default: "./static" */
    int cache_max_age;             /* Default: 3600 (1 hour), 0 = no caching */

    /* Slot limits (per worker) */
    int slots_normal_max;          /* Default: 100 */
    int slots_large_max;           /* Default: 20 */
    int slots_huge_max;            /* Default: 5 */

    /* Rate limiting (per worker) */
    double rate_limit_rps;         /* Requests per second per IP, 0 = disabled */
    double rate_limit_burst;       /* Burst size (max tokens), 0 = same as rps */

    /* TLS settings (Phase 2) */
    int tls_enabled;               /* Default: 0 (disabled) */
    int tls_port;                  /* Default: 8443 */
    char tls_cert_file[256];       /* Path to certificate file */
    char tls_key_file[256];        /* Path to private key file */

    /* HTTP/2 settings (Phase 3) */
    int http2_enabled;             /* Default: 1 (enabled when TLS is enabled) */

    /* Logging settings (Phase 5) */
    int json_logging;              /* Default: 0 (text format), 1 = JSON format */
    int verbose;                   /* Default: 0 (minimal), 1 = full logging with IPs */

    /* ACME/Let's Encrypt settings (Phase 10) */
    char acme_challenge_dir[256];  /* Directory for ACME HTTP-01 challenges */

    /* Security settings (Phase 6) */
    char blocklist_file[256];      /* Path to IP blocklist file, empty = disabled */
    char allowlist_file[256];      /* Path to IP allowlist file, empty = disabled */
    int seccomp_enabled;           /* Default: 0 (disabled), 1 = enable seccomp filter */
} Config;

/*
 * Load configuration from an INI file.
 * Returns NULL on allocation failure.
 * Missing file or invalid values use defaults.
 */
Config *config_load(const char *path);

/*
 * Create configuration with default values.
 * Returns NULL on allocation failure.
 */
Config *config_default(void);

/*
 * Free configuration structure.
 */
void config_free(Config *c);

/*
 * Print configuration to stdout (for debugging).
 */
void config_print(const Config *c);

#endif /* CONFIG_H */
