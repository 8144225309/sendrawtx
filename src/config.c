#include "config.h"
#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>

/* Special value indicating chain not set - MUST be configured */
#define CHAIN_NOT_SET (-1)

/* Default values */
#define DEFAULT_INITIAL_BUFFER_SIZE   4096
#define DEFAULT_MAX_BUFFER_SIZE       (16 * 1024 * 1024)  /* 16MB */
#define DEFAULT_TIER_LARGE_THRESHOLD  (64 * 1024)         /* 64KB */
#define DEFAULT_TIER_HUGE_THRESHOLD   (1024 * 1024)       /* 1MB */
#define DEFAULT_LISTEN_PORT           8080
#define DEFAULT_MAX_CONNECTIONS       100
#define DEFAULT_READ_TIMEOUT_SEC      30
#define DEFAULT_STATIC_DIR            "./static"
#define DEFAULT_CACHE_MAX_AGE         3600                /* 1 hour */
#define DEFAULT_SLOTS_NORMAL_MAX      100
#define DEFAULT_SLOTS_LARGE_MAX       20
#define DEFAULT_SLOTS_HUGE_MAX        5
#define DEFAULT_RATE_LIMIT_RPS        100.0             /* 100 req/sec per IP */
#define DEFAULT_RATE_LIMIT_BURST      200.0             /* Allow burst of 200 */
#define DEFAULT_TLS_ENABLED           0                 /* TLS disabled by default */
#define DEFAULT_TLS_PORT              8443
#define DEFAULT_TLS_CERT_FILE         ""
#define DEFAULT_TLS_KEY_FILE          ""
#define DEFAULT_HTTP2_ENABLED         1                 /* HTTP/2 enabled when TLS enabled */
#define DEFAULT_JSON_LOGGING          0                 /* Text format by default */
#define DEFAULT_VERBOSE               0                 /* Minimal logging by default */
#define DEFAULT_ACME_CHALLENGE_DIR    ".well-known/acme-challenge"

/* RPC defaults */
#define DEFAULT_RPC_HOST              "127.0.0.1"
#define DEFAULT_RPC_TIMEOUT_SEC       30

/* Default RPC ports per chain */
#define DEFAULT_RPC_PORT_MAINNET      8332
#define DEFAULT_RPC_PORT_TESTNET      18332
#define DEFAULT_RPC_PORT_SIGNET       38332
#define DEFAULT_RPC_PORT_REGTEST      18443

/*
 * Initialize an RPCConfig struct to defaults.
 */
static void rpc_config_default(RPCConfig *rpc, int default_port)
{
    memset(rpc, 0, sizeof(RPCConfig));
    rpc->enabled = 0;
    strncpy(rpc->host, DEFAULT_RPC_HOST, sizeof(rpc->host) - 1);
    rpc->port = default_port;
    rpc->timeout_sec = DEFAULT_RPC_TIMEOUT_SEC;
}

static char *trim(char *str)
{
    char *end;

    /* Trim leading space */
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) {
        return str;
    }

    /* Trim trailing space */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    end[1] = '\0';

    return str;
}

static size_t parse_size_t(const char *str, size_t def_val)
{
    char *endptr;
    unsigned long long val;

    errno = 0;
    val = strtoull(str, &endptr, 10);

    if (errno != 0 || endptr == str || *endptr != '\0') {
        fprintf(stderr, "Warning: Invalid size value '%s', using default %zu\n",
                str, def_val);
        return def_val;
    }

    if (val > SIZE_MAX) {
        fprintf(stderr, "Warning: Size value '%s' too large, using default %zu\n",
                str, def_val);
        return def_val;
    }

    return (size_t)val;
}

static int parse_int(const char *str, int def_val)
{
    char *endptr;
    long val;

    errno = 0;
    val = strtol(str, &endptr, 10);

    if (errno != 0 || endptr == str || *endptr != '\0') {
        fprintf(stderr, "Warning: Invalid int value '%s', using default %d\n",
                str, def_val);
        return def_val;
    }

    if (val < 0 || val > INT_MAX) {
        fprintf(stderr, "Warning: Int value '%s' out of range, using default %d\n",
                str, def_val);
        return def_val;
    }

    return (int)val;
}

