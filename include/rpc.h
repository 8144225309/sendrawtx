#ifndef RPC_H
#define RPC_H

#include "network.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>

/* Forward declarations for libevent types */
struct event_base;
struct bufferevent;
struct event;

/*
 * Bitcoin Core RPC Client - Phase 13c + async extension
 *
 * Synchronous JSON-RPC client for Bitcoin Core (startup/testing).
 * Asynchronous bufferevent-based client for event loop use.
 * Supports both username/password and cookie authentication.
 *
 * Usage (sync):
 *   RPCClient client;
 *   rpc_init(&client, "127.0.0.1", 18443, "user", "pass", CHAIN_REGTEST);
 *
 *   char result[1024];
 *   if (rpc_sendrawtransaction(&client, hex_tx, result, sizeof(result)) == 0) {
 *       printf("TXID: %s\n", result);
 *   }
 *
 * Usage (async):
 *   rpc_manager_init_async(&mgr, base, ...);
 *   rpc_manager_broadcast_async(&mgr, chain, hex_tx, my_callback, my_data);
 */

/* Maximum sizes */
#define RPC_MAX_HOST_LEN 256
#define RPC_MAX_AUTH_LEN 512
#define RPC_MAX_WALLET_LEN 64
#define RPC_MAX_RESPONSE_LEN (4 * 1024 * 1024)  /* 4MB for large responses */

/* RPC error codes */
#define RPC_OK              0
#define RPC_ERR_CONNECT    -1   /* Failed to connect */
#define RPC_ERR_AUTH       -2   /* Authentication failed (401/403) */
#define RPC_ERR_TIMEOUT    -3   /* Request timed out */
#define RPC_ERR_PARSE      -4   /* Failed to parse response */
#define RPC_ERR_NODE       -5   /* Node returned an error */
#define RPC_ERR_MEMORY     -6   /* Memory allocation failed */
#define RPC_ERR_COOKIE     -7   /* Failed to read cookie file */
#define RPC_ERR_CANCELLED  -8   /* Request was cancelled */

/* Async completion callback */
typedef void (*RPCResultCallback)(int status, const char *result,
                                   size_t result_len, void *user_data);

/* Forward declaration for RPCRequest */
typedef struct RPCRequest RPCRequest;

/*
 * RPC connection configuration.
 */
typedef struct {
    int enabled;                        /* Is this chain enabled? */
    char host[RPC_MAX_HOST_LEN];        /* RPC host */
    int port;                           /* RPC port */
    char user[64];                      /* RPC username (if not using cookie) */
    char password[64];                  /* RPC password (if not using cookie) */
    char cookie_file[256];              /* Path to .cookie file */
    char datadir[256];                  /* Bitcoin datadir (for auto cookie path) */
    int timeout_sec;                    /* Request timeout (default: 30) */
    char wallet[RPC_MAX_WALLET_LEN];    /* Wallet name (optional) */
} RPCConfig;

/*
 * RPC client handle.
 */
typedef struct {
    char host[RPC_MAX_HOST_LEN];
    int port;
    char auth_header[RPC_MAX_AUTH_LEN]; /* "Basic <base64>" */
    int timeout_sec;
    char wallet[RPC_MAX_WALLET_LEN];
    BitcoinChain chain;

    /* Connection state */
    int available;                      /* Is connection working? */
    uint64_t request_count;             /* Total requests made */
    uint64_t error_count;               /* Total errors */

    /* Cookie auth state */
    char cookie_path[256];              /* Path to cookie file */
    time_t cookie_mtime;                /* Last modified time of cookie */

    /* Pre-resolved address for async connections */
    struct sockaddr_storage resolved_addr;
    socklen_t resolved_addr_len;
} RPCClient;

/*
 * RPC Manager - handles multiple chain connections.
 */
typedef struct {
    RPCClient mainnet;
    RPCClient testnet;
    RPCClient signet;
    RPCClient regtest;

    /* Async state */
    struct event_base *base;            /* NULL = sync-only mode */
    RPCRequest *active_requests;        /* Head of active doubly-linked list */

    /* Stats */
    uint64_t total_broadcasts;
    uint64_t successful_broadcasts;
    uint64_t failed_broadcasts;
} RPCManager;

/* ========== Initialization ========== */

/*
 * Initialize RPC client from config.
 * Returns 0 on success, error code on failure.
 */
int rpc_init(RPCClient *client, const RPCConfig *config, BitcoinChain chain);

/*
 * Initialize RPC client with explicit credentials.
 */
int rpc_init_simple(RPCClient *client, const char *host, int port,
                    const char *user, const char *password, BitcoinChain chain);

/*
 * Initialize RPC client with cookie auth.
 */
int rpc_init_cookie(RPCClient *client, const char *host, int port,
                    const char *cookie_path, BitcoinChain chain);

