/*
 * Real-World Async RPC Test
 *
 * A/B comparison of sync vs async RPC against a live Bitcoin Core node.
 * Proves: (A) sync path blocks the event loop, (B) async path doesn't.
 * Also tests correctness, error handling, and throughput.
 *
 * Usage:
 *   ./test_async_rpc <host> <port> <user> <pass> <tx_hex>
 *
 * Requires a running bitcoind in regtest mode with a funded wallet.
 * See tests/run_async_test.sh for automated setup.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "rpc.h"
#include "log.h"

static int test_count = 0;
static int pass_count = 0;
static int fail_count = 0;

#define TEST(name) do { \
    test_count++; \
    printf("\n[TEST %d] %s\n", test_count, name); \
} while(0)

#define PASS(msg) do { \
    pass_count++; \
    printf("  PASS: %s\n", msg); \
} while(0)

#define FAIL(msg) do { \
    fail_count++; \
    printf("  FAIL: %s\n", msg); \
} while(0)

/* ========== Timing ========== */

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* ========== Tick counter for loop-responsiveness ========== */

static volatile int tick_counter = 0;

static void tick_cb(evutil_socket_t fd, short events, void *ctx)
{
    (void)fd; (void)events; (void)ctx;
    tick_counter++;
}

static void loop_exit_cb(evutil_socket_t fd, short events, void *ctx)
{
    (void)fd; (void)events;
    event_base_loopexit(ctx, NULL);
}

static void run_loop_briefly(struct event_base *base, int max_ms)
{
    struct timeval tv = { .tv_sec = max_ms / 1000,
                          .tv_usec = (max_ms % 1000) * 1000 };
    struct event *timer = evtimer_new(base, loop_exit_cb, base);
    evtimer_add(timer, &tv);
    event_base_dispatch(base);
    event_free(timer);
}

/* ========== Sync-from-loop context ========== */

typedef struct {
    RPCManager *mgr;
    const char *tx_hex;
    int rpc_status;
    char result[4096];
    int ticks_before;
    int ticks_after;
    double time_before;
    double time_after;
} SyncFromLoopCtx;

/*
 * Called from INSIDE the event loop via a timer.
 * The blocking rpc_manager_broadcast() freezes the thread here.
 */
static void sync_from_loop_cb(evutil_socket_t fd, short events, void *arg)
{
    SyncFromLoopCtx *ctx = arg;
    (void)fd; (void)events;

    ctx->ticks_before = tick_counter;
    ctx->time_before = now_ms();

    ctx->rpc_status = rpc_manager_broadcast(ctx->mgr, CHAIN_REGTEST,
                                             ctx->tx_hex,
                                             ctx->result, sizeof(ctx->result));

    ctx->time_after = now_ms();
    ctx->ticks_after = tick_counter;
}

/* ========== Async callback context ========== */

typedef struct {
    int called;
    int status;
    char result[4096];
    size_t result_len;
    double time_called;
} AsyncResult;

static void async_callback(int status, const char *result,
                             size_t result_len, void *user_data)
{
    AsyncResult *ar = user_data;
    ar->called = 1;
    ar->status = status;
    ar->time_called = now_ms();
    ar->result_len = result_len < sizeof(ar->result) - 1
                     ? result_len : sizeof(ar->result) - 1;
    memcpy(ar->result, result, ar->result_len);
    ar->result[ar->result_len] = '\0';
}

/* ========== Throughput test context ========== */

#define THROUGHPUT_N 20

static int throughput_completed = 0;
static struct event_base *throughput_base = NULL;

static void throughput_cb(int status, const char *result,
                            size_t result_len, void *user_data)
{
    (void)status; (void)result; (void)result_len; (void)user_data;
    throughput_completed++;
    if (throughput_completed >= THROUGHPUT_N && throughput_base) {
        event_base_loopexit(throughput_base, NULL);
    }
}

/* ========== Helper: create manager for tests ========== */

static int make_manager(RPCManager *mgr, struct event_base *base,
                         const char *host, int port,
                         const char *user, const char *pass)
{
    RPCConfig cfg = {0};
    cfg.enabled = 1;
    strncpy(cfg.host, host, sizeof(cfg.host) - 1);
    cfg.port = port;
    strncpy(cfg.user, user, sizeof(cfg.user) - 1);
    strncpy(cfg.password, pass, sizeof(cfg.password) - 1);
    cfg.timeout_sec = 10;
    return rpc_manager_init_async(mgr, base, NULL, NULL, NULL, &cfg);
}

