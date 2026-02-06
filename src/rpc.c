/*
 * Bitcoin Core RPC Client - Phase 13c
 *
 * Synchronous JSON-RPC over HTTP using plain sockets.
 * Simple and reliable - no complex async state machines.
 */

#include "rpc.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

/* Base64 encoding for auth header */
static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const char *input, size_t len, char *output)
{
    size_t i, j;
    for (i = 0, j = 0; i < len; i += 3) {
        uint32_t v = (unsigned char)input[i] << 16;
        if (i + 1 < len) v |= (unsigned char)input[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)input[i + 2];

        output[j++] = base64_table[(v >> 18) & 0x3f];
        output[j++] = base64_table[(v >> 12) & 0x3f];
        output[j++] = (i + 1 < len) ? base64_table[(v >> 6) & 0x3f] : '=';
        output[j++] = (i + 2 < len) ? base64_table[v & 0x3f] : '=';
    }
    output[j] = '\0';
}

/*
 * Build Basic auth header from user:password.
 */
static void build_auth_header(const char *user, const char *password,
                              char *header, size_t header_sz)
{
    char credentials[128];
    char encoded[256];

    snprintf(credentials, sizeof(credentials), "%s:%s", user, password);
    base64_encode(credentials, strlen(credentials), encoded);

    /* Ensure we don't overflow header buffer */
    if (header_sz > 7) {
        snprintf(header, header_sz, "Basic %.500s", encoded);
    } else {
        header[0] = '\0';
    }
}

/*
 * Read cookie file and build auth header.
 * Cookie format: __cookie__:<hex>
 */
static int read_cookie_file(const char *path, char *header, size_t header_sz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return RPC_ERR_COOKIE;
    }

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return RPC_ERR_COOKIE;
    }
    fclose(f);

    /* Remove newline */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[--len] = '\0';
    }

    /* Cookie format is already user:pass style (__cookie__:hex) */
    char encoded[256];
    base64_encode(line, strlen(line), encoded);

    /* Ensure we don't overflow header buffer */
    if (header_sz > 7) {
        snprintf(header, header_sz, "Basic %.500s", encoded);
    } else {
        header[0] = '\0';
    }

    return 0;
}

/*
 * Get cookie file path from datadir and chain.
 */
static void get_cookie_path_from_datadir(const char *datadir, BitcoinChain chain,
                                         char *path, size_t path_sz)
{
    const char *subdir;
    switch (chain) {
        case CHAIN_TESTNET: subdir = "/testnet3"; break;
        case CHAIN_SIGNET:  subdir = "/signet"; break;
        case CHAIN_REGTEST: subdir = "/regtest"; break;
        default:            subdir = ""; break;
    }
    snprintf(path, path_sz, "%.200s%.10s/.cookie", datadir, subdir);
}

/* ========== Initialization ========== */

int rpc_init(RPCClient *client, const RPCConfig *config, BitcoinChain chain)
{
    memset(client, 0, sizeof(RPCClient));
    client->chain = chain;
    client->timeout_sec = config->timeout_sec > 0 ? config->timeout_sec : 30;

    snprintf(client->host, sizeof(client->host), "%s", config->host);
    client->port = config->port;

    if (config->wallet[0]) {
        snprintf(client->wallet, sizeof(client->wallet), "%s", config->wallet);
    }

    /* Determine auth method */
    if (config->cookie_file[0]) {
        /* Direct cookie file path */
        snprintf(client->cookie_path, sizeof(client->cookie_path), "%s", config->cookie_file);
        if (read_cookie_file(client->cookie_path, client->auth_header,
                             sizeof(client->auth_header)) != 0) {
            log_warn("RPC: Failed to read cookie file: %s", client->cookie_path);
            return RPC_ERR_COOKIE;
        }
    } else if (config->datadir[0]) {
        /* Build cookie path from datadir */
        get_cookie_path_from_datadir(config->datadir, chain,
                                     client->cookie_path, sizeof(client->cookie_path));
        if (read_cookie_file(client->cookie_path, client->auth_header,
                             sizeof(client->auth_header)) != 0) {
            log_warn("RPC: Failed to read cookie file: %s", client->cookie_path);
            return RPC_ERR_COOKIE;
        }
    } else if (config->user[0] && config->password[0]) {
        /* Username/password auth */
        build_auth_header(config->user, config->password,
                         client->auth_header, sizeof(client->auth_header));
    } else {
        log_error("RPC: No authentication configured for %s",
                  network_chain_to_string(chain));
        return RPC_ERR_AUTH;
    }

    log_info("RPC: Initialized %s client -> %s:%d",
             network_chain_to_string(chain), client->host, client->port);

    return RPC_OK;
}

