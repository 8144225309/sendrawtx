#include "endpoints.h"
#include "worker.h"
#include "connection.h"
#include "http2.h"
#include "router.h"
#include "slot_manager.h"
#include "rate_limiter.h"
#include "tls.h"
#include "hex.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

/*
 * Get number of open file descriptors for this process.
 */
static int get_open_fds(void)
{
#ifdef __linux__
    int count = 0;
    DIR *dir = opendir("/proc/self/fd");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] != '.') {
                count++;
            }
        }
        closedir(dir);
    }
    return count;
#else
    return -1;  /* Not available on this platform */
#endif
}

/*
 * Get maximum file descriptors for this process.
 */
static int get_max_fds(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        return (int)rl.rlim_cur;
    }
    return -1;
}

/*
 * Generate /health JSON response body.
 */
int generate_health_body(WorkerProcess *worker, char *buf, size_t bufsize)
{
    /* Calculate uptime */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long uptime_sec = now.tv_sec - worker->start_time.tv_sec;

    /* Get FD counts */
    int open_fds = get_open_fds();
    int max_fds = get_max_fds();
    double fd_usage_pct = (max_fds > 0) ? (100.0 * open_fds / max_fds) : 0.0;

    /* Check if TLS is enabled and get cert expiry */
    int tls_enabled = (worker->tls.ctx != NULL) ? 1 : 0;
    time_t cert_expiry = tls_get_cert_expiry(&worker->tls);
    int cert_days_remaining = 0;
    int cert_warning = 0;
    if (cert_expiry > 0) {
        cert_days_remaining = (int)((cert_expiry - time(NULL)) / 86400);
        cert_warning = (cert_days_remaining < 30) ? 1 : 0;
    }

    int body_len = snprintf(buf, bufsize,
        "{\"status\":\"healthy\","
        "\"worker_id\":%d,"
        "\"uptime_seconds\":%ld,"
        "\"active_connections\":%d,"
        "\"requests_processed\":%lu,"
        "\"slots\":{"
            "\"normal\":{\"used\":%d,\"max\":%d},"
            "\"large\":{\"used\":%d,\"max\":%d},"
            "\"huge\":{\"used\":%d,\"max\":%d}"
        "},"
        "\"rate_limiter_entries\":%d,"
        "\"tls\":{\"enabled\":%s,\"cert_expires_in_days\":%d,\"cert_expiry_warning\":%s},"
        "\"resources\":{"
            "\"open_fds\":%d,"
            "\"max_fds\":%d,"
            "\"fd_usage_percent\":%.1f"
        "}}",
        worker->worker_id,
        uptime_sec,
        worker->active_connections,
        (unsigned long)worker->requests_processed,
        slot_manager_current(&worker->slots, TIER_NORMAL),
        slot_manager_max(&worker->slots, TIER_NORMAL),
        slot_manager_current(&worker->slots, TIER_LARGE),
        slot_manager_max(&worker->slots, TIER_LARGE),
        slot_manager_current(&worker->slots, TIER_HUGE),
        slot_manager_max(&worker->slots, TIER_HUGE),
        rate_limiter_get_entry_count(&worker->rate_limiter),
        tls_enabled ? "true" : "false",
        cert_days_remaining,
        cert_warning ? "true" : "false",
        open_fds,
        max_fds,
        fd_usage_pct);

    if (body_len < 0)
        return 0;
    if ((size_t)body_len >= bufsize)
        body_len = (int)(bufsize - 1);

    return body_len;
}

/*
 * Generate /metrics Prometheus response body.
 */