static double parse_double(const char *str, double def_val)
{
    char *endptr;
    double val;

    errno = 0;
    val = strtod(str, &endptr);

    if (errno != 0 || endptr == str || *endptr != '\0') {
        fprintf(stderr, "Warning: Invalid double value '%s', using default %.1f\n",
                str, def_val);
        return def_val;
    }

    if (val < 0) {
        fprintf(stderr, "Warning: Double value '%s' negative, using default %.1f\n",
                str, def_val);
        return def_val;
    }

    return val;
}

Config *config_default(void)
{
    Config *c = malloc(sizeof(Config));
    if (!c) {
        return NULL;
    }

    /* Network chain - MUST be set in config file */
    c->chain = (BitcoinChain)CHAIN_NOT_SET;

    c->initial_buffer_size = DEFAULT_INITIAL_BUFFER_SIZE;
    c->max_buffer_size = DEFAULT_MAX_BUFFER_SIZE;
    c->tier_large_threshold = DEFAULT_TIER_LARGE_THRESHOLD;
    c->tier_huge_threshold = DEFAULT_TIER_HUGE_THRESHOLD;
    c->listen_port = DEFAULT_LISTEN_PORT;
    c->max_connections = DEFAULT_MAX_CONNECTIONS;
    c->read_timeout_sec = DEFAULT_READ_TIMEOUT_SEC;
    strncpy(c->static_dir, DEFAULT_STATIC_DIR, sizeof(c->static_dir) - 1);
    c->static_dir[sizeof(c->static_dir) - 1] = '\0';
    c->cache_max_age = DEFAULT_CACHE_MAX_AGE;
    c->slots_normal_max = DEFAULT_SLOTS_NORMAL_MAX;
    c->slots_large_max = DEFAULT_SLOTS_LARGE_MAX;
    c->slots_huge_max = DEFAULT_SLOTS_HUGE_MAX;
    c->rate_limit_rps = DEFAULT_RATE_LIMIT_RPS;
    c->rate_limit_burst = DEFAULT_RATE_LIMIT_BURST;

    /* TLS settings */
    c->tls_enabled = DEFAULT_TLS_ENABLED;
    c->tls_port = DEFAULT_TLS_PORT;
    c->tls_cert_file[0] = '\0';
    c->tls_key_file[0] = '\0';
    c->http2_enabled = DEFAULT_HTTP2_ENABLED;

    /* Logging settings */
    c->json_logging = DEFAULT_JSON_LOGGING;
    c->verbose = DEFAULT_VERBOSE;

    /* ACME settings */
    strncpy(c->acme_challenge_dir, DEFAULT_ACME_CHALLENGE_DIR, sizeof(c->acme_challenge_dir) - 1);
    c->acme_challenge_dir[sizeof(c->acme_challenge_dir) - 1] = '\0';

    /* Security settings (Phase 6) */
    c->blocklist_file[0] = '\0';
    c->allowlist_file[0] = '\0';
    c->seccomp_enabled = 0;  /* Disabled by default, needs tuning */

    /* RPC settings - all disabled by default */
    rpc_config_default(&c->rpc_mainnet, DEFAULT_RPC_PORT_MAINNET);
    rpc_config_default(&c->rpc_testnet, DEFAULT_RPC_PORT_TESTNET);
    rpc_config_default(&c->rpc_signet, DEFAULT_RPC_PORT_SIGNET);
    rpc_config_default(&c->rpc_regtest, DEFAULT_RPC_PORT_REGTEST);

    return c;
}