int rpc_init_simple(RPCClient *client, const char *host, int port,
                    const char *user, const char *password, BitcoinChain chain)
{
    RPCConfig config = {0};
    config.enabled = 1;
    strncpy(config.host, host, sizeof(config.host) - 1);
    config.port = port;
    strncpy(config.user, user, sizeof(config.user) - 1);
    strncpy(config.password, password, sizeof(config.password) - 1);
    config.timeout_sec = 30;

    return rpc_init(client, &config, chain);
}

int rpc_init_cookie(RPCClient *client, const char *host, int port,
                    const char *cookie_path, BitcoinChain chain)
{
    RPCConfig config = {0};
    config.enabled = 1;
    strncpy(config.host, host, sizeof(config.host) - 1);
    config.port = port;
    strncpy(config.cookie_file, cookie_path, sizeof(config.cookie_file) - 1);
    config.timeout_sec = 30;

    return rpc_init(client, &config, chain);
}

int rpc_refresh_cookie(RPCClient *client)
{
    if (!client->cookie_path[0]) {
        return RPC_OK;  /* Not using cookie auth */
    }

    int ret = read_cookie_file(client->cookie_path, client->auth_header,
                               sizeof(client->auth_header));
    if (ret == 0) {
        log_debug("RPC: Refreshed cookie auth for %s",
                  network_chain_to_string(client->chain));
    }
    return ret;
}

/* ========== Low-level HTTP ========== */

/*
 * Connect to RPC server.
 * Returns socket fd or -1 on error.
 */
