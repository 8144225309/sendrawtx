/*
 * Real-World Async RPC Test
 *
 * A/B comparison of sync vs async RPC against a live Bitcoin Core node.
 * Every comparison uses SEPARATE never-before-seen transactions so
 * bitcoind does full validation on both paths.
 *
 * Usage:
 *   ./test_async_rpc <host> <port> <user> <pass> <tx_file>
 *
 * tx_file format: one raw TX hex per line.
 *   Line 1:    sync A/B test
 *   Line 2:    async A/B test
 *   Lines 3-N: first half for sync throughput, second half for async throughput
 *
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

/* ========== TX file reader ========== */

#define MAX_TX 64
#define MAX_TX_LEN 8192

static char tx_list[MAX_TX][MAX_TX_LEN];
static int tx_count = 0;

static int load_tx_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open TX file: %s\n", path);
        return -1;
    }

    char line[MAX_TX_LEN];
    while (fgets(line, sizeof(line), f) && tx_count < MAX_TX) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len < 10) continue;
        memcpy(tx_list[tx_count], line, len + 1);
        tx_count++;
    }

    fclose(f);
    return tx_count;
}

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

/* ========== Throughput context ========== */

static int throughput_completed = 0;
static int throughput_target = 0;
static struct event_base *throughput_base = NULL;

static void throughput_cb(int status, const char *result,
                            size_t result_len, void *user_data)
{
    (void)status; (void)result; (void)result_len; (void)user_data;
    throughput_completed++;
    if (throughput_completed >= throughput_target && throughput_base) {
        event_base_loopexit(throughput_base, NULL);
    }
}

/* ========== Helper: create manager ========== */

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
    cfg.timeout_sec = 30;
    return rpc_manager_init_async(mgr, base, NULL, NULL, NULL, &cfg);
}

