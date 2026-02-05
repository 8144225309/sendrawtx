#ifndef RPC_H
#define RPC_H

#include "network.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/*
 * Bitcoin Core RPC Client - Phase 13c
 *
 * Synchronous JSON-RPC client for Bitcoin Core.
 * Supports both username/password and cookie authentication.
 *
 * Usage:
 *   RPCClient client;
 *   rpc_init(&client, "127.0.0.1", 18443, "user", "pass", CHAIN_REGTEST);
 *
 *   char result[1024];
 *   if (rpc_sendrawtransaction(&client, hex_tx, result, sizeof(result)) == 0) {
 *       printf("TXID: %s\n", result);
 *   }
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
} RPCClient;

/*
 * RPC Manager - handles multiple chain connections.
 */
typedef struct {
    RPCClient mainnet;
    RPCClient testnet;
    RPCClient signet;
    RPCClient regtest;

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

#endif /* RPC_H */