int generate_metrics_body(WorkerProcess *worker, char *buf, size_t bufsize)
{
    size_t offset = 0;
    size_t remaining = bufsize - 1;  /* Reserve space for null terminator */
    int n;

    /* Helper macro to safely advance buffer position */
    #define METRICS_ADVANCE() do { \
        if (n > 0 && (size_t)n < remaining) { \
            offset += (size_t)n; \
            remaining -= (size_t)n; \
        } else { \
            remaining = 0; \
        } \
    } while(0)

    /* Calculate uptime */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double uptime_sec = (now.tv_sec - worker->start_time.tv_sec) +
                        (now.tv_nsec - worker->start_time.tv_nsec) / 1e9;

    /* Get FD counts */
    int open_fds = get_open_fds();
    int max_fds = get_max_fds();

    /* === Basic Counters === */
    n = snprintf(buf + offset, remaining,
        "# HELP rawrelay_requests_total Total requests processed\n"
        "# TYPE rawrelay_requests_total counter\n"
        "rawrelay_requests_total{worker=\"%d\"} %lu\n"
        "\n"
        "# HELP rawrelay_connections_accepted_total Total connections accepted\n"
        "# TYPE rawrelay_connections_accepted_total counter\n"
        "rawrelay_connections_accepted_total{worker=\"%d\"} %lu\n"
        "\n"
        "# HELP rawrelay_connections_rejected_total Rejected connections by reason\n"
        "# TYPE rawrelay_connections_rejected_total counter\n"
        "rawrelay_connections_rejected_total{worker=\"%d\",reason=\"rate_limit\"} %lu\n"
        "rawrelay_connections_rejected_total{worker=\"%d\",reason=\"slot_limit\"} %lu\n"
        "rawrelay_connections_rejected_total{worker=\"%d\",reason=\"blocked\"} %lu\n"
        "\n"
        "# HELP rawrelay_connections_allowlisted_total Connections that bypassed rate limiting\n"
        "# TYPE rawrelay_connections_allowlisted_total counter\n"
        "rawrelay_connections_allowlisted_total{worker=\"%d\"} %lu\n"
        "\n"
        "# HELP rawrelay_active_connections Current active connections\n"
        "# TYPE rawrelay_active_connections gauge\n"
        "rawrelay_active_connections{worker=\"%d\"} %d\n"
        "\n",
        worker->worker_id, (unsigned long)worker->requests_processed,
        worker->worker_id, (unsigned long)worker->connections_accepted,
        worker->worker_id, (unsigned long)worker->connections_rejected_rate,
        worker->worker_id, (unsigned long)worker->connections_rejected_slot,
        worker->worker_id, (unsigned long)worker->connections_rejected_blocked,
        worker->worker_id, (unsigned long)worker->connections_allowlisted,
        worker->worker_id, worker->active_connections);
    METRICS_ADVANCE();

    /* === Request Latency Histogram === */
    n = snprintf(buf + offset, remaining,
        "# HELP rawrelay_request_duration_seconds Request latency histogram\n"
        "# TYPE rawrelay_request_duration_seconds histogram\n"
        "rawrelay_request_duration_seconds_bucket{worker=\"%d\",le=\"0.001\"} %lu\n"
        "rawrelay_request_duration_seconds_bucket{worker=\"%d\",le=\"0.005\"} %lu\n"
        "rawrelay_request_duration_seconds_bucket{worker=\"%d\",le=\"0.01\"} %lu\n"
        "rawrelay_request_duration_seconds_bucket{worker=\"%d\",le=\"0.05\"} %lu\n"
        "rawrelay_request_duration_seconds_bucket{worker=\"%d\",le=\"0.1\"} %lu\n"
        "rawrelay_request_duration_seconds_bucket{worker=\"%d\",le=\"0.5\"} %lu\n"
        "rawrelay_request_duration_seconds_bucket{worker=\"%d\",le=\"1\"} %lu\n"
        "rawrelay_request_duration_seconds_bucket{worker=\"%d\",le=\"5\"} %lu\n"
        "rawrelay_request_duration_seconds_bucket{worker=\"%d\",le=\"+Inf\"} %lu\n"
        "rawrelay_request_duration_seconds_sum{worker=\"%d\"} %.6f\n"
        "rawrelay_request_duration_seconds_count{worker=\"%d\"} %lu\n"
        "\n",
        worker->worker_id, (unsigned long)worker->latency_bucket_1ms,
        worker->worker_id, (unsigned long)worker->latency_bucket_5ms,
        worker->worker_id, (unsigned long)worker->latency_bucket_10ms,
        worker->worker_id, (unsigned long)worker->latency_bucket_50ms,
        worker->worker_id, (unsigned long)worker->latency_bucket_100ms,
        worker->worker_id, (unsigned long)worker->latency_bucket_500ms,
        worker->worker_id, (unsigned long)worker->latency_bucket_1s,
        worker->worker_id, (unsigned long)worker->latency_bucket_5s,
        worker->worker_id, (unsigned long)worker->latency_bucket_inf,
        worker->worker_id, worker->latency_sum_seconds,
        worker->worker_id, (unsigned long)worker->latency_bucket_inf);
    METRICS_ADVANCE();

    /* === HTTP Status Code Counters === */
    n = snprintf(buf + offset, remaining,
        "# HELP rawrelay_http_requests_total HTTP requests by status code\n"
        "# TYPE rawrelay_http_requests_total counter\n"
        "rawrelay_http_requests_total{worker=\"%d\",status=\"200\"} %lu\n"
        "rawrelay_http_requests_total{worker=\"%d\",status=\"400\"} %lu\n"
        "rawrelay_http_requests_total{worker=\"%d\",status=\"404\"} %lu\n"
        "rawrelay_http_requests_total{worker=\"%d\",status=\"408\"} %lu\n"
        "rawrelay_http_requests_total{worker=\"%d\",status=\"429\"} %lu\n"
        "rawrelay_http_requests_total{worker=\"%d\",status=\"503\"} %lu\n"
        "\n"
        "# HELP rawrelay_http_requests_by_class_total HTTP requests by status class\n"
        "# TYPE rawrelay_http_requests_by_class_total counter\n"
        "rawrelay_http_requests_by_class_total{worker=\"%d\",class=\"2xx\"} %lu\n"
        "rawrelay_http_requests_by_class_total{worker=\"%d\",class=\"4xx\"} %lu\n"
        "rawrelay_http_requests_by_class_total{worker=\"%d\",class=\"5xx\"} %lu\n"
        "\n",
        worker->worker_id, (unsigned long)worker->status_200,
        worker->worker_id, (unsigned long)worker->status_400,
        worker->worker_id, (unsigned long)worker->status_404,
        worker->worker_id, (unsigned long)worker->status_408,
        worker->worker_id, (unsigned long)worker->status_429,
        worker->worker_id, (unsigned long)worker->status_503,
        worker->worker_id, (unsigned long)worker->status_2xx,
        worker->worker_id, (unsigned long)worker->status_4xx,
        worker->worker_id, (unsigned long)worker->status_5xx);
    METRICS_ADVANCE();

    /* === Request Method Counters === */
    n = snprintf(buf + offset, remaining,
        "# HELP rawrelay_requests_by_method_total HTTP requests by method\n"
        "# TYPE rawrelay_requests_by_method_total counter\n"
        "rawrelay_requests_by_method_total{worker=\"%d\",method=\"GET\"} %lu\n"
        "rawrelay_requests_by_method_total{worker=\"%d\",method=\"POST\"} %lu\n"
        "rawrelay_requests_by_method_total{worker=\"%d\",method=\"OTHER\"} %lu\n"
        "\n",
        worker->worker_id, (unsigned long)worker->method_get,
        worker->worker_id, (unsigned long)worker->method_post,
        worker->worker_id, (unsigned long)worker->method_other);
    METRICS_ADVANCE();

    /* === Process Info === */
    n = snprintf(buf + offset, remaining,
        "# HELP rawrelay_process_start_time_seconds Unix timestamp of process start\n"
        "# TYPE rawrelay_process_start_time_seconds gauge\n"
        "rawrelay_process_start_time_seconds{worker=\"%d\"} %ld\n"
        "\n"
        "# HELP rawrelay_process_uptime_seconds Process uptime in seconds\n"
        "# TYPE rawrelay_process_uptime_seconds gauge\n"
        "rawrelay_process_uptime_seconds{worker=\"%d\"} %.3f\n"
        "\n",
        worker->worker_id, (long)worker->start_wallclock,
        worker->worker_id, uptime_sec);
    METRICS_ADVANCE();

    /* === File Descriptor Metrics === */
    if (open_fds >= 0 && max_fds >= 0) {
        n = snprintf(buf + offset, remaining,
            "# HELP rawrelay_open_fds Current number of open file descriptors\n"
            "# TYPE rawrelay_open_fds gauge\n"
            "rawrelay_open_fds{worker=\"%d\"} %d\n"
            "\n"
            "# HELP rawrelay_max_fds Maximum file descriptors allowed\n"
            "# TYPE rawrelay_max_fds gauge\n"
            "rawrelay_max_fds{worker=\"%d\"} %d\n"
            "\n",
            worker->worker_id, open_fds,
            worker->worker_id, max_fds);
        METRICS_ADVANCE();
    }

    /* === TLS Metrics === */
    n = snprintf(buf + offset, remaining,
        "# HELP rawrelay_tls_handshakes_total TLS handshakes by protocol version\n"
        "# TYPE rawrelay_tls_handshakes_total counter\n"
        "rawrelay_tls_handshakes_total{worker=\"%d\",protocol=\"TLSv1.2\"} %lu\n"
        "rawrelay_tls_handshakes_total{worker=\"%d\",protocol=\"TLSv1.3\"} %lu\n"
        "\n"
        "# HELP rawrelay_tls_handshake_errors_total TLS handshake errors\n"
        "# TYPE rawrelay_tls_handshake_errors_total counter\n"
        "rawrelay_tls_handshake_errors_total{worker=\"%d\"} %lu\n"
        "\n",
        worker->worker_id, (unsigned long)worker->tls_protocol_tls12,
        worker->worker_id, (unsigned long)worker->tls_protocol_tls13,
        worker->worker_id, (unsigned long)worker->tls_handshake_errors);
    METRICS_ADVANCE();

    /* === TLS Certificate Expiry === */
    time_t cert_expiry = tls_get_cert_expiry(&worker->tls);
    if (cert_expiry > 0) {
        n = snprintf(buf + offset, remaining,
            "# HELP rawrelay_tls_cert_expiry_timestamp_seconds Unix timestamp when certificate expires\n"
            "# TYPE rawrelay_tls_cert_expiry_timestamp_seconds gauge\n"
            "rawrelay_tls_cert_expiry_timestamp_seconds{worker=\"%d\"} %ld\n"
            "\n",
            worker->worker_id, (long)cert_expiry);
        METRICS_ADVANCE();
    }

    /* === HTTP/2 Metrics === */
    n = snprintf(buf + offset, remaining,
        "# HELP rawrelay_http2_streams_total Total HTTP/2 streams opened\n"
        "# TYPE rawrelay_http2_streams_total counter\n"
        "rawrelay_http2_streams_total{worker=\"%d\"} %lu\n"
        "\n"
        "# HELP rawrelay_http2_streams_active Current active HTTP/2 streams\n"
        "# TYPE rawrelay_http2_streams_active gauge\n"
        "rawrelay_http2_streams_active{worker=\"%d\"} %d\n"
        "\n"
        "# HELP rawrelay_http2_rst_stream_total HTTP/2 RST_STREAM frames sent\n"
        "# TYPE rawrelay_http2_rst_stream_total counter\n"
        "rawrelay_http2_rst_stream_total{worker=\"%d\"} %lu\n"
        "\n"
        "# HELP rawrelay_http2_goaway_total HTTP/2 GOAWAY frames sent\n"
        "# TYPE rawrelay_http2_goaway_total counter\n"
        "rawrelay_http2_goaway_total{worker=\"%d\"} %lu\n"
        "\n",
        worker->worker_id, (unsigned long)worker->h2_streams_total,
        worker->worker_id, worker->h2_streams_active,
        worker->worker_id, (unsigned long)worker->h2_rst_stream_total,
        worker->worker_id, (unsigned long)worker->h2_goaway_sent);
    METRICS_ADVANCE();

    /* === Error Type Counters === */
    n = snprintf(buf + offset, remaining,
        "# HELP rawrelay_errors_total Errors by type\n"
        "# TYPE rawrelay_errors_total counter\n"
        "rawrelay_errors_total{worker=\"%d\",type=\"timeout\"} %lu\n"
        "rawrelay_errors_total{worker=\"%d\",type=\"parse_error\"} %lu\n"
        "rawrelay_errors_total{worker=\"%d\",type=\"tls_error\"} %lu\n"
        "\n",
        worker->worker_id, (unsigned long)worker->errors_timeout,
        worker->worker_id, (unsigned long)worker->errors_parse,
        worker->worker_id, (unsigned long)worker->errors_tls);
    METRICS_ADVANCE();

    /* === Slot Metrics === */
    n = snprintf(buf + offset, remaining,
        "# HELP rawrelay_slots_used Slots currently in use by tier\n"
        "# TYPE rawrelay_slots_used gauge\n"
        "rawrelay_slots_used{worker=\"%d\",tier=\"normal\"} %d\n"
        "rawrelay_slots_used{worker=\"%d\",tier=\"large\"} %d\n"
        "rawrelay_slots_used{worker=\"%d\",tier=\"huge\"} %d\n"
        "\n"
        "# HELP rawrelay_slots_max Maximum slots by tier\n"
        "# TYPE rawrelay_slots_max gauge\n"
        "rawrelay_slots_max{worker=\"%d\",tier=\"normal\"} %d\n"
        "rawrelay_slots_max{worker=\"%d\",tier=\"large\"} %d\n"
        "rawrelay_slots_max{worker=\"%d\",tier=\"huge\"} %d\n"
        "\n"
        "# HELP rawrelay_rate_limiter_entries Current rate limiter table size\n"
        "# TYPE rawrelay_rate_limiter_entries gauge\n"
        "rawrelay_rate_limiter_entries{worker=\"%d\"} %d\n",
        worker->worker_id, slot_manager_current(&worker->slots, TIER_NORMAL),
        worker->worker_id, slot_manager_current(&worker->slots, TIER_LARGE),
        worker->worker_id, slot_manager_current(&worker->slots, TIER_HUGE),
        worker->worker_id, slot_manager_max(&worker->slots, TIER_NORMAL),
        worker->worker_id, slot_manager_max(&worker->slots, TIER_LARGE),
        worker->worker_id, slot_manager_max(&worker->slots, TIER_HUGE),
        worker->worker_id, rate_limiter_get_entry_count(&worker->rate_limiter));
    METRICS_ADVANCE();

    /* === Extended Metrics === */
    n = snprintf(buf + offset, remaining,
        "\n"
        "# HELP rawrelay_response_bytes_total Total response bytes sent\n"
        "# TYPE rawrelay_response_bytes_total counter\n"
        "rawrelay_response_bytes_total{worker=\"%d\"} %lu\n"
        "\n"
        "# HELP rawrelay_slowloris_kills_total Connections killed by slowloris detection\n"
        "# TYPE rawrelay_slowloris_kills_total counter\n"
        "rawrelay_slowloris_kills_total{worker=\"%d\"} %lu\n"
        "\n"
        "# HELP rawrelay_slot_promotion_failures_total Tier promotion failures due to no slots\n"
        "# TYPE rawrelay_slot_promotion_failures_total counter\n"
        "rawrelay_slot_promotion_failures_total{worker=\"%d\"} %lu\n"
        "\n"
        "# HELP rawrelay_keepalive_reuses_total Requests served on reused keep-alive connections\n"
        "# TYPE rawrelay_keepalive_reuses_total counter\n"
        "rawrelay_keepalive_reuses_total{worker=\"%d\"} %lu\n",
        worker->worker_id, (unsigned long)worker->response_bytes_total,
        worker->worker_id, (unsigned long)worker->slowloris_kills,
        worker->worker_id, (unsigned long)worker->slot_promotion_failures,
        worker->worker_id, (unsigned long)worker->keepalive_reuses);
    METRICS_ADVANCE();

    /* === Per-Endpoint Counters === */
    n = snprintf(buf + offset, remaining,
        "\n"
        "# HELP rawrelay_endpoint_requests_total Requests by endpoint\n"
        "# TYPE rawrelay_endpoint_requests_total counter\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/health\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/ready\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/alive\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/version\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/metrics\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/broadcast\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/result\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/docs\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/status\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/logos\"} %lu\n"
        "rawrelay_endpoint_requests_total{worker=\"%d\",endpoint=\"/acme\"} %lu\n",
        worker->worker_id, (unsigned long)worker->endpoint_health,
        worker->worker_id, (unsigned long)worker->endpoint_ready,
        worker->worker_id, (unsigned long)worker->endpoint_alive,
        worker->worker_id, (unsigned long)worker->endpoint_version,
        worker->worker_id, (unsigned long)worker->endpoint_metrics,
        worker->worker_id, (unsigned long)worker->endpoint_home,
        worker->worker_id, (unsigned long)worker->endpoint_broadcast,
        worker->worker_id, (unsigned long)worker->endpoint_result,
        worker->worker_id, (unsigned long)worker->endpoint_docs,
        worker->worker_id, (unsigned long)worker->endpoint_status,
        worker->worker_id, (unsigned long)worker->endpoint_logos,
        worker->worker_id, (unsigned long)worker->endpoint_acme);
    METRICS_ADVANCE();

    /* === RPC / Bitcoin Node Metrics === */
    {
        RPCManager *rpc = &worker->rpc;
        n = snprintf(buf + offset, remaining,
            "\n"
            "# HELP rawrelay_rpc_broadcasts_total Total transaction broadcast attempts\n"
            "# TYPE rawrelay_rpc_broadcasts_total counter\n"
            "rawrelay_rpc_broadcasts_total{worker=\"%d\"} %lu\n"
            "\n"
            "# HELP rawrelay_rpc_broadcasts_success_total Successful transaction broadcasts\n"
            "# TYPE rawrelay_rpc_broadcasts_success_total counter\n"
            "rawrelay_rpc_broadcasts_success_total{worker=\"%d\"} %lu\n"
            "\n"
            "# HELP rawrelay_rpc_broadcasts_failed_total Failed transaction broadcasts\n"
            "# TYPE rawrelay_rpc_broadcasts_failed_total counter\n"
            "rawrelay_rpc_broadcasts_failed_total{worker=\"%d\"} %lu\n",
            worker->worker_id, (unsigned long)rpc->total_broadcasts,
            worker->worker_id, (unsigned long)rpc->successful_broadcasts,
            worker->worker_id, (unsigned long)rpc->failed_broadcasts);
        METRICS_ADVANCE();

        /* Per-chain RPC client stats */
        struct { const char *name; RPCClient *client; } chains[] = {
            { "mainnet", &rpc->mainnet },
            { "testnet", &rpc->testnet },
            { "signet",  &rpc->signet },
            { "regtest", &rpc->regtest },
        };

        int first_client = 1;
        for (int i = 0; i < 4; i++) {
            if (chains[i].client->host[0] == '\0') continue;
            if (first_client) {
                n = snprintf(buf + offset, remaining,
                    "\n"
                    "# HELP rawrelay_rpc_requests_total Total RPC requests to Bitcoin node\n"
                    "# TYPE rawrelay_rpc_requests_total counter\n");
                METRICS_ADVANCE();
                first_client = 0;
            }
            n = snprintf(buf + offset, remaining,
                "rawrelay_rpc_requests_total{worker=\"%d\",chain=\"%s\"} %lu\n",
                worker->worker_id, chains[i].name,
                (unsigned long)chains[i].client->request_count);
            METRICS_ADVANCE();
        }

        first_client = 1;
        for (int i = 0; i < 4; i++) {
            if (chains[i].client->host[0] == '\0') continue;
            if (first_client) {
                n = snprintf(buf + offset, remaining,
                    "\n"
                    "# HELP rawrelay_rpc_errors_total Total RPC errors by chain\n"
                    "# TYPE rawrelay_rpc_errors_total counter\n");
                METRICS_ADVANCE();
                first_client = 0;
            }
            n = snprintf(buf + offset, remaining,
                "rawrelay_rpc_errors_total{worker=\"%d\",chain=\"%s\"} %lu\n",
                worker->worker_id, chains[i].name,
                (unsigned long)chains[i].client->error_count);
            METRICS_ADVANCE();
        }

        first_client = 1;
        for (int i = 0; i < 4; i++) {
            if (chains[i].client->host[0] == '\0') continue;
            if (first_client) {
                n = snprintf(buf + offset, remaining,
                    "\n"
                    "# HELP rawrelay_rpc_node_up Bitcoin node availability (1=up, 0=down)\n"
                    "# TYPE rawrelay_rpc_node_up gauge\n");
                METRICS_ADVANCE();
                first_client = 0;
            }
            n = snprintf(buf + offset, remaining,
                "rawrelay_rpc_node_up{worker=\"%d\",chain=\"%s\"} %d\n",
                worker->worker_id, chains[i].name,
                chains[i].client->available);
            METRICS_ADVANCE();
        }
    }

    /* Ensure null termination */
    buf[offset] = '\0';

    #undef METRICS_ADVANCE

    return (int)offset;
}