static int rpc_connect(RPCClient *client)
{
    struct addrinfo hints, *res, *rp;
    int fd = -1;
    char port_str[16];

    snprintf(port_str, sizeof(port_str), "%d", client->port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(client->host, port_str, &hints, &res);
    if (err != 0) {
        log_error("RPC: getaddrinfo(%s): %s", client->host, gai_strerror(err));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        /* Set connect timeout */
        struct timeval tv = { .tv_sec = client->timeout_sec, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  /* Success */
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        log_error("RPC: Failed to connect to %s:%d", client->host, client->port);
    }

    return fd;
}

/*
 * Send HTTP request and receive response.
 * Returns response body (caller must free) or NULL on error.
 */
static char *rpc_http_request(RPCClient *client,
                              const char *body, size_t body_len,
                              int *http_status, int *rpc_error)
{
    int fd = rpc_connect(client);
    if (fd < 0) {
        *rpc_error = RPC_ERR_CONNECT;
        return NULL;
    }

    /* Build HTTP request */
    char header[2048];
    const char *path = client->wallet[0] ? "/wallet/" : "/";

    int header_len = snprintf(header, sizeof(header),
        "POST %s%s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, client->wallet,
        client->host, client->port,
        client->auth_header,
        body_len);

    /* Send header + body */
    if (send(fd, header, header_len, 0) != header_len ||
        send(fd, body, body_len, 0) != (ssize_t)body_len) {
        log_error("RPC: Failed to send request");
        close(fd);
        *rpc_error = RPC_ERR_CONNECT;
        return NULL;
    }

    /* Receive response */
    char *response = malloc(RPC_MAX_RESPONSE_LEN);
    if (!response) {
        close(fd);
        *rpc_error = RPC_ERR_MEMORY;
        return NULL;
    }

    size_t total = 0;
    ssize_t n;
    while ((n = recv(fd, response + total, RPC_MAX_RESPONSE_LEN - total - 1, 0)) > 0) {
        total += n;
        if (total >= RPC_MAX_RESPONSE_LEN - 1) break;
    }
    response[total] = '\0';

    close(fd);

    if (total == 0) {
        free(response);
        *rpc_error = RPC_ERR_CONNECT;
        return NULL;
    }

    /* Parse HTTP status */
    *http_status = 0;
    if (strncmp(response, "HTTP/", 5) == 0) {
        char *status_start = strchr(response, ' ');
        if (status_start) {
            *http_status = atoi(status_start + 1);
        }
    }

    /* Find body (after \r\n\r\n) */
    char *body_start = strstr(response, "\r\n\r\n");
    if (!body_start) {
        free(response);
        *rpc_error = RPC_ERR_PARSE;
        return NULL;
    }
    body_start += 4;

    /* Move body to start of buffer */
    size_t body_offset = body_start - response;
    memmove(response, body_start, total - body_offset + 1);

    client->request_count++;

    if (*http_status == 401 || *http_status == 403) {
        *rpc_error = RPC_ERR_AUTH;
        free(response);
        return NULL;
    }

    *rpc_error = RPC_OK;
    return response;
}

/* ========== JSON-RPC Helpers ========== */

/*
 * Build JSON-RPC request.
 */
static char *build_jsonrpc_request(const char *method, const char *params)
{
    static uint64_t request_id = 0;
    char *request = malloc(strlen(params) + 256);
    if (!request) return NULL;

    sprintf(request,
        "{\"jsonrpc\":\"1.0\",\"id\":%lu,\"method\":\"%s\",\"params\":%s}",
        (unsigned long)(++request_id), method, params);

    return request;
}

/*
 * Extract result or error from JSON-RPC response.
 * Very simple JSON parsing - just finds "result" or "error" fields.
 */
static int parse_jsonrpc_response(const char *response, char *result,
                                  size_t result_sz, int *is_error)
{
    *is_error = 0;

    /* Check for error first */
    const char *error_pos = strstr(response, "\"error\":");
    if (error_pos) {
        /* Check if error is null */
        const char *val_start = error_pos + 8;
        while (*val_start == ' ' || *val_start == '\t') val_start++;

        if (strncmp(val_start, "null", 4) != 0) {
            /* Has error - extract message */
            const char *msg_pos = strstr(val_start, "\"message\":");
            if (msg_pos) {
                msg_pos += 10;
                while (*msg_pos == ' ' || *msg_pos == '"') msg_pos++;
                const char *msg_end = strchr(msg_pos, '"');
                if (msg_end) {
                    size_t len = msg_end - msg_pos;
                    if (len >= result_sz) len = result_sz - 1;
                    strncpy(result, msg_pos, len);
                    result[len] = '\0';
                    *is_error = 1;
                    return RPC_ERR_NODE;
                }
            }
            /* Fallback - copy entire error object */
            strncpy(result, val_start, result_sz - 1);
            result[result_sz - 1] = '\0';
            *is_error = 1;
            return RPC_ERR_NODE;
        }
    }

    /* Extract result */
    const char *result_pos = strstr(response, "\"result\":");
    if (!result_pos) {
        strncpy(result, "No result in response", result_sz - 1);
        return RPC_ERR_PARSE;
    }

    result_pos += 9;
    while (*result_pos == ' ' || *result_pos == '\t') result_pos++;

    /* Handle string result (txid, etc.) */
    if (*result_pos == '"') {
        result_pos++;
        const char *end = strchr(result_pos, '"');
        if (end) {
            size_t len = end - result_pos;
            if (len >= result_sz) len = result_sz - 1;
            strncpy(result, result_pos, len);
            result[len] = '\0';
            return RPC_OK;
        }
    }

    /* Handle object/array result */
    if (*result_pos == '{' || *result_pos == '[') {
        /* Find matching brace/bracket */
        int depth = 1;
        char open = *result_pos;
        char close_char = (open == '{') ? '}' : ']';
        const char *p = result_pos + 1;
        int in_string = 0;

        while (*p && depth > 0) {
            if (*p == '"' && *(p-1) != '\\') {
                in_string = !in_string;
            } else if (!in_string) {
                if (*p == open) depth++;
                else if (*p == close_char) depth--;
            }
            p++;
        }

        size_t len = p - result_pos;
        if (len >= result_sz) len = result_sz - 1;
        strncpy(result, result_pos, len);
        result[len] = '\0';
        return RPC_OK;
    }

    /* Handle null */
    if (strncmp(result_pos, "null", 4) == 0) {
        strncpy(result, "null", result_sz - 1);
        return RPC_OK;
    }

    /* Handle number/boolean */
    const char *end = result_pos;
    while (*end && *end != ',' && *end != '}') end++;
    size_t len = end - result_pos;
    if (len >= result_sz) len = result_sz - 1;
    strncpy(result, result_pos, len);
    result[len] = '\0';

    return RPC_OK;
}

/* ========== High-level RPC Methods ========== */

/*
 * Generic RPC call.
 */
static int rpc_call(RPCClient *client, const char *method, const char *params,
                    char *result, size_t result_sz)
{
    char *request = build_jsonrpc_request(method, params);
    if (!request) {
        strncpy(result, "Memory allocation failed", result_sz - 1);
        return RPC_ERR_MEMORY;
    }

    int http_status, rpc_error;
    char *response = rpc_http_request(client, request, strlen(request),
                                      &http_status, &rpc_error);
    free(request);

    if (!response) {
        client->error_count++;

        /* Try refreshing cookie on auth failure */
        if (rpc_error == RPC_ERR_AUTH && client->cookie_path[0]) {
            log_info("RPC: Auth failed, refreshing cookie...");
            if (rpc_refresh_cookie(client) == 0) {
                /* Retry with new cookie */
                request = build_jsonrpc_request(method, params);
                if (request) {
                    response = rpc_http_request(client, request, strlen(request),
                                              &http_status, &rpc_error);
                    free(request);
                }
            }
        }

        if (!response) {
            switch (rpc_error) {
                case RPC_ERR_CONNECT:
                    strncpy(result, "Failed to connect to node", result_sz - 1);
                    break;
                case RPC_ERR_AUTH:
                    strncpy(result, "Authentication failed", result_sz - 1);
                    break;
                case RPC_ERR_TIMEOUT:
                    strncpy(result, "Request timed out", result_sz - 1);
                    break;
                default:
                    strncpy(result, "RPC request failed", result_sz - 1);
            }
            client->available = 0;
            return rpc_error;
        }
    }

    int is_error;
    int ret = parse_jsonrpc_response(response, result, result_sz, &is_error);
    free(response);

    if (is_error) {
        client->error_count++;
        log_debug("RPC %s error: %s", method, result);
    } else {
        client->available = 1;
    }

    return ret;
}

int rpc_test_connection(RPCClient *client)
{
    char result[256];
    int ret = rpc_call(client, "getblockchaininfo", "[]", result, sizeof(result));

    if (ret == RPC_OK) {
        client->available = 1;
        log_info("RPC: %s connection OK", network_chain_to_string(client->chain));
    } else {
        client->available = 0;
        log_warn("RPC: %s connection failed: %s",
                 network_chain_to_string(client->chain), result);
    }

    return ret;
}

int rpc_sendrawtransaction(RPCClient *client, const char *hex_tx,
                           char *result, size_t result_sz)
{
    /* Build params: ["<hex>"] */
    size_t params_len = strlen(hex_tx) + 8;
    char *params = malloc(params_len);
    if (!params) {
        strncpy(result, "Memory allocation failed", result_sz - 1);
        return RPC_ERR_MEMORY;
    }
    snprintf(params, params_len, "[\"%s\"]", hex_tx);

    int ret = rpc_call(client, "sendrawtransaction", params, result, result_sz);
    free(params);

    if (ret == RPC_OK) {
        log_info("RPC: Broadcast TX -> %s (%.16s...)",
                 network_chain_to_string(client->chain), result);
    }

    return ret;
}

int rpc_getblockchaininfo(RPCClient *client, char *result, size_t result_sz)
{
    return rpc_call(client, "getblockchaininfo", "[]", result, result_sz);
}

int rpc_getrawtransaction(RPCClient *client, const char *txid,
                          char *result, size_t result_sz)
{
    char params[128];
    snprintf(params, sizeof(params), "[\"%s\"]", txid);
    return rpc_call(client, "getrawtransaction", params, result, result_sz);
}

int rpc_getmempoolentry(RPCClient *client, const char *txid,
                        char *result, size_t result_sz)
{
    char params[128];
    snprintf(params, sizeof(params), "[\"%s\"]", txid);
    return rpc_call(client, "getmempoolentry", params, result, result_sz);
}

int rpc_decoderawtransaction(RPCClient *client, const char *hex_tx,
                             char *result, size_t result_sz)
{
    size_t params_len = strlen(hex_tx) + 8;
    char *params = malloc(params_len);
    if (!params) return RPC_ERR_MEMORY;
    snprintf(params, params_len, "[\"%s\"]", hex_tx);

    int ret = rpc_call(client, "decoderawtransaction", params, result, result_sz);
    free(params);
    return ret;
}

int rpc_testmempoolaccept(RPCClient *client, const char *hex_tx,
                          char *result, size_t result_sz)
{
    size_t params_len = strlen(hex_tx) + 16;
    char *params = malloc(params_len);
    if (!params) return RPC_ERR_MEMORY;
    snprintf(params, params_len, "[[\"%s\"]]", hex_tx);

    int ret = rpc_call(client, "testmempoolaccept", params, result, result_sz);
    free(params);
    return ret;
}

/* ========== RPC Manager ========== */

int rpc_manager_init(RPCManager *mgr,
                     const RPCConfig *mainnet,
                     const RPCConfig *testnet,
                     const RPCConfig *signet,
                     const RPCConfig *regtest)
{
    memset(mgr, 0, sizeof(RPCManager));

    if (mainnet && mainnet->enabled) {
        if (rpc_init(&mgr->mainnet, mainnet, CHAIN_MAINNET) != 0) {
            log_warn("RPC Manager: Failed to init mainnet");
        }
    }

    if (testnet && testnet->enabled) {
        if (rpc_init(&mgr->testnet, testnet, CHAIN_TESTNET) != 0) {
            log_warn("RPC Manager: Failed to init testnet");
        }
    }

    if (signet && signet->enabled) {
        if (rpc_init(&mgr->signet, signet, CHAIN_SIGNET) != 0) {
            log_warn("RPC Manager: Failed to init signet");
        }
    }

    if (regtest && regtest->enabled) {
        if (rpc_init(&mgr->regtest, regtest, CHAIN_REGTEST) != 0) {
            log_warn("RPC Manager: Failed to init regtest");
        }
    }

    return 0;
}

RPCClient *rpc_manager_get_client(RPCManager *mgr, BitcoinChain chain)
{
    switch (chain) {
        case CHAIN_MAINNET: return mgr->mainnet.host[0] ? &mgr->mainnet : NULL;
        case CHAIN_TESTNET: return mgr->testnet.host[0] ? &mgr->testnet : NULL;
        case CHAIN_SIGNET:  return mgr->signet.host[0] ? &mgr->signet : NULL;
        case CHAIN_REGTEST: return mgr->regtest.host[0] ? &mgr->regtest : NULL;
        default: return NULL;
    }
}

int rpc_manager_broadcast(RPCManager *mgr, BitcoinChain chain,
                          const char *hex_tx, char *txid, size_t txid_sz)
{
    RPCClient *client = rpc_manager_get_client(mgr, chain);
    if (!client) {
        snprintf(txid, txid_sz, "No RPC configured for %s",
                 network_chain_to_string(chain));
        return RPC_ERR_CONNECT;
    }

    mgr->total_broadcasts++;

    int ret = rpc_sendrawtransaction(client, hex_tx, txid, txid_sz);
    if (ret == RPC_OK) {
        mgr->successful_broadcasts++;
    } else {
        mgr->failed_broadcasts++;
    }

    return ret;
}

void rpc_manager_log_status(RPCManager *mgr)
{
    log_info("RPC Manager Status:");
    log_info("  Total broadcasts: %lu", (unsigned long)mgr->total_broadcasts);
    log_info("  Successful: %lu", (unsigned long)mgr->successful_broadcasts);
    log_info("  Failed: %lu", (unsigned long)mgr->failed_broadcasts);

    if (mgr->mainnet.host[0]) {
        log_info("  Mainnet: %s:%d (%s)", mgr->mainnet.host, mgr->mainnet.port,
                 mgr->mainnet.available ? "UP" : "DOWN");
    }
    if (mgr->testnet.host[0]) {
        log_info("  Testnet: %s:%d (%s)", mgr->testnet.host, mgr->testnet.port,
                 mgr->testnet.available ? "UP" : "DOWN");
    }
    if (mgr->signet.host[0]) {
        log_info("  Signet: %s:%d (%s)", mgr->signet.host, mgr->signet.port,
                 mgr->signet.available ? "UP" : "DOWN");
    }
    if (mgr->regtest.host[0]) {
        log_info("  Regtest: %s:%d (%s)", mgr->regtest.host, mgr->regtest.port,
                 mgr->regtest.available ? "UP" : "DOWN");
    }
}

/* ========== Async RPC Implementation ========== */

/* Default async timeout in seconds */
#define RPC_ASYNC_TIMEOUT_SEC 30

/* Initial response buffer size */
#define RPC_ASYNC_INITIAL_BUF 4096

/* Forward declarations for async callbacks */
static void rpc_async_read_cb(struct bufferevent *bev, void *ctx);
static void rpc_async_event_cb(struct bufferevent *bev, short events, void *ctx);
static void rpc_async_timeout_cb(evutil_socket_t fd, short events, void *ctx);
static void rpc_async_process_response(RPCRequest *req);
static void rpc_request_complete(RPCRequest *req, int status,
                                  const char *result, size_t result_len);
static int rpc_async_connect(RPCRequest *req);

/*
 * Pre-resolve hostname for a client into resolved_addr.
 * Must be called before seccomp locks down DNS.
 */
static int rpc_resolve_host(RPCClient *client)
{
    struct addrinfo hints, *res;
    char port_str[16];

    snprintf(port_str, sizeof(port_str), "%d", client->port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(client->host, port_str, &hints, &res);
    if (err != 0) {
        log_error("RPC: getaddrinfo(%s): %s", client->host, gai_strerror(err));
        return -1;
    }

    /* Store the first result */
    if (res->ai_addrlen <= sizeof(client->resolved_addr)) {
        memcpy(&client->resolved_addr, res->ai_addr, res->ai_addrlen);
        client->resolved_addr_len = res->ai_addrlen;
    } else {
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    log_debug("RPC: Pre-resolved %s:%d", client->host, client->port);
    return 0;
}

int rpc_manager_init_async(RPCManager *mgr,
                            struct event_base *base,
                            const RPCConfig *mainnet,
                            const RPCConfig *testnet,
                            const RPCConfig *signet,
                            const RPCConfig *regtest)
{
    /* First do the normal sync init */
    int ret = rpc_manager_init(mgr, mainnet, testnet, signet, regtest);

    /* Store event base for async operations */
    mgr->base = base;
    mgr->active_requests = NULL;

    /* Pre-resolve hostnames for all enabled clients */
    if (mgr->mainnet.host[0]) {
        if (rpc_resolve_host(&mgr->mainnet) < 0) {
            log_warn("RPC: Failed to resolve mainnet host %s", mgr->mainnet.host);
        }
    }
    if (mgr->testnet.host[0]) {
        if (rpc_resolve_host(&mgr->testnet) < 0) {
            log_warn("RPC: Failed to resolve testnet host %s", mgr->testnet.host);
        }
    }
    if (mgr->signet.host[0]) {
        if (rpc_resolve_host(&mgr->signet) < 0) {
            log_warn("RPC: Failed to resolve signet host %s", mgr->signet.host);
        }
    }
    if (mgr->regtest.host[0]) {
        if (rpc_resolve_host(&mgr->regtest) < 0) {
            log_warn("RPC: Failed to resolve regtest host %s", mgr->regtest.host);
        }
    }

    log_info("RPC: Async manager initialized (event_base=%p)", (void *)base);
    return ret;
}

/*
 * Add request to the manager's active list.
 */
static void rpc_request_list_add(RPCManager *mgr, RPCRequest *req)
{
    req->next = mgr->active_requests;
    req->prev = NULL;
    if (mgr->active_requests) {
        mgr->active_requests->prev = req;
    }
    mgr->active_requests = req;
}

/*
 * Remove request from the manager's active list.
 */
static void rpc_request_list_remove(RPCManager *mgr, RPCRequest *req)
{
    if (req->prev) {
        req->prev->next = req->next;
    } else {
        mgr->active_requests = req->next;
    }
    if (req->next) {
        req->next->prev = req->prev;
    }
    req->next = NULL;
    req->prev = NULL;
}

/*
 * Free all resources associated with an RPCRequest.
 */
static void rpc_request_free(RPCRequest *req)
{
    if (req->bev) {
        bufferevent_free(req->bev);
        req->bev = NULL;
    }
    if (req->timeout_ev) {
        event_free(req->timeout_ev);
        req->timeout_ev = NULL;
    }
    free(req->request_body);
    free(req->response_buf);
    free(req);
}

/*
 * Complete an async request: fire callback, remove from list, free.
 */
static void rpc_request_complete(RPCRequest *req, int status,
                                  const char *result, size_t result_len)
{
    /* Remove from active list */
    if (req->mgr) {
        rpc_request_list_remove(req->mgr, req);
    }

    /* Fire callback if still set (cancelled requests have NULL callback) */
    if (req->callback) {
        req->callback(status, result, result_len, req->callback_data);
    }

    rpc_request_free(req);
}

/*
 * Build HTTP request bytes for an async RPC call.
 * Returns a malloc'd string (caller takes ownership).
 */
static char *rpc_build_http_request(RPCClient *client, const char *body,
                                     size_t body_len, size_t *out_len)
{
    const char *path = client->wallet[0] ? "/wallet/" : "/";
    char header[2048];

    int header_len = snprintf(header, sizeof(header),
        "POST %s%s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Authorization: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, client->wallet,
        client->host, client->port,
        client->auth_header,
        body_len);

    size_t total_len = (size_t)header_len + body_len;
    char *buf = malloc(total_len);
    if (!buf) return NULL;

    memcpy(buf, header, header_len);
    memcpy(buf + header_len, body, body_len);
    *out_len = total_len;
    return buf;
}

/*
 * Initiate async connection to RPC server.
 */
static int rpc_async_connect(RPCRequest *req)
{
    RPCClient *client = req->client;

    if (client->resolved_addr_len == 0) {
        log_error("RPC async: No resolved address for %s:%d",
                  client->host, client->port);
        return -1;
    }

    /* Create bufferevent */
    req->bev = bufferevent_socket_new(req->base, -1, BEV_OPT_CLOSE_ON_FREE);
    if (!req->bev) {
        log_error("RPC async: Failed to create bufferevent");
        return -1;
    }

    /* Set callbacks */
    bufferevent_setcb(req->bev, rpc_async_read_cb, NULL,
                      rpc_async_event_cb, req);

    /* Set per-operation timeouts on the bufferevent */
    struct timeval tv = { .tv_sec = client->timeout_sec, .tv_usec = 0 };
    bufferevent_set_timeouts(req->bev, &tv, &tv);

    /* Start overall timeout timer */
    req->timeout_ev = evtimer_new(req->base, rpc_async_timeout_cb, req);
    if (req->timeout_ev) {
        struct timeval overall_tv = { .tv_sec = client->timeout_sec, .tv_usec = 0 };
        evtimer_add(req->timeout_ev, &overall_tv);
    }

    /* Initiate async connect */
    if (bufferevent_socket_connect(req->bev,
            (struct sockaddr *)&client->resolved_addr,
            client->resolved_addr_len) < 0) {
        log_error("RPC async: bufferevent_socket_connect failed");
        bufferevent_free(req->bev);
        req->bev = NULL;
        if (req->timeout_ev) {
            event_free(req->timeout_ev);
            req->timeout_ev = NULL;
        }
        return -1;
    }

    return 0;
}

/*
 * Event callback: connected, EOF, error, timeout.
 */
static void rpc_async_event_cb(struct bufferevent *bev, short events, void *ctx)
{
    RPCRequest *req = ctx;
    (void)bev;

    if (events & BEV_EVENT_CONNECTED) {
        /* Connection established — send the HTTP request */
        size_t http_len;
        char *http_req = rpc_build_http_request(req->client,
                                                 req->request_body,
                                                 req->request_body_len,
                                                 &http_len);
        if (!http_req) {
            rpc_request_complete(req, RPC_ERR_MEMORY, "Memory allocation failed", 24);
            return;
        }

        bufferevent_write(req->bev, http_req, http_len);
        free(http_req);

        /* Enable reading for the response */
        bufferevent_enable(req->bev, EV_READ);
        return;
    }

    if (events & BEV_EVENT_EOF) {
        /* Server closed connection — response is complete */
        rpc_async_process_response(req);
        return;
    }

    if (events & BEV_EVENT_TIMEOUT) {
        log_debug("RPC async: timeout for %s:%d",
                  req->client->host, req->client->port);
        rpc_request_complete(req, RPC_ERR_TIMEOUT, "Request timed out", 17);
        return;
    }

    if (events & BEV_EVENT_ERROR) {
        int err = EVUTIL_SOCKET_ERROR();
        log_debug("RPC async: connection error for %s:%d: %s",
                  req->client->host, req->client->port,
                  evutil_socket_error_to_string(err));
        rpc_request_complete(req, RPC_ERR_CONNECT,
                              "Failed to connect to node", 25);
        return;
    }
}

/*
 * Read callback: accumulate response data.
 */
static void rpc_async_read_cb(struct bufferevent *bev, void *ctx)
{
    RPCRequest *req = ctx;
    struct evbuffer *input = bufferevent_get_input(bev);
    size_t avail = evbuffer_get_length(input);

    if (avail == 0) return;

    /* Grow buffer if needed */
    size_t needed = req->response_len + avail + 1;
    if (needed > RPC_MAX_RESPONSE_LEN) {
        log_warn("RPC async: response exceeds %d bytes, truncating",
                 RPC_MAX_RESPONSE_LEN);
        avail = RPC_MAX_RESPONSE_LEN - req->response_len - 1;
        if (avail == 0) return;
    }

    if (needed > req->response_cap) {
        size_t new_cap = req->response_cap ? req->response_cap * 2 : RPC_ASYNC_INITIAL_BUF;
        while (new_cap < needed) new_cap *= 2;
        if (new_cap > RPC_MAX_RESPONSE_LEN) new_cap = RPC_MAX_RESPONSE_LEN;

        char *new_buf = realloc(req->response_buf, new_cap);
        if (!new_buf) {
            rpc_request_complete(req, RPC_ERR_MEMORY,
                                  "Memory allocation failed", 24);
            return;
        }
        req->response_buf = new_buf;
        req->response_cap = new_cap;
    }

    /* Drain data from evbuffer into our response buffer */
    evbuffer_remove(input, req->response_buf + req->response_len, avail);
    req->response_len += avail;
    req->response_buf[req->response_len] = '\0';
}

/*
 * Overall timeout callback.
 */
static void rpc_async_timeout_cb(evutil_socket_t fd, short events, void *ctx)
{
    RPCRequest *req = ctx;
    (void)fd;
    (void)events;

    log_debug("RPC async: overall timeout for %s:%d",
              req->client->host, req->client->port);
    rpc_request_complete(req, RPC_ERR_TIMEOUT, "Request timed out", 17);
}

/*
 * Process a complete HTTP response from the RPC server.
 * Handles HTTP status parsing, auth retry, and JSON-RPC result extraction.
 */
static void rpc_async_process_response(RPCRequest *req)
{
    if (!req->response_buf || req->response_len == 0) {
        rpc_request_complete(req, RPC_ERR_CONNECT,
                              "Empty response from node", 24);
        return;
    }

    /* Parse HTTP status line */
    int http_status = 0;
    if (strncmp(req->response_buf, "HTTP/", 5) == 0) {
        char *status_start = strchr(req->response_buf, ' ');
        if (status_start) {
            http_status = atoi(status_start + 1);
        }
    }

    /* Handle auth failure with cookie retry */
    if ((http_status == 401 || http_status == 403) && !req->auth_retried) {
        if (req->client->cookie_path[0]) {
            log_info("RPC async: auth failed, refreshing cookie...");
            req->auth_retried = 1;

            if (rpc_refresh_cookie(req->client) == 0) {
                /* Reset response state and reconnect */
                free(req->response_buf);
                req->response_buf = NULL;
                req->response_len = 0;
                req->response_cap = 0;

                if (req->bev) {
                    bufferevent_free(req->bev);
                    req->bev = NULL;
                }
                if (req->timeout_ev) {
                    event_free(req->timeout_ev);
                    req->timeout_ev = NULL;
                }

                if (rpc_async_connect(req) == 0) {
                    return;  /* Retry in progress */
                }
            }
        }
        req->client->error_count++;
        rpc_request_complete(req, RPC_ERR_AUTH,
                              "Authentication failed", 21);
        return;
    }

    if (http_status == 401 || http_status == 403) {
        req->client->error_count++;
        rpc_request_complete(req, RPC_ERR_AUTH,
                              "Authentication failed", 21);
        return;
    }

    /* Find body after \r\n\r\n */
    char *body_start = strstr(req->response_buf, "\r\n\r\n");
    if (!body_start) {
        rpc_request_complete(req, RPC_ERR_PARSE,
                              "Malformed HTTP response", 23);
        return;
    }
    body_start += 4;

    /* Parse JSON-RPC response using existing logic */
    char result[4096];
    int is_error;
    int ret = parse_jsonrpc_response(body_start, result, sizeof(result), &is_error);

    req->client->request_count++;

    if (is_error) {
        req->client->error_count++;
        rpc_request_complete(req, ret, result, strlen(result));
    } else {
        req->client->available = 1;
        rpc_request_complete(req, RPC_OK, result, strlen(result));
    }
}

RPCRequest *rpc_manager_broadcast_async(RPCManager *mgr, BitcoinChain chain,
                                         const char *hex_tx,
                                         RPCResultCallback callback,
                                         void *user_data)
{
    if (!mgr->base) {
        log_error("RPC async: no event_base (async not initialized)");
        if (callback) {
            callback(RPC_ERR_CONNECT, "Async RPC not initialized", 25, user_data);
        }
        return NULL;
    }

    RPCClient *client = rpc_manager_get_client(mgr, chain);
    if (!client) {
        log_error("RPC async: no client for %s", network_chain_to_string(chain));
        if (callback) {
            char msg[64];
            int len = snprintf(msg, sizeof(msg), "No RPC configured for %s",
                               network_chain_to_string(chain));
            callback(RPC_ERR_CONNECT, msg, len, user_data);
        }
        return NULL;
    }

    /* Build JSON-RPC request body */
    size_t params_len = strlen(hex_tx) + 8;
    char *params = malloc(params_len);
    if (!params) {
        if (callback) {
            callback(RPC_ERR_MEMORY, "Memory allocation failed", 24, user_data);
        }
        return NULL;
    }
    snprintf(params, params_len, "[\"%s\"]", hex_tx);

    char *body = build_jsonrpc_request("sendrawtransaction", params);
    free(params);
    if (!body) {
        if (callback) {
            callback(RPC_ERR_MEMORY, "Memory allocation failed", 24, user_data);
        }
        return NULL;
    }

    /* Create request */
    RPCRequest *req = calloc(1, sizeof(RPCRequest));
    if (!req) {
        free(body);
        if (callback) {
            callback(RPC_ERR_MEMORY, "Memory allocation failed", 24, user_data);
        }
        return NULL;
    }

    req->client = client;
    req->mgr = mgr;
    req->base = mgr->base;
    req->request_body = body;
    req->request_body_len = strlen(body);
    req->callback = callback;
    req->callback_data = user_data;

    /* Add to active list */
    rpc_request_list_add(mgr, req);
    mgr->total_broadcasts++;

    /* Initiate async connect */
    if (rpc_async_connect(req) < 0) {
        rpc_request_list_remove(mgr, req);
        mgr->failed_broadcasts++;
        if (callback) {
            callback(RPC_ERR_CONNECT, "Failed to connect to node", 25, user_data);
        }
        rpc_request_free(req);
        return NULL;
    }

    return req;
}

void rpc_request_cancel(RPCRequest *req)
{
    if (!req) return;

    /* Prevent callback from firing */
    req->callback = NULL;
    req->callback_data = NULL;

    /* Remove from active list */
    if (req->mgr) {
        rpc_request_list_remove(req->mgr, req);
    }

    rpc_request_free(req);
}

void rpc_manager_cancel_all(RPCManager *mgr)
{
    while (mgr->active_requests) {
        RPCRequest *req = mgr->active_requests;
        rpc_request_cancel(req);
    }
}