/*
 * Refresh cookie authentication (re-read cookie file).
 * Call this if you get auth errors - bitcoind regenerates cookie on restart.
 */
int rpc_refresh_cookie(RPCClient *client);

/*
 * Test RPC connection.
 * Returns 0 if connection is working.
 */
int rpc_test_connection(RPCClient *client);

/* ========== Bitcoin RPC Methods ========== */

/*
 * Send raw transaction to the network.
 *
 * Parameters:
 *   client    - RPC client
 *   hex_tx    - Raw transaction in hex format
 *   result    - Buffer for txid (64 chars + null) or error message
 *   result_sz - Size of result buffer
 *
 * Returns:
 *   0 on success (result contains txid)
 *   RPC_ERR_* on failure (result contains error message)
 */
int rpc_sendrawtransaction(RPCClient *client, const char *hex_tx,
                           char *result, size_t result_sz);

/*
 * Get blockchain info (chain, blocks, headers, etc.)
 * Result is raw JSON response.
 */
int rpc_getblockchaininfo(RPCClient *client, char *result, size_t result_sz);

/*
 * Get raw transaction by txid.
 * Returns hex-encoded transaction if found.
 */
int rpc_getrawtransaction(RPCClient *client, const char *txid,
                          char *result, size_t result_sz);

/*
 * Get mempool entry for a transaction.
 * Returns JSON with fee info, etc.
 */
int rpc_getmempoolentry(RPCClient *client, const char *txid,
                        char *result, size_t result_sz);

/*
 * Decode raw transaction without sending.
 * Returns JSON with parsed transaction structure.
 */
int rpc_decoderawtransaction(RPCClient *client, const char *hex_tx,
                             char *result, size_t result_sz);

/*
 * Test mempool acceptance without broadcasting.
 * Useful for validation before broadcast.
 */
int rpc_testmempoolaccept(RPCClient *client, const char *hex_tx,
                          char *result, size_t result_sz);

/* ========== RPC Manager ========== */

/*
 * Initialize RPC manager with config for all chains.
 */
int rpc_manager_init(RPCManager *mgr,
                     const RPCConfig *mainnet,
                     const RPCConfig *testnet,
                     const RPCConfig *signet,
                     const RPCConfig *regtest);

/*
 * Get RPC client for a specific chain.
 * Returns NULL if chain is not configured/enabled.
 */
RPCClient *rpc_manager_get_client(RPCManager *mgr, BitcoinChain chain);

/*
 * Broadcast transaction to appropriate chain.
 * Auto-detects chain from transaction addresses if in mixed mode.
 */
int rpc_manager_broadcast(RPCManager *mgr, BitcoinChain chain,
                          const char *hex_tx, char *txid, size_t txid_sz);

/*
 * Log RPC manager status.
 */
void rpc_manager_log_status(RPCManager *mgr);

/* ========== Async RPC API ========== */

/*
 * In-flight async RPC request.
 */
struct RPCRequest {
    RPCClient *client;
    RPCManager *mgr;
    struct bufferevent *bev;
    struct event *timeout_ev;
    struct event_base *base;

    /* Request */
    char *request_body;
    size_t request_body_len;

    /* Response accumulation */
    char *response_buf;
    size_t response_len;
    size_t response_cap;

    /* Callback */
    RPCResultCallback callback;
    void *callback_data;

    /* State */
    int auth_retried;               /* Cookie refresh retry flag */

    /* Active list (doubly-linked, intrusive) */
    RPCRequest *next;
    RPCRequest *prev;
};

/*
 * Initialize RPC manager for async operation.
 * Like rpc_manager_init() but stores event_base and pre-resolves
 * hostnames via getaddrinfo() (safe to call before seccomp).
 *
 * Must be called AFTER event_base_new() creates the loop.
 */
int rpc_manager_init_async(RPCManager *mgr,
                            struct event_base *base,
                            const RPCConfig *mainnet,
                            const RPCConfig *testnet,
                            const RPCConfig *signet,
                            const RPCConfig *regtest);

/*
 * Broadcast a raw transaction asynchronously.
 * Callback fires when the RPC completes (or fails/times out).
 * Returns the RPCRequest handle (for cancellation), or NULL on error.
 */
RPCRequest *rpc_manager_broadcast_async(RPCManager *mgr, BitcoinChain chain,
                                         const char *hex_tx,
                                         RPCResultCallback callback,
                                         void *user_data);

/*
 * Cancel an in-flight async request.
 * The callback will NOT be fired after cancellation.
 */
void rpc_request_cancel(RPCRequest *req);

/*
 * Cancel all in-flight async requests.
 * Call during shutdown before destroying the event_base.
 */
void rpc_manager_cancel_all(RPCManager *mgr);

#endif /* RPC_H */