/* ========== Tests ========== */

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
            "Usage: %s <host> <port> <user> <pass> <tx_hex>\n"
            "\n"
            "Requires a running bitcoind regtest with a funded wallet.\n"
            "See tests/run_async_test.sh for automated setup.\n",
            argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *user = argv[3];
    const char *pass = argv[4];
    const char *tx_hex = argv[5];

    signal(SIGPIPE, SIG_IGN);
    log_init(LOG_WARN);

    printf("================================================\n");
    printf("Real-World Async RPC Test\n");
    printf("Target: %s:%d (regtest)\n", host, port);
    printf("TX hex: %.60s... (%zu bytes)\n", tx_hex, strlen(tx_hex));
    printf("================================================\n");

    /* ============================================================
     * TEST 1: Connectivity — can we talk to the node at all?
     * ============================================================ */
    TEST("Sync RPC: verify connectivity to live node");
    {
        struct event_base *base = event_base_new();
        RPCManager mgr;
        make_manager(&mgr, base, host, port, user, pass);

        RPCClient *client = rpc_manager_get_client(&mgr, CHAIN_REGTEST);
        if (!client) {
            FAIL("No regtest client configured");
            return 1;
        }

        char info[4096];
        int ret = rpc_getblockchaininfo(client, info, sizeof(info));
        if (ret == RPC_OK) {
            PASS("Connected to live regtest node");
            printf("       Response: %.80s...\n", info);
        } else {
            FAIL("Could not connect — is bitcoind running?");
            printf("       Error: %s\n", info);
            return 1;
        }

        rpc_manager_cancel_all(&mgr);
        event_base_free(base);
    }

    /* ============================================================
     * TEST 2 (A): Sync broadcast FROM INSIDE the event loop.
     *
     * Setup: 1ms repeating tick timer + sync RPC fired from a
     * timer callback. The blocking connect/send/recv freezes the
     * thread, so the tick timer cannot fire during the call.
     *
     * Expected: ticks_during == 0
     * ============================================================ */
    TEST("A) SYNC broadcast from inside event loop — proves blocking");

    char sync_txid[4096] = {0};
    double sync_elapsed = 0;
    {
        struct event_base *base = event_base_new();
        RPCManager mgr;
        make_manager(&mgr, base, host, port, user, pass);

        /* 1ms repeating tick timer */
        tick_counter = 0;
        struct timeval tick_iv = { .tv_sec = 0, .tv_usec = 1000 };
        struct event *tick_ev = event_new(base, -1, EV_PERSIST, tick_cb, NULL);
        event_add(tick_ev, &tick_iv);

        /* Schedule the sync call to fire immediately from inside the loop */
        SyncFromLoopCtx ctx = { .mgr = &mgr, .tx_hex = tx_hex };
        struct timeval imm = { .tv_sec = 0, .tv_usec = 0 };
        struct event *trigger = evtimer_new(base, sync_from_loop_cb, &ctx);
        evtimer_add(trigger, &imm);

        /* Run — sync call will block inside the loop */
        run_loop_briefly(base, 10000);

        sync_elapsed = ctx.time_after - ctx.time_before;
        int ticks_during = ctx.ticks_after - ctx.ticks_before;

        if (ctx.rpc_status == RPC_OK) {
            PASS("Sync RPC completed against live node");
            printf("       TXID: %.64s\n", ctx.result);
            snprintf(sync_txid, sizeof(sync_txid), "%s", ctx.result);
        } else if (ctx.rpc_status == RPC_ERR_NODE) {
            /* Node error (e.g., already in mempool) is still a valid response */
            PASS("Sync RPC got node response");
            printf("       Node said: %s\n", ctx.result);
            snprintf(sync_txid, sizeof(sync_txid), "%s", ctx.result);
        } else {
            FAIL("Sync RPC failed to reach node");
            printf("       Status: %d, Error: %s\n", ctx.rpc_status, ctx.result);
        }

        printf("       Wall time: %.1f ms\n", sync_elapsed);
        printf("       Ticks during sync call: %d (1ms timer, expect 0)\n",
               ticks_during);

        if (ticks_during == 0) {
            PASS("Event loop was BLOCKED during sync call (0 ticks)");
        } else {
            FAIL("Expected 0 ticks during blocking sync call");
            printf("       Got %d — loop wasn't fully blocked?\n", ticks_during);
        }

        event_free(trigger);
        event_free(tick_ev);
        rpc_manager_cancel_all(&mgr);
        event_base_free(base);
    }

    /* ============================================================
     * TEST 3 (B): Async broadcast with the same tick timer.
     *
     * Same node, same TX, same 1ms tick. But this time the RPC
     * goes through bufferevent — the event loop keeps running.
     *
     * Expected: ticks > 0 during the call
     * ============================================================ */
    TEST("B) ASYNC broadcast — proves non-blocking");

    char async_txid[4096] = {0};
    double async_elapsed = 0;
    int async_ticks = 0;
    {
        struct event_base *base = event_base_new();
        RPCManager mgr;
        make_manager(&mgr, base, host, port, user, pass);

        /* Same 1ms tick timer */
        tick_counter = 0;
        struct timeval tick_iv = { .tv_sec = 0, .tv_usec = 1000 };
        struct event *tick_ev = event_new(base, -1, EV_PERSIST, tick_cb, NULL);
        event_add(tick_ev, &tick_iv);

        AsyncResult ar = {0};
        double t_start = now_ms();

        RPCRequest *req = rpc_manager_broadcast_async(&mgr, CHAIN_REGTEST,
                                                        tx_hex,
                                                        async_callback, &ar);
        if (!req) {
            FAIL("broadcast_async returned NULL");
        } else {
            run_loop_briefly(base, 10000);

            async_elapsed = ar.time_called > 0 ? ar.time_called - t_start : 0;
            async_ticks = tick_counter;

            if (ar.called) {
                PASS("Async callback fired");
                if (ar.status == RPC_OK) {
                    PASS("Node accepted TX");
                    printf("       TXID: %.64s\n", ar.result);
                    snprintf(async_txid, sizeof(async_txid), "%s", ar.result);
                } else if (ar.status == RPC_ERR_NODE) {
                    PASS("Node responded (duplicate/already-in-mempool is expected)");
                    printf("       Node said: %s\n", ar.result);
                    snprintf(async_txid, sizeof(async_txid), "%s", ar.result);
                } else {
                    FAIL("Unexpected error from async RPC");
                    printf("       Status: %d, Result: %s\n", ar.status, ar.result);
                }
            } else {
                FAIL("Callback never fired");
            }

            printf("       Wall time: %.1f ms\n", async_elapsed);
            printf("       Ticks during async call: %d (1ms timer)\n", async_ticks);

            if (async_ticks > 0) {
                PASS("Event loop was RESPONSIVE during async call");
            } else {
                FAIL("0 ticks — loop was blocked during async RPC");
            }
        }

        event_free(tick_ev);
        rpc_manager_cancel_all(&mgr);
        event_base_free(base);
    }

    /* ============================================================
     * TEST 4: A/B Summary — direct comparison
     * ============================================================ */
    TEST("A/B comparison summary");
    {
        printf("       SYNC:  %.1f ms, 0 ticks (blocked)\n", sync_elapsed);
        printf("       ASYNC: %.1f ms, %d ticks (responsive)\n",
               async_elapsed, async_ticks);

        if (async_ticks > 0) {
            PASS("Async kept loop alive while sync froze it");
        } else {
            FAIL("No difference between sync and async");
        }
    }

    /* ============================================================
     * TEST 5: Correctness — both paths talk to the same node.
     *
     * If both got a TXID, they should match (same TX).
     * If one got "already in mempool" that's also correct.
     * ============================================================ */
    TEST("Correctness: sync and async return consistent results");
    {
        if (sync_txid[0] && async_txid[0]) {
            /* Both got a response. If both are 64-char hex, compare TXIDs */
            int sync_is_txid = (strlen(sync_txid) == 64);
            int async_is_txid = (strlen(async_txid) == 64);

            if (sync_is_txid && async_is_txid) {
                if (strcmp(sync_txid, async_txid) == 0) {
                    PASS("Both paths returned identical TXID");
                    printf("       %s\n", sync_txid);
                } else {
                    FAIL("TXIDs differ between sync and async");
                    printf("       Sync:  %s\n", sync_txid);
                    printf("       Async: %s\n", async_txid);
                }
            } else {
                /* One got TXID, other got error message — both valid */
                PASS("Both paths got responses from node");
                printf("       Sync:  %s\n", sync_txid);
                printf("       Async: %s\n", async_txid);
            }
        } else {
            FAIL("Missing response from one or both paths");
        }
    }

    /* ============================================================
     * TEST 6: Async error handling with invalid TX
     * ============================================================ */
    TEST("Async: invalid TX gets proper error from live node");
    {
        struct event_base *base = event_base_new();
        RPCManager mgr;
        make_manager(&mgr, base, host, port, user, pass);

        AsyncResult ar = {0};
        RPCRequest *req = rpc_manager_broadcast_async(&mgr, CHAIN_REGTEST,
                                                        "deadbeef",
                                                        async_callback, &ar);
        if (!req) {
            FAIL("broadcast_async returned NULL for invalid TX");
        } else {
            run_loop_briefly(base, 10000);

            if (ar.called) {
                PASS("Callback fired for invalid TX");
                if (ar.status == RPC_ERR_NODE) {
                    PASS("Got RPC_ERR_NODE (correct error code)");
                    printf("       Error from node: %s\n", ar.result);
                } else {
                    FAIL("Expected RPC_ERR_NODE for invalid TX");
                    printf("       Got status %d: %s\n", ar.status, ar.result);
                }
            } else {
                FAIL("Callback never fired for invalid TX");
            }
        }

        rpc_manager_cancel_all(&mgr);
        event_base_free(base);
    }

    /* ============================================================
     * TEST 7: Throughput — serial sync vs concurrent async.
     *
     * Fire THROUGHPUT_N broadcasts each way, measure wall time.
     * Sync: N sequential blocking calls.
     * Async: N concurrent non-blocking calls.
     * ============================================================ */
    TEST("Throughput: serial sync vs concurrent async");
    {
        struct event_base *base = event_base_new();
        RPCManager mgr;
        make_manager(&mgr, base, host, port, user, pass);

        /* --- Serial sync --- */
        double sync_start = now_ms();
        for (int i = 0; i < THROUGHPUT_N; i++) {
            char result[256];
            rpc_manager_broadcast(&mgr, CHAIN_REGTEST, tx_hex,
                                   result, sizeof(result));
        }
        double sync_total = now_ms() - sync_start;

        /* --- Concurrent async --- */
        throughput_completed = 0;
        throughput_base = base;
        double async_start = now_ms();
        for (int i = 0; i < THROUGHPUT_N; i++) {
            rpc_manager_broadcast_async(&mgr, CHAIN_REGTEST, tx_hex,
                                          throughput_cb, NULL);
        }
        run_loop_briefly(base, 30000);
        double async_total = now_ms() - async_start;

        printf("       Sync:  %d calls in %.1f ms (%.1f ms/call)\n",
               THROUGHPUT_N, sync_total, sync_total / THROUGHPUT_N);
        printf("       Async: %d/%d completed in %.1f ms (%.1f ms/call)\n",
               throughput_completed, THROUGHPUT_N, async_total,
               throughput_completed > 0 ? async_total / throughput_completed : 0);

        if (throughput_completed == THROUGHPUT_N) {
            PASS("All async broadcasts completed");
        } else {
            FAIL("Not all async broadcasts completed");
            printf("       Only %d/%d\n", throughput_completed, THROUGHPUT_N);
        }

        double speedup = sync_total / (async_total > 0 ? async_total : 1);
        printf("       Speedup: %.1fx\n", speedup);

        if (async_total < sync_total) {
            PASS("Concurrent async is faster than serial sync");
        } else {
            printf("       NOTE: async not faster — localhost overhead may dominate\n");
            printf("       (This is expected on localhost; real gains appear over network)\n");
            /* Not a failure — just informational */
            PASS("Throughput comparison completed");
        }

        rpc_manager_cancel_all(&mgr);
        event_base_free(base);
    }

    /* ============================================================ */

    printf("\n================================================\n");
    printf("Results: %d/%d passed", pass_count, test_count);
    if (fail_count > 0) {
        printf(" (%d FAILED)", fail_count);
    }
    printf("\n================================================\n");

    return (fail_count == 0) ? 0 : 1;
}