Config *config_load(const char *path)
{
    Config *c;
    FILE *f;
    char line[512];
    char section[64] = "";

    c = config_default();
    if (!c) {
        return NULL;
    }

    if (!path) {
        return c;
    }

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Warning: Cannot open config file '%s', using defaults\n", path);
        return c;
    }

    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        char *key, *value, *eq;

        /* Skip empty lines and comments */
        if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';') {
            continue;
        }

        /* Section header */
        if (*trimmed == '[') {
            char *end = strchr(trimmed, ']');
            if (end) {
                *end = '\0';
                strncpy(section, trimmed + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        /* Key = value */
        eq = strchr(trimmed, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        key = trim(trimmed);
        value = trim(eq + 1);

        /* Parse based on section and key */
        if (strcmp(section, "buffer") == 0) {
            if (strcmp(key, "initial_size") == 0) {
                c->initial_buffer_size = parse_size_t(value, DEFAULT_INITIAL_BUFFER_SIZE);
            } else if (strcmp(key, "max_size") == 0) {
                c->max_buffer_size = parse_size_t(value, DEFAULT_MAX_BUFFER_SIZE);
            }
        } else if (strcmp(section, "tiers") == 0) {
            if (strcmp(key, "large_threshold") == 0) {
                c->tier_large_threshold = parse_size_t(value, DEFAULT_TIER_LARGE_THRESHOLD);
            } else if (strcmp(key, "huge_threshold") == 0) {
                c->tier_huge_threshold = parse_size_t(value, DEFAULT_TIER_HUGE_THRESHOLD);
            }
        } else if (strcmp(section, "server") == 0) {
            if (strcmp(key, "port") == 0) {
                c->listen_port = parse_int(value, DEFAULT_LISTEN_PORT);
            } else if (strcmp(key, "max_connections") == 0) {
                c->max_connections = parse_int(value, DEFAULT_MAX_CONNECTIONS);
            } else if (strcmp(key, "read_timeout") == 0) {
                c->read_timeout_sec = parse_int(value, DEFAULT_READ_TIMEOUT_SEC);
            }
        } else if (strcmp(section, "static") == 0) {
            if (strcmp(key, "dir") == 0) {
                strncpy(c->static_dir, value, sizeof(c->static_dir) - 1);
                c->static_dir[sizeof(c->static_dir) - 1] = '\0';
            } else if (strcmp(key, "cache_max_age") == 0) {
                c->cache_max_age = parse_int(value, DEFAULT_CACHE_MAX_AGE);
            }
        } else if (strcmp(section, "slots") == 0) {
            if (strcmp(key, "normal_max") == 0) {
                c->slots_normal_max = parse_int(value, DEFAULT_SLOTS_NORMAL_MAX);
            } else if (strcmp(key, "large_max") == 0) {
                c->slots_large_max = parse_int(value, DEFAULT_SLOTS_LARGE_MAX);
            } else if (strcmp(key, "huge_max") == 0) {
                c->slots_huge_max = parse_int(value, DEFAULT_SLOTS_HUGE_MAX);
            }
        } else if (strcmp(section, "ratelimit") == 0) {
            if (strcmp(key, "rps") == 0) {
                c->rate_limit_rps = parse_double(value, DEFAULT_RATE_LIMIT_RPS);
            } else if (strcmp(key, "burst") == 0) {
                c->rate_limit_burst = parse_double(value, DEFAULT_RATE_LIMIT_BURST);
            }
        } else if (strcmp(section, "tls") == 0) {
            if (strcmp(key, "enabled") == 0) {
                c->tls_enabled = parse_int(value, DEFAULT_TLS_ENABLED);
            } else if (strcmp(key, "port") == 0) {
                c->tls_port = parse_int(value, DEFAULT_TLS_PORT);
            } else if (strcmp(key, "cert_file") == 0) {
                strncpy(c->tls_cert_file, value, sizeof(c->tls_cert_file) - 1);
                c->tls_cert_file[sizeof(c->tls_cert_file) - 1] = '\0';
            } else if (strcmp(key, "key_file") == 0) {
                strncpy(c->tls_key_file, value, sizeof(c->tls_key_file) - 1);
                c->tls_key_file[sizeof(c->tls_key_file) - 1] = '\0';
            } else if (strcmp(key, "http2_enabled") == 0) {
                c->http2_enabled = parse_int(value, DEFAULT_HTTP2_ENABLED);
            }
        } else if (strcmp(section, "logging") == 0) {
            if (strcmp(key, "json") == 0) {
                c->json_logging = parse_int(value, DEFAULT_JSON_LOGGING);
            } else if (strcmp(key, "verbose") == 0) {
                c->verbose = parse_int(value, DEFAULT_VERBOSE);
            }
        } else if (strcmp(section, "acme") == 0) {
            if (strcmp(key, "challenge_dir") == 0) {
                strncpy(c->acme_challenge_dir, value, sizeof(c->acme_challenge_dir) - 1);
                c->acme_challenge_dir[sizeof(c->acme_challenge_dir) - 1] = '\0';
            }
        } else if (strcmp(section, "security") == 0) {
            if (strcmp(key, "blocklist_file") == 0) {
                strncpy(c->blocklist_file, value, sizeof(c->blocklist_file) - 1);
                c->blocklist_file[sizeof(c->blocklist_file) - 1] = '\0';
            } else if (strcmp(key, "allowlist_file") == 0) {
                strncpy(c->allowlist_file, value, sizeof(c->allowlist_file) - 1);
                c->allowlist_file[sizeof(c->allowlist_file) - 1] = '\0';
            } else if (strcmp(key, "seccomp") == 0) {
                c->seccomp_enabled = parse_int(value, 0);
            }
        } else if (strcmp(section, "network") == 0) {
            if (strcmp(key, "chain") == 0) {
                int chain = network_chain_from_string(value);
                if (chain < 0) {
                    fprintf(stderr, "ERROR: Invalid chain '%s'. Must be: mainnet, testnet, signet, regtest, or mixed\n", value);
                } else {
                    c->chain = (BitcoinChain)chain;
                }
            }
        } else if (strcmp(section, "rpc.mainnet") == 0) {
            RPCConfig *rpc = &c->rpc_mainnet;
            if (strcmp(key, "enabled") == 0) {
                rpc->enabled = parse_int(value, 0);
            } else if (strcmp(key, "host") == 0) {
                strncpy(rpc->host, value, sizeof(rpc->host) - 1);
            } else if (strcmp(key, "port") == 0) {
                rpc->port = parse_int(value, DEFAULT_RPC_PORT_MAINNET);
            } else if (strcmp(key, "user") == 0) {
                strncpy(rpc->user, value, sizeof(rpc->user) - 1);
            } else if (strcmp(key, "password") == 0) {
                strncpy(rpc->password, value, sizeof(rpc->password) - 1);
            } else if (strcmp(key, "cookie_file") == 0) {
                strncpy(rpc->cookie_file, value, sizeof(rpc->cookie_file) - 1);
            } else if (strcmp(key, "datadir") == 0) {
                strncpy(rpc->datadir, value, sizeof(rpc->datadir) - 1);
            } else if (strcmp(key, "timeout") == 0) {
                rpc->timeout_sec = parse_int(value, DEFAULT_RPC_TIMEOUT_SEC);
            } else if (strcmp(key, "wallet") == 0) {
                strncpy(rpc->wallet, value, sizeof(rpc->wallet) - 1);
            }
        } else if (strcmp(section, "rpc.testnet") == 0) {
            RPCConfig *rpc = &c->rpc_testnet;
            if (strcmp(key, "enabled") == 0) {
                rpc->enabled = parse_int(value, 0);
            } else if (strcmp(key, "host") == 0) {
                strncpy(rpc->host, value, sizeof(rpc->host) - 1);
            } else if (strcmp(key, "port") == 0) {
                rpc->port = parse_int(value, DEFAULT_RPC_PORT_TESTNET);
            } else if (strcmp(key, "user") == 0) {
                strncpy(rpc->user, value, sizeof(rpc->user) - 1);
            } else if (strcmp(key, "password") == 0) {
                strncpy(rpc->password, value, sizeof(rpc->password) - 1);
            } else if (strcmp(key, "cookie_file") == 0) {
                strncpy(rpc->cookie_file, value, sizeof(rpc->cookie_file) - 1);
            } else if (strcmp(key, "datadir") == 0) {
                strncpy(rpc->datadir, value, sizeof(rpc->datadir) - 1);
            } else if (strcmp(key, "timeout") == 0) {
                rpc->timeout_sec = parse_int(value, DEFAULT_RPC_TIMEOUT_SEC);
            } else if (strcmp(key, "wallet") == 0) {
                strncpy(rpc->wallet, value, sizeof(rpc->wallet) - 1);
            }
        } else if (strcmp(section, "rpc.signet") == 0) {
            RPCConfig *rpc = &c->rpc_signet;
            if (strcmp(key, "enabled") == 0) {
                rpc->enabled = parse_int(value, 0);
            } else if (strcmp(key, "host") == 0) {
                strncpy(rpc->host, value, sizeof(rpc->host) - 1);
            } else if (strcmp(key, "port") == 0) {
                rpc->port = parse_int(value, DEFAULT_RPC_PORT_SIGNET);
            } else if (strcmp(key, "user") == 0) {
                strncpy(rpc->user, value, sizeof(rpc->user) - 1);
            } else if (strcmp(key, "password") == 0) {
                strncpy(rpc->password, value, sizeof(rpc->password) - 1);
            } else if (strcmp(key, "cookie_file") == 0) {
                strncpy(rpc->cookie_file, value, sizeof(rpc->cookie_file) - 1);
            } else if (strcmp(key, "datadir") == 0) {
                strncpy(rpc->datadir, value, sizeof(rpc->datadir) - 1);
            } else if (strcmp(key, "timeout") == 0) {
                rpc->timeout_sec = parse_int(value, DEFAULT_RPC_TIMEOUT_SEC);
            } else if (strcmp(key, "wallet") == 0) {
                strncpy(rpc->wallet, value, sizeof(rpc->wallet) - 1);
            }
        } else if (strcmp(section, "rpc.regtest") == 0) {
            RPCConfig *rpc = &c->rpc_regtest;
            if (strcmp(key, "enabled") == 0) {
                rpc->enabled = parse_int(value, 0);
            } else if (strcmp(key, "host") == 0) {
                strncpy(rpc->host, value, sizeof(rpc->host) - 1);
            } else if (strcmp(key, "port") == 0) {
                rpc->port = parse_int(value, DEFAULT_RPC_PORT_REGTEST);
            } else if (strcmp(key, "user") == 0) {
                strncpy(rpc->user, value, sizeof(rpc->user) - 1);
            } else if (strcmp(key, "password") == 0) {
                strncpy(rpc->password, value, sizeof(rpc->password) - 1);
            } else if (strcmp(key, "cookie_file") == 0) {
                strncpy(rpc->cookie_file, value, sizeof(rpc->cookie_file) - 1);
            } else if (strcmp(key, "datadir") == 0) {
                strncpy(rpc->datadir, value, sizeof(rpc->datadir) - 1);
            } else if (strcmp(key, "timeout") == 0) {
                rpc->timeout_sec = parse_int(value, DEFAULT_RPC_TIMEOUT_SEC);
            } else if (strcmp(key, "wallet") == 0) {
                strncpy(rpc->wallet, value, sizeof(rpc->wallet) - 1);
            }
        }
    }

    fclose(f);

    /* Validate chain is set - REQUIRED */
    if ((int)c->chain == CHAIN_NOT_SET) {
        fprintf(stderr, "\n");
        fprintf(stderr, "===========================================================\n");
        fprintf(stderr, "ERROR: [network] chain= is REQUIRED in config file\n");
        fprintf(stderr, "===========================================================\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Add one of the following to your config file:\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "  [network]\n");
        fprintf(stderr, "  chain=mainnet     # Production Bitcoin network\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "  [network]\n");
        fprintf(stderr, "  chain=testnet     # Public test network\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "  [network]\n");
        fprintf(stderr, "  chain=signet      # Signed test network\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "  [network]\n");
        fprintf(stderr, "  chain=regtest     # Local regression test\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "  [network]\n");
        fprintf(stderr, "  chain=mixed       # Multi-chain mode (routes by address)\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "This prevents accidentally mixing transactions between networks.\n");
        fprintf(stderr, "===========================================================\n");
        config_free(c);
        return NULL;
    }

    /* Mixed mode requires at least one RPC connection enabled */
    if (c->chain == CHAIN_MIXED) {
        int any_enabled = c->rpc_mainnet.enabled || c->rpc_testnet.enabled ||
                          c->rpc_signet.enabled || c->rpc_regtest.enabled;
        if (!any_enabled) {
            fprintf(stderr, "\n");
            fprintf(stderr, "===========================================================\n");
            fprintf(stderr, "ERROR: Mixed mode requires at least one [rpc.*] enabled\n");
            fprintf(stderr, "===========================================================\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "When using chain=mixed, you must configure at least one RPC\n");
            fprintf(stderr, "connection. Example:\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "  [rpc.mainnet]\n");
            fprintf(stderr, "  enabled = 1\n");
            fprintf(stderr, "  host = 127.0.0.1\n");
            fprintf(stderr, "  port = 8332\n");
            fprintf(stderr, "  user = rpcuser\n");
            fprintf(stderr, "  password = rpcpassword\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "===========================================================\n");
            config_free(c);
            return NULL;
        }
    }

    /* Validate tier ordering */
    if (c->tier_large_threshold >= c->tier_huge_threshold) {
        fprintf(stderr, "Warning: large_threshold >= huge_threshold, adjusting\n");
        c->tier_huge_threshold = c->tier_large_threshold * 2;
    }

    return c;
}

void config_free(Config *c)
{
    free(c);
}

void config_print(const Config *c)
{
    if (!c) {
        printf("Config: NULL\n");
        return;
    }

    printf("Configuration:\n");

    /* Network/Chain - show prominently at top */
    printf("  Network:\n");
    printf("    chain:            %s", network_chain_to_string(c->chain));
    if (network_is_test_network(c->chain)) {
        printf(" [TEST NETWORK - coins have no value]");
    }
    printf("\n");

    printf("  Buffer:\n");
    printf("    initial_size:     %zu bytes\n", c->initial_buffer_size);
    printf("    max_size:         %zu bytes (%.1f MB)\n",
           c->max_buffer_size, c->max_buffer_size / (1024.0 * 1024.0));
    printf("  Tiers:\n");
    printf("    large_threshold:  %zu bytes (%.1f KB)\n",
           c->tier_large_threshold, c->tier_large_threshold / 1024.0);
    printf("    huge_threshold:   %zu bytes (%.1f MB)\n",
           c->tier_huge_threshold, c->tier_huge_threshold / (1024.0 * 1024.0));
    printf("  Server:\n");
    printf("    port:             %d\n", c->listen_port);
    printf("    max_connections:  %d\n", c->max_connections);
    printf("    read_timeout:     %d seconds\n", c->read_timeout_sec);
    printf("  Static:\n");
    printf("    dir:              %s\n", c->static_dir);
    printf("    cache_max_age:    %d seconds\n", c->cache_max_age);
    printf("  Slots (per worker):\n");
    printf("    normal_max:       %d\n", c->slots_normal_max);
    printf("    large_max:        %d\n", c->slots_large_max);
    printf("    huge_max:         %d\n", c->slots_huge_max);
    printf("  Rate Limiting (per worker, per IP):\n");
    if (c->rate_limit_rps > 0) {
        printf("    rps:              %.1f req/sec\n", c->rate_limit_rps);
        printf("    burst:            %.1f requests\n", c->rate_limit_burst);
    } else {
        printf("    status:           DISABLED\n");
    }
    printf("  TLS:\n");
    if (c->tls_enabled) {
        printf("    status:           ENABLED\n");
        printf("    port:             %d\n", c->tls_port);
        printf("    cert_file:        %s\n", c->tls_cert_file);
        printf("    key_file:         %s\n", c->tls_key_file);
        printf("    http2:            %s\n", c->http2_enabled ? "ENABLED" : "DISABLED");
    } else {
        printf("    status:           DISABLED\n");
    }
    printf("  Logging:\n");
    printf("    json_format:      %s\n", c->json_logging ? "ENABLED" : "DISABLED");
    printf("    verbose:          %s\n", c->verbose ? "ENABLED (full IPs)" : "DISABLED (IPs hidden)");
    printf("  ACME:\n");
    printf("    challenge_dir:    %s\n", c->acme_challenge_dir);
    printf("  Security:\n");
    printf("    blocklist_file:   %s\n", c->blocklist_file[0] ? c->blocklist_file : "(disabled)");
    printf("    allowlist_file:   %s\n", c->allowlist_file[0] ? c->allowlist_file : "(disabled)");
    printf("    seccomp:          %s\n", c->seccomp_enabled ? "ENABLED" : "DISABLED");
}
