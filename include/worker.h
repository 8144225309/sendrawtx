#ifndef WORKER_H
#define WORKER_H

#include "config.h"
#include "static_files.h"
#include "slot_manager.h"
#include "rate_limiter.h"
#include "ip_acl.h"
#include "tls.h"
#include "rpc.h"
#include <stdint.h>
#include <stdbool.h>
#include <event2/event.h>
#include <event2/listener.h>

/*
 * Worker process - handles connections independently.
 *
 * Each worker:
 * - Runs on dedicated CPU core
 * - Has own SO_REUSEPORT socket
 * - Has own event loop
 * - Has NO shared state with other workers
 */

/* Forward declaration */
struct Connection;

typedef struct WorkerProcess {
    /* Identity */
    int worker_id;
    int cpu_core;

    /* Event loop */
    struct event_base *base;
    struct evconnlistener *listener;
    struct evconnlistener *tls_listener;  /* TLS listener (port 8443) */

    /* Configuration (read-only after init) */
    Config *config;

    /* Static files (loaded per-worker, no locks needed) */
    StaticFiles static_files;

    /* Slot manager (per-worker connection limits) */
    SlotManager slots;

    /* Rate limiter (per-worker, per-IP limits) */
    RateLimiter rate_limiter;

    /* IP ACL (blocklist/allowlist per-worker) */
    IpACLContext ip_acl;

    /* TLS context (per-worker, used for TLS connections) */
    TLSContext tls;

    /* RPC manager for Bitcoin node connections (Phase 13c) */
    RPCManager rpc;

    /* State flags */
    volatile bool draining;
    bool listener_disabled;
    bool tls_listener_disabled;

    /* Statistics */
    uint64_t connections_accepted;
    uint64_t connections_rejected_rate;
    uint64_t connections_rejected_slot;
    uint64_t connections_rejected_blocked;   /* Blocked by IP blocklist */
    uint64_t connections_allowlisted;        /* Bypassed rate limiting via allowlist */
    uint64_t requests_processed;
    int active_connections;

    /* Process info (Phase 5 metrics) */
    struct timespec start_time;        /* Worker start time for uptime (monotonic) */
    time_t start_wallclock;            /* Wall-clock epoch for Prometheus metrics */

    /* Request latency histogram (Phase 5)
     * Buckets: 1ms, 5ms, 10ms, 50ms, 100ms, 500ms, 1s, 5s, +Inf */
    uint64_t latency_bucket_1ms;       /* le="0.001" */
    uint64_t latency_bucket_5ms;       /* le="0.005" */
    uint64_t latency_bucket_10ms;      /* le="0.01" */
    uint64_t latency_bucket_50ms;      /* le="0.05" */
    uint64_t latency_bucket_100ms;     /* le="0.1" */
    uint64_t latency_bucket_500ms;     /* le="0.5" */
    uint64_t latency_bucket_1s;        /* le="1" */
    uint64_t latency_bucket_5s;        /* le="5" */
    uint64_t latency_bucket_inf;       /* le="+Inf" */
    double latency_sum_seconds;        /* Sum of all request durations */

    /* Status code counters (Phase 5) */
    uint64_t status_2xx;               /* 200-299 */
    uint64_t status_4xx;               /* 400-499 */
    uint64_t status_5xx;               /* 500-599 */

    /* Specific status codes for detailed metrics */
    uint64_t status_200;
    uint64_t status_400;
    uint64_t status_404;
    uint64_t status_408;               /* Timeout */
    uint64_t status_429;               /* Rate limited */
    uint64_t status_503;               /* Service unavailable */

    /* Request method counters (Phase 5) */
    uint64_t method_get;
    uint64_t method_post;
    uint64_t method_other;

    /* TLS metrics (Phase 5) */
    uint64_t tls_handshakes_total;
    uint64_t tls_handshake_errors;
    uint64_t tls_protocol_tls12;
    uint64_t tls_protocol_tls13;

    /* HTTP/2 metrics (Phase 5) */
    uint64_t h2_streams_total;
    int h2_streams_active;
    uint64_t h2_rst_stream_total;
    uint64_t h2_goaway_sent;

    /* Error type counters (Phase 5) */
    uint64_t errors_timeout;
    uint64_t errors_parse;
    uint64_t errors_tls;

    /* Per-endpoint counters */
    uint64_t endpoint_health;
    uint64_t endpoint_ready;
    uint64_t endpoint_alive;
    uint64_t endpoint_version;
    uint64_t endpoint_metrics;
    uint64_t endpoint_home;
    uint64_t endpoint_broadcast;
    uint64_t endpoint_result;
    uint64_t endpoint_docs;
    uint64_t endpoint_status;
    uint64_t endpoint_acme;

    /* Extended metrics */
    uint64_t response_bytes_total;       /* Total response bytes sent */
    uint64_t slowloris_kills;            /* Slowloris detections */
    uint64_t slot_promotion_failures;    /* Tier promotion failures (no slots) */
    uint64_t keepalive_reuses;           /* Requests served on reused connections */

    /* Active connections list (intrusive linked list) */
    struct Connection *connections;

    /* Signal handling events */
    struct event *signal_event;         /* SIGUSR1 - graceful drain */
    struct event *signal_event_reload;  /* SIGUSR2 - TLS cert reload */

    /* Cleanup timer event (for rate limiter) */
    struct event *cleanup_event;
} WorkerProcess;

/*
 * Worker main entry point.
 * Called after fork() in child process.
 * Does not return (calls exit()).
 */
void worker_main(int worker_id, Config *config);

/*
 * Get number of available CPUs.
 */
int get_num_cpus(void);

/*
 * Pin current process to specific CPU core.
 * Returns 0 on success, -1 on error.
 */
int pin_to_cpu(int cpu);

/*
 * Check if worker should exit (draining with no connections).
 * Called after a connection closes.
 */
void worker_check_drain(WorkerProcess *worker);

#endif /* WORKER_H */