/*
 * Serve ACME HTTP-01 challenge for HTTP/2.
 */
int serve_acme_challenge_h2(Connection *conn, int32_t stream_id,
                            const char *path, size_t path_len,
                            const char *request_id)
{
    WorkerProcess *worker = conn->worker;
    const char *acme_dir = worker->config->acme_challenge_dir;

    /* Extract token from path: /.well-known/acme-challenge/{token} */
    const size_t prefix_len = 27;  /* ".well-known/acme-challenge/" */

    if (path_len < prefix_len + 2 || path[0] != '/') {
        log_warn("ACME H2: Invalid path format from %s", log_format_ip(conn->client_ip));
        goto not_found;
    }

    const char *token = path + 1 + prefix_len;
    size_t token_len = path_len - 1 - prefix_len;

    /* Security: Reject path traversal attempts */
    if (strstr(token, "..") || memchr(token, '/', token_len) || memchr(token, '\\', token_len)) {
        log_warn("ACME H2: Path traversal attempt from %s: %s",
                 log_format_ip(conn->client_ip), path);
        goto not_found;
    }

    /* Security: Token should be base64url encoded (alphanumeric, -, _) */
    for (size_t i = 0; i < token_len; i++) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            log_warn("ACME H2: Invalid token character from %s: '%c'",
                     log_format_ip(conn->client_ip), c);
            goto not_found;
        }
    }

    /* Build full path to challenge file */
    char filepath[512];
    int n = snprintf(filepath, sizeof(filepath), "%s/%.*s",
                     acme_dir, (int)token_len, token);
    if (n < 0 || (size_t)n >= sizeof(filepath)) {
        log_warn("ACME H2: Path too long from %s", log_format_ip(conn->client_ip));
        goto not_found;
    }

    /* Open and read the challenge file */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        log_warn("ACME H2: Challenge file not found: %s (from %s)",
                 filepath, log_format_ip(conn->client_ip));
        goto not_found;
    }

    /* Get file size */
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        log_warn("ACME H2: Cannot stat challenge file: %s", filepath);
        close(fd);
        goto not_found;
    }

    /* Sanity check - ACME tokens are typically < 256 bytes */
    if (st.st_size > 4096) {
        log_warn("ACME H2: Challenge file too large: %s (%ld bytes)",
                 filepath, (long)st.st_size);
        close(fd);
        goto not_found;
    }

    /* Read file content */
    char content[4097];
    ssize_t bytes_read = read(fd, content, st.st_size);
    close(fd);

    if (bytes_read < 0 || bytes_read != st.st_size) {
        log_warn("ACME H2: Failed to read challenge file: %s", filepath);
        goto not_found;
    }

    (void)request_id;  /* Available for future use in response headers */

    log_info("ACME H2: Serving challenge for token %.*s to %s",
             (int)token_len, token, log_format_ip(conn->client_ip));

    h2_send_response(conn, stream_id, 200, "text/plain",
                     (const unsigned char *)content, bytes_read);
    return (int)bytes_read;

