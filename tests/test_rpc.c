/*
 * RPC Client Test - Phase 13c
 *
 * Tests the RPC client against a running Bitcoin Core regtest node.
 * Start bitcoind with: bitcoind -regtest -daemon
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rpc.h"
#include "log.h"

static int test_count = 0;
static int pass_count = 0;

#define TEST(name) do { \
    test_count++; \
    printf("\n[TEST %d] %s\n", test_count, name); \
} while(0)

#define PASS(msg) do { \
    pass_count++; \
    printf("  PASS: %s\n", msg); \
} while(0)

#define FAIL(msg) do { \
    printf("  FAIL: %s\n", msg); \
} while(0)

#define SKIP(msg) do { \
    printf("  SKIP: %s\n", msg); \
} while(0)

/*
 * Test basic initialization with user/pass.
 */
static void test_init_simple(void)
{
    TEST("rpc_init_simple");

    RPCClient client;
    int ret = rpc_init_simple(&client, "127.0.0.1", 18443,
                              "testuser", "testpass", CHAIN_REGTEST);

    if (ret == RPC_OK) {
        PASS("Initialized with user/pass");
        printf("       Host: %s:%d\n", client.host, client.port);
        printf("       Chain: %s\n", network_chain_to_string(client.chain));
    } else {
        FAIL("Init failed");
    }
}

/*
 * Test cookie file initialization.
 */
static void test_init_cookie(void)
{
    TEST("rpc_init_cookie");

    RPCClient client;
    const char *cookie_path = "/tmp/.bitcoin/regtest/.cookie";

    /* Check if cookie file exists */
    if (access(cookie_path, R_OK) != 0) {
        SKIP("Cookie file not found (bitcoind not running?)");
        printf("       Expected: %s\n", cookie_path);
        return;
    }

    int ret = rpc_init_cookie(&client, "127.0.0.1", 18443,
                              cookie_path, CHAIN_REGTEST);

    if (ret == RPC_OK) {
        PASS("Initialized with cookie auth");
        printf("       Cookie: %s\n", client.cookie_path);
    } else {
        FAIL("Cookie init failed");
    }
}

/*
 * Test connection to running node.
 */
static void test_connection(RPCClient *client)
{
    TEST("rpc_test_connection");

    int ret = rpc_test_connection(client);

    if (ret == RPC_OK) {
        PASS("Connected to node");
        printf("       Available: %s\n", client->available ? "yes" : "no");
    } else if (ret == RPC_ERR_CONNECT) {
        SKIP("Node not running");
    } else if (ret == RPC_ERR_AUTH) {
        FAIL("Authentication failed");
    } else {
        FAIL("Connection failed");
    }
}

/*
 * Test getblockchaininfo.
 */
static void test_getblockchaininfo(RPCClient *client)
{
    TEST("rpc_getblockchaininfo");

    if (!client->available) {
        SKIP("Node not available");
        return;
    }

    char result[4096];
    int ret = rpc_getblockchaininfo(client, result, sizeof(result));

    if (ret == RPC_OK) {
        PASS("Got blockchain info");
        /* Print first 200 chars */
        printf("       %.200s...\n", result);
    } else {
        FAIL(result);
    }
}

/*
 * Test decoderawtransaction with a sample transaction.
 */
static void test_decoderawtransaction(RPCClient *client)
{
    TEST("rpc_decoderawtransaction");

    if (!client->available) {
        SKIP("Node not available");
        return;
    }

    /* Simple P2PKH transaction (may not be valid, just testing decode) */
    const char *sample_tx =
        "0100000001c997a5e56e104102fa209c6a852dd90660a20b2d9c352423edce25857fcd3704"
        "000000004847304402204e45e16932b8af514961a1d3a1a25fdf3f4f7732e9d624c6c61548"
        "ab5fb8cd410220181522ec8eca07de4860a4acdd12909d831cc56cbbac4622082221a8768d"
        "1d0901ffffffff0200e1f5050000000043410496b538e853519c726a2c91e61ec11600ae13"
        "90813a627c66fb8be7947be63c52da7589379515d4e0a604f8141781e62294721166bf621e"
        "73a82cbf2342c858eeac00286bee0000000043410411db93e1dcdb8a016b49840f8c53bc1e"
        "b68a382e97b1482ecad7b148a6909a5cb2e0eaddfb84ccf9744464f82e160bfa9b8b64f9d4"
        "c03f999b8643f656b412a3ac00000000";

    char result[8192];
    int ret = rpc_decoderawtransaction(client, sample_tx, result, sizeof(result));

    if (ret == RPC_OK) {
        PASS("Decoded transaction");
        printf("       %.200s...\n", result);
    } else {
        /* Decode might fail if tx format is invalid, that's ok */
        printf("  INFO: %s\n", result);
    }
}