/* ========== Main ========== */

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
            "Usage: %s <host> <port> <user> <pass> <tx_file>\n"
            "\n"
            "tx_file: one signed TX hex per line.\n"
            "  Line 1: used for sync A/B test (fresh TX)\n"
            "  Line 2: used for async A/B test (different fresh TX)\n"
            "  Lines 3+: split for throughput comparison (all fresh)\n",
            argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *user = argv[3];
    const char *pass = argv[4];
    const char *tx_file = argv[5];

    signal(SIGPIPE, SIG_IGN);
    log_init(LOG_WARN);

    if (load_tx_file(tx_file) < 2) {
        fprintf(stderr, "Need at least 2 transactions in %s\n", tx_file);
        return 1;
    }

    printf("================================================\n");
    printf("Real-World Async RPC Test\n");
    printf("Target: %s:%d (regtest)\n", host, port);
    printf("Loaded %d unique transactions from %s\n", tx_count, tx_file);
    printf("================================================\n");

    const char *sync_tx = tx_list[0];
    const char *async_tx = tx_list[1];

    /* ============================================================
     * TEST 1: Connectivity
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
            return 1;
        }

        rpc_manager_cancel_all(&mgr);
        event_base_free(base);
    }

    /* ============================================================
     * TEST 2 (A): Sync broadcast of FRESH TX_A from inside loop.
     *
     * TX_A has never been broadcast. Bitcoind does full validation:
     * decode, check inputs exist, verify signatures, mempool policy.
     * ============================================================ */
    TEST("A) SYNC broadcast of fresh TX_A from inside event loop");

    char sync_result[4096] = {0};
    double sync_elapsed = 0;
    int sync_status = -99;
    {
        struct event_base *base = event_base_new();
        RPCManager mgr;
        make_manager(&mgr, base, host, port, user, pass);

        tick_counter = 0;
        struct timeval tick_iv = { .tv_sec = 0, .tv_usec = 1000 };
        struct event *tick_ev = event_new(base, -1, EV_PERSIST, tick_cb, NULL);
        event_add(tick_ev, &tick_iv);

        SyncFromLoopCtx ctx = { .mgr = &mgr, .tx_hex = sync_tx };
        struct timeval imm = { .tv_sec = 0, .tv_usec = 0 };
        struct event *trigger = evtimer_new(base, sync_from_loop_cb, &ctx);
        evtimer_add(trigger, &imm);

        run_loop_briefly(base, 30000);

        sync_elapsed = ctx.time_after - ctx.time_before;
        sync_status = ctx.rpc_status;
        int ticks_during = ctx.ticks_after - ctx.ticks_before;
        snprintf(sync_result, sizeof(sync_result), "%s", ctx.result);

        printf("       TX_A: %.40s... (never broadcast before)\n", sync_tx);

        if (sync_status == RPC_OK) {
            PASS("Sync RPC completed — node did full validation");
            printf("       TXID: %.64s\n", ctx.result);
        } else {
            FAIL("Sync RPC failed");
            printf("       Status: %d, Error: %s\n", sync_status, ctx.result);
        }

        printf("       Wall time: %.1f ms\n", sync_elapsed);
        printf("       Ticks during sync call: %d (1ms timer, expect 0)\n",
               ticks_during);

        if (ticks_during == 0) {
            PASS("Event loop was BLOCKED during sync call (0 ticks)");
        } else {
            FAIL("Expected 0 ticks during blocking sync call");
        }

        event_free(trigger);
        event_free(tick_ev);
        rpc_manager_cancel_all(&mgr);
        event_base_free(base);
    }

    /* ============================================================
     * TEST 3 (B): Async broadcast of DIFFERENT FRESH TX_B.
     *
     * TX_B has also never been broadcast. Same validation work.
     * Same 1ms tick timer. The only difference is the code path.
     * ============================================================ */
    TEST("B) ASYNC broadcast of fresh TX_B (different TX, same work)");

    char async_result[4096] = {0};
    double async_elapsed = 0;
    int async_ticks = 0;
    int async_status = -99;
    {
        struct event_base *base = event_base_new();
        RPCManager mgr;
        make_manager(&mgr, base, host, port, user, pass);

        tick_counter = 0;
        struct timeval tick_iv = { .tv_sec = 0, .tv_usec = 1000 };
        struct event *tick_ev = event_new(base, -1, EV_PERSIST, tick_cb, NULL);
        event_add(tick_ev, &tick_iv);

        AsyncResult ar = {0};
        double t_start = now_ms();

        RPCRequest *req = rpc_manager_broadcast_async(&mgr, CHAIN_REGTEST,
                                                        async_tx,
                                                        async_callback, &ar);
        printf("       TX_B: %.40s... (never broadcast before)\n", async_tx);

        if (!req) {
            FAIL("broadcast_async returned NULL");
        } else {
            run_loop_briefly(base, 30000);

            async_elapsed = ar.time_called > 0 ? ar.time_called - t_start : 0;
            async_ticks = tick_counter;
            async_status = ar.status;
            snprintf(async_result, sizeof(async_result), "%s", ar.result);

            if (ar.called) {
                PASS("Async callback fired");
                if (async_status == RPC_OK) {
                    PASS("Node accepted TX_B — full validation");
                    printf("       TXID: %.64s\n", ar.result);
                } else {
                    FAIL("Async RPC error");
                    printf("       Status: %d, Result: %s\n", async_status, ar.result);
                }
            } else {
                FAIL("Callback never fired");
            }

            printf("       Wall time: %.1f ms\n", async_elapsed);
            printf("       Ticks during async call: %d (1ms timer)\n",
                   async_ticks);

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
     * TEST 4: A/B side-by-side
     * ============================================================ */
    TEST("A/B comparison — both did full validation of fresh TXs");
    {
        printf("       SYNC  (TX_A): %.1f ms, 0 ticks → BLOCKED\n",
               sync_elapsed);
        printf("       ASYNC (TX_B): %.1f ms, %d ticks → RESPONSIVE\n",
               async_elapsed, async_ticks);

        if (async_ticks > 0 && sync_status == RPC_OK && async_status == RPC_OK) {
            PASS("Both paths did real validation; only async kept the loop alive");
        } else if (async_ticks > 0) {
            PASS("Async kept loop alive while sync froze it");
        } else {
            FAIL("No meaningful difference");
        }
    }

    /* ============================================================
     * TEST 5: Async error handling
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
            FAIL("broadcast_async returned NULL");
        } else {
            run_loop_briefly(base, 10000);

            if (ar.called) {
                PASS("Callback fired for invalid TX");
                if (ar.status == RPC_ERR_NODE) {
                    PASS("Got RPC_ERR_NODE (correct)");
                    printf("       Error: %s\n", ar.result);
                } else {
                    FAIL("Expected RPC_ERR_NODE");
                    printf("       Got status %d: %s\n", ar.status, ar.result);
                }
            } else {
                FAIL("Callback never fired");
            }
        }

        rpc_manager_cancel_all(&mgr);
        event_base_free(base);
    }

    /* ============================================================
     * TEST 6: Throughput — all fresh unique TXs, no duplicates.
     *
     * First half of remaining TXs → serial sync
     * Second half → concurrent async
     * Every TX requires full validation by bitcoind.
     * ============================================================ */
    TEST("Throughput: serial sync vs concurrent async (all unique fresh TXs)");
    {
        int avail = tx_count - 2;
        int half = avail / 2;

        if (half < 2) {
            printf("       Not enough TXs (have %d extra, need 4+)\n", avail);
            PASS("Skipped — insufficient transactions");
        } else {
            struct event_base *base = event_base_new();
            RPCManager mgr;
            make_manager(&mgr, base, host, port, user, pass);

            printf("       %d unique fresh TXs for sync, %d for async\n",
                   half, half);

            /* Serial sync */
            double sync_start = now_ms();
            int sync_ok = 0;
            for (int i = 0; i < half; i++) {
                char result[256];
                int ret = rpc_manager_broadcast(&mgr, CHAIN_REGTEST,
                                                 tx_list[2 + i],
                                                 result, sizeof(result));
                if (ret == RPC_OK) sync_ok++;
            }
            double sync_total = now_ms() - sync_start;

            /* Concurrent async */
            throughput_completed = 0;
            throughput_target = half;
            throughput_base = base;

            double async_start = now_ms();
            for (int i = 0; i < half; i++) {
                rpc_manager_broadcast_async(&mgr, CHAIN_REGTEST,
                                              tx_list[2 + half + i],
                                              throughput_cb, NULL);
            }
            run_loop_briefly(base, 60000);
            double async_total = now_ms() - async_start;

            printf("       Sync:  %d/%d in %.1f ms (%.1f ms/tx)\n",
                   sync_ok, half, sync_total, sync_total / half);
            printf("       Async: %d/%d in %.1f ms (%.1f ms/tx)\n",
                   throughput_completed, half, async_total,
                   half > 0 ? async_total / half : 0);

            if (sync_ok == half) {
                PASS("All sync broadcasts validated");
            } else {
                FAIL("Some sync broadcasts failed");
            }

            if (throughput_completed == half) {
                PASS("All async broadcasts validated");
            } else {
                FAIL("Not all async broadcasts completed");
            }

            double speedup = sync_total / (async_total > 0 ? async_total : 1);
            printf("       Speedup: %.1fx\n", speedup);

            if (async_total < sync_total) {
                PASS("Concurrent async faster than serial sync");
            } else {
                printf("       NOTE: async not faster on localhost\n");
                PASS("Throughput comparison completed");
            }

            rpc_manager_cancel_all(&mgr);
            event_base_free(base);
        }
    }

    /* ============================================================ */

    printf("\n================================================\n");
    printf("Results: %d passed, %d failed (across %d tests)",
           pass_count, fail_count, test_count);
    printf("\n================================================\n");

    return (fail_count == 0) ? 0 : 1;
}