not_found:
    h2_send_response(conn, stream_id, 404, "text/plain",
                     (const unsigned char *)"Not Found", 9);
    return -1;
}

/*
 * Validate hex characters in a path buffer.
 * For paths > 64 chars, validates that all characters are valid hex.
 * Allows "tx/" prefix.
 */
int validate_hex_path(const char *path, size_t path_len)
{
    /* Short paths (< 64 chars) could be /tx/txid or other routes - don't validate */
    if (path_len < 64) {
        return 0;
    }

    const char *p = path;
    const char *end = path + path_len;

    /* Allow 'tx/' prefix */
    if (path_len > 3 && p[0] == 't' && p[1] == 'x' && p[2] == '/') {
        p += 3;
    }

    /* Validate remaining characters as hex */
    for (; p < end; p++) {
        if (!is_hex_char((unsigned char)*p)) {
            return -1;
        }
    }

    return 0;
}

/*
 * Update latency histogram bucket based on duration in seconds.
 */
void update_latency_histogram(WorkerProcess *worker, double duration_sec)
{
    /* Increment appropriate bucket (cumulative histogram) */
    if (duration_sec <= 0.001) worker->latency_bucket_1ms++;
    if (duration_sec <= 0.005) worker->latency_bucket_5ms++;
    if (duration_sec <= 0.01) worker->latency_bucket_10ms++;
    if (duration_sec <= 0.05) worker->latency_bucket_50ms++;
    if (duration_sec <= 0.1) worker->latency_bucket_100ms++;
    if (duration_sec <= 0.5) worker->latency_bucket_500ms++;
    if (duration_sec <= 1.0) worker->latency_bucket_1s++;
    if (duration_sec <= 5.0) worker->latency_bucket_5s++;
    worker->latency_bucket_inf++;  /* +Inf always increments */

    worker->latency_sum_seconds += duration_sec;
}