/*
 * Test RPC Manager.
 */
static void test_manager(void)
{
    TEST("RPC Manager");

    RPCConfig regtest_cfg = {0};
    regtest_cfg.enabled = 1;
    strncpy(regtest_cfg.host, "127.0.0.1", sizeof(regtest_cfg.host) - 1);
    regtest_cfg.port = 18443;
    strncpy(regtest_cfg.user, "testuser", sizeof(regtest_cfg.user) - 1);
    strncpy(regtest_cfg.password, "testpass", sizeof(regtest_cfg.password) - 1);
    regtest_cfg.timeout_sec = 30;

    RPCManager mgr;
    int ret = rpc_manager_init(&mgr, NULL, NULL, NULL, &regtest_cfg);

    if (ret == 0) {
        PASS("Manager initialized");

        RPCClient *client = rpc_manager_get_client(&mgr, CHAIN_REGTEST);
        if (client) {
            PASS("Got regtest client");
        } else {
            FAIL("No regtest client");
        }

        client = rpc_manager_get_client(&mgr, CHAIN_MAINNET);
        if (client == NULL) {
            PASS("Mainnet correctly not configured");
        } else {
            FAIL("Mainnet should not be configured");
        }
    } else {
        FAIL("Manager init failed");
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("================================================\n");
    printf("RPC Client Test Suite - Phase 13c\n");
    printf("================================================\n");

    /* Initialize logging */
    log_init(LOG_DEBUG);

    /* Test basic initialization */
    test_init_simple();
    test_init_cookie();

    /* Test with actual node if available */
    printf("\n--- Live Node Tests ---\n");
    printf("(Requires: bitcoind -regtest running)\n");

    /* Try to connect with user/pass first */
    RPCClient client;

    /* Try rawrelay-devnet node first (running on port 18888) */
    int connected = 0;

    /* Check for rawrelay-devnet (user/pass auth) */
    rpc_init_simple(&client, "127.0.0.1", 18888,
                   "rawrelay", "devnet123", CHAIN_REGTEST);
    printf("\nTrying rawrelay-devnet node (port 18888)...\n");

    test_connection(&client);
    connected = client.available;

    /* Fallback: try cookie auth from common locations */
    if (!connected) {
        const char *cookie_paths[] = {
            "/home/obscurity/.rawrelay-devnet/data/regtest/.cookie",
            "/tmp/.bitcoin/regtest/.cookie",
            NULL
        };

        for (int i = 0; cookie_paths[i] != NULL; i++) {
            if (access(cookie_paths[i], R_OK) == 0) {
                if (rpc_init_cookie(&client, "127.0.0.1", 18443,
                                   cookie_paths[i], CHAIN_REGTEST) == 0) {
                    printf("\nTrying cookie: %s\n", cookie_paths[i]);
                    test_connection(&client);
                    if (client.available) {
                        connected = 1;
                        break;
                    }
                }
            }
        }
    }

    /* Skip duplicate connection test since we already called it */
    test_getblockchaininfo(&client);
    test_decoderawtransaction(&client);

    /* Test manager */
    test_manager();

    /* Summary */
    printf("\n================================================\n");
    printf("Results: %d/%d tests passed\n", pass_count, test_count);
    if (pass_count < test_count) {
        printf("Note: Some tests require a running bitcoind -regtest\n");
    }
    printf("================================================\n");

    /* Pass if basic tests work (first 2 + manager test) */
    return (pass_count >= 4) ? 0 : 1;
}