/*
 * Update status code counters.
 */
void update_status_counters(WorkerProcess *worker, int status)
{
    /* Category counters */
    if (status >= 200 && status < 300) {
        worker->status_2xx++;
    } else if (status >= 400 && status < 500) {
        worker->status_4xx++;
    } else if (status >= 500 && status < 600) {
        worker->status_5xx++;
    }

    /* Specific status counters */
    switch (status) {
        case 200: worker->status_200++; break;
        case 400: worker->status_400++; break;
        case 404: worker->status_404++; break;
        case 408: worker->status_408++; break;
        case 429: worker->status_429++; break;
        case 503: worker->status_503++; break;
        default: break;
    }
}

/*
 * Update method counters.
 */
void update_method_counters(WorkerProcess *worker, const char *method)
{
    if (!method || !method[0]) {
        worker->method_other++;
        return;
    }

    if (strcmp(method, "GET") == 0) {
        worker->method_get++;
    } else if (strcmp(method, "POST") == 0) {
        worker->method_post++;
    } else {
        worker->method_other++;
    }
}

/*
 * Update per-endpoint counters.
 */
void update_endpoint_counter(WorkerProcess *worker, RouteType route)
{
    switch (route) {
        case ROUTE_HEALTH:    worker->endpoint_health++; break;
        case ROUTE_READY:     worker->endpoint_ready++; break;
        case ROUTE_ALIVE:     worker->endpoint_alive++; break;
        case ROUTE_VERSION:   worker->endpoint_version++; break;
        case ROUTE_METRICS:   worker->endpoint_metrics++; break;
        case ROUTE_HOME:      worker->endpoint_home++; break;
        case ROUTE_BROADCAST: worker->endpoint_broadcast++; break;
        case ROUTE_RESULT:    worker->endpoint_result++; break;
        case ROUTE_DOCS:      worker->endpoint_docs++; break;
        case ROUTE_STATUS:    worker->endpoint_status++; break;
        case ROUTE_LOGOS:     worker->endpoint_logos++; break;
        case ROUTE_ACME_CHALLENGE: worker->endpoint_acme++; break;
        case ROUTE_ERROR:     break;  /* tracked via 404 status counter */
    }
}

/*
 * Log access entry for a completed request.
 */
void log_request_access(const char *client_ip, const char *method,
                        const char *path, int status, size_t bytes_sent,
                        double duration_ms, const char *request_id)
{
    log_access(client_ip, method, path, status, bytes_sent, duration_ms, request_id);
}
