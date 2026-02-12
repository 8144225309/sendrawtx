#include "worker.h"
#include "connection.h"
#include "tcp_opts.h"
#include "static_files.h"
#include "tls.h"
#include "security.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sched.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/bufferevent_ssl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

/* Global worker pointer (reserved for future signal handler use) */
static WorkerProcess *g_worker = NULL;

/* Forward declarations */
static void accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *addr, int socklen, void *ctx);
static void tls_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                          struct sockaddr *addr, int socklen, void *ctx);
static void accept_error_cb(struct evconnlistener *listener, void *ctx);
static void signal_cb(evutil_socket_t sig, short events, void *ctx);
static void signal_reload_cb(evutil_socket_t sig, short events, void *ctx);
static void cleanup_timer_cb(evutil_socket_t fd, short events, void *ctx);
static void send_403_response(int fd);
static void send_503_response(int fd);
static void send_429_response(int fd);
static int create_tls_reuseport_socket(Config *config);

int get_num_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}

int pin_to_cpu(int cpu)
{
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
        log_warn("Failed to pin to CPU %d: %s", cpu, strerror(errno));
        return -1;
    }
    return 0;
#else
    /* CPU affinity not available on this platform */
    (void)cpu;
    log_warn("CPU affinity not supported on this platform");
    return 0;
#endif
}

/*
 * Create SO_REUSEPORT listener socket.
 * Each worker creates its own socket binding to the same port.
 */
static int create_reuseport_socket_on_port(int port)
{
    int fd;
    int opt = 1;
    struct sockaddr_in6 addr;

    /* Create IPv6 socket (accepts IPv4 via dual-stack) */
    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("socket() failed: %s", strerror(errno));
        return -1;
    }

    /* Set non-blocking mode (portable - works on Linux and macOS) */
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        log_error("fcntl(O_NONBLOCK) failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* SO_REUSEPORT - the key to multi-process architecture! */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        log_error("SO_REUSEPORT failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Also set SO_REUSEADDR for faster restart */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Allow IPv4 connections on IPv6 socket (dual-stack) */
    opt = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));

    /* Bind to all interfaces */
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("bind() failed on port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 1024) < 0) {
        log_error("listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int create_reuseport_socket(Config *config)
{
    return create_reuseport_socket_on_port(config->listen_port);
}

static int create_tls_reuseport_socket(Config *config)
{
    return create_reuseport_socket_on_port(config->tls_port);
}

/*
 * Set up signal handling for worker.
 */
static void setup_worker_signals(WorkerProcess *worker)
{
    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Handle SIGUSR1 for graceful drain via libevent */
    worker->signal_event = evsignal_new(worker->base, SIGUSR1, signal_cb, worker);
    if (worker->signal_event) {
        event_add(worker->signal_event, NULL);
    }

    /* Handle SIGUSR2 for TLS certificate reload via libevent */
    worker->signal_event_reload = evsignal_new(worker->base, SIGUSR2, signal_reload_cb, worker);
    if (worker->signal_event_reload) {
        event_add(worker->signal_event_reload, NULL);
    }
}

/*
 * Signal callback for SIGUSR1 (graceful drain).
 */
static void signal_cb(evutil_socket_t sig, short events, void *ctx)
{
    WorkerProcess *worker = ctx;
    (void)sig;
    (void)events;

    log_info("Received SIGUSR1, starting graceful drain");
    worker->draining = true;
    worker_check_drain(worker);
}

/*
 * Signal callback for SIGUSR2 (TLS certificate reload).
 * Used for ACME certificate renewal - certbot can trigger this after renewal.
 */
static void signal_reload_cb(evutil_socket_t sig, short events, void *ctx)
{
    WorkerProcess *worker = ctx;
    (void)sig;
    (void)events;

    log_info("Received SIGUSR2, reloading TLS certificates");

    if (!worker->config->tls_enabled) {
        log_warn("TLS not enabled, ignoring reload signal");
        return;
    }

    if (tls_context_reload(&worker->tls, worker->config) < 0) {
        log_error("Failed to reload TLS certificates");
    }
}

/*
 * Check if we should exit (draining and no active connections).
 * Exported for use by connection.c
 */
void worker_check_drain(WorkerProcess *worker)
{
    if (!worker->draining) {
        return;
    }

    /* Stop accepting new connections */
    if (!worker->listener_disabled && worker->listener) {
        evconnlistener_disable(worker->listener);
        worker->listener_disabled = true;
        log_info("Stopped accepting new connections");
    }

    /* Stop accepting new TLS connections */
    if (!worker->tls_listener_disabled && worker->tls_listener) {
        evconnlistener_disable(worker->tls_listener);
        worker->tls_listener_disabled = true;
        log_info("Stopped accepting new TLS connections");
    }

    /* Exit if no active connections */
    if (worker->active_connections == 0) {
        log_info("No active connections, exiting");
        event_base_loopexit(worker->base, NULL);
    }
}

/*
 * Send 503 Service Unavailable response.
 * Used when slot limits are reached.
 */
static void send_503_response(int fd)
{
    const char *response =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 20\r\n"
        "Connection: close\r\n"
        "Retry-After: 5\r\n"
        "\r\n"
        "Service Unavailable\n";
    /* Best-effort send - ignore result since we're closing anyway */
    ssize_t n __attribute__((unused)) = write(fd, response, strlen(response));
}

/*
 * Send 429 Too Many Requests response.
 * Used when rate limit is exceeded.
 */
static void send_429_response(int fd)
{
    const char *response =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 18\r\n"
        "Connection: close\r\n"
        "Retry-After: 1\r\n"
        "\r\n"
        "Too Many Requests\n";
    /* Best-effort send - ignore result since we're closing anyway */
    ssize_t n __attribute__((unused)) = write(fd, response, strlen(response));
}

/*
 * Send 403 Forbidden response.
 * Used when IP is blocked by ACL.
 */
static void send_403_response(int fd)
{
    const char *response =
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 10\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Forbidden\n";
    /* Best-effort send - ignore result since we're closing anyway */
    ssize_t n __attribute__((unused)) = write(fd, response, strlen(response));
}

/*
 * Periodic cleanup timer callback.
 * Cleans up expired rate limiter entries.
 */
static void cleanup_timer_cb(evutil_socket_t fd, short events, void *ctx)
{
    WorkerProcess *worker = ctx;
    (void)fd;
    (void)events;

    rate_limiter_cleanup(&worker->rate_limiter);
}

/*
 * Extract IP address string from sockaddr.
 */
static void get_ip_string(struct sockaddr *addr, char *ip_buf, size_t ip_buf_size)
{
    if (addr->sa_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip_buf, ip_buf_size);
    } else if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, ip_buf, ip_buf_size);
    } else {
        strncpy(ip_buf, "unknown", ip_buf_size);
    }
}

/*
 * Accept callback - called for each new connection.
 * Creates a bufferevent-based Connection for async I/O.
 */
static void accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *addr, int socklen, void *ctx)
{
    WorkerProcess *worker = ctx;
    Connection *conn;
    char client_ip[INET6_ADDRSTRLEN];
    (void)listener;

    /* FIRST: Enable TCP_NODELAY before any I/O */
    tcp_nodelay_enable(fd);

    /* If draining, reject new connections */
    if (worker->draining) {
        close(fd);
        return;
    }

    /* Extract client IP for ACL and rate limiting */
    get_ip_string(addr, client_ip, sizeof(client_ip));

    /* Check IP ACL first (before rate limiting) */
    IpACLResult acl_result = ip_acl_check(&worker->ip_acl, client_ip);

    if (acl_result == IP_ACL_BLOCK) {
        send_403_response(fd);
        close(fd);
        worker->connections_rejected_blocked++;
        return;
    }

    /* Check rate limit - skip if allowlisted */
    if (acl_result != IP_ACL_ALLOW) {
        if (!rate_limiter_allow(&worker->rate_limiter, client_ip)) {
            send_429_response(fd);
            close(fd);
            worker->connections_rejected_rate++;
            return;
        }
    } else {
        worker->connections_allowlisted++;
    }

    /* Check slot availability */
    if (!slot_manager_acquire_normal(&worker->slots)) {
        send_503_response(fd);
        close(fd);
        worker->connections_rejected_slot++;
        return;
    }

    worker->connections_accepted++;
    worker->active_connections++;

    /* Create connection with bufferevent */
    conn = connection_new(worker, fd, addr, socklen);
    if (!conn) {
        log_error("Failed to create connection");
        close(fd);
        worker->active_connections--;
        slot_manager_release_normal(&worker->slots);
        return;
    }

    /* Connection is now managed by bufferevent callbacks */
}

/*
 * TLS accept callback - called for each new TLS connection.
 * Creates an SSL-wrapped bufferevent for async TLS I/O.
 */
static void tls_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                          struct sockaddr *addr, int socklen, void *ctx)
{
    WorkerProcess *worker = ctx;
    char client_ip[INET6_ADDRSTRLEN];
    (void)listener;

    /* FIRST: Enable TCP_NODELAY before any I/O */
    tcp_nodelay_enable(fd);

    /* If draining, reject new connections */
    if (worker->draining) {
        close(fd);
        return;
    }

    /* Extract client IP for ACL and rate limiting */
    get_ip_string(addr, client_ip, sizeof(client_ip));

    /* Check IP ACL first (before rate limiting) */
    IpACLResult acl_result = ip_acl_check(&worker->ip_acl, client_ip);

    if (acl_result == IP_ACL_BLOCK) {
        send_403_response(fd);
        close(fd);
        worker->connections_rejected_blocked++;
        return;
    }

    /* Check rate limit - skip if allowlisted */
    if (acl_result != IP_ACL_ALLOW) {
        if (!rate_limiter_allow(&worker->rate_limiter, client_ip)) {
            send_429_response(fd);
            close(fd);
            worker->connections_rejected_rate++;
            return;
        }
    } else {
        worker->connections_allowlisted++;
    }

    /* Check slot availability */
    if (!slot_manager_acquire_normal(&worker->slots)) {
        send_503_response(fd);
        close(fd);
        worker->connections_rejected_slot++;
        return;
    }

    worker->connections_accepted++;
    worker->active_connections++;

    /* Create SSL object */
    SSL *ssl = tls_create_ssl(&worker->tls);
    if (!ssl) {
        log_error("Failed to create SSL object");
        close(fd);
        worker->active_connections--;
        slot_manager_release_normal(&worker->slots);
        return;
    }

    /* Create SSL bufferevent */
    struct bufferevent *bev = bufferevent_openssl_socket_new(
        worker->base, fd, ssl,
        BUFFEREVENT_SSL_ACCEPTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);

    if (!bev) {
        log_error("Failed to create SSL bufferevent");
        SSL_free(ssl);
        close(fd);
        worker->active_connections--;
        slot_manager_release_normal(&worker->slots);
        return;
    }

    /* Create connection using shared init (fixes slot leak - sets slot_held=true) */
    Connection *conn = connection_new_with_bev(worker, bev, addr, socklen);
    if (!conn) {
        log_error("Failed to allocate TLS connection");
        bufferevent_free(bev);  /* This frees SSL and closes fd */
        worker->active_connections--;
        slot_manager_release_normal(&worker->slots);
        return;
    }

    conn->ssl = ssl;
    conn->tls_handshake_done = false;

    log_debug("TLS connection from %s:%d", log_format_ip(conn->client_ip), conn->client_port);
}

/*
 * Accept error callback.
 */
static void accept_error_cb(struct evconnlistener *listener, void *ctx)
{
    WorkerProcess *worker = ctx;
    int err = EVUTIL_SOCKET_ERROR();
    (void)listener;

    log_error("Accept error: %s", evutil_socket_error_to_string(err));

    /* Don't exit on transient errors */
    if (err == EMFILE || err == ENFILE) {
        log_warn("Too many open files, continuing...");
        return;
    }

    /* Fatal error - exit worker */
    event_base_loopexit(worker->base, NULL);
}

/*
 * Clean up worker resources.
 */
static void worker_cleanup(WorkerProcess *worker)
{
    if (worker->signal_event) {
        event_free(worker->signal_event);
        worker->signal_event = NULL;
    }

    if (worker->signal_event_reload) {
        event_free(worker->signal_event_reload);
        worker->signal_event_reload = NULL;
    }

    if (worker->cleanup_event) {
        event_free(worker->cleanup_event);
        worker->cleanup_event = NULL;
    }

    if (worker->listener) {
        evconnlistener_free(worker->listener);
        worker->listener = NULL;
    }

    if (worker->tls_listener) {
        evconnlistener_free(worker->tls_listener);
        worker->tls_listener = NULL;
    }

    /* Cancel in-flight async RPC requests before destroying event loop */
    rpc_manager_cancel_all(&worker->rpc);

    if (worker->base) {
        event_base_free(worker->base);
        worker->base = NULL;
    }

    /* Free TLS context */
    tls_context_free(&worker->tls);

    /* Free static files */
    static_files_free(&worker->static_files);

    /* Free rate limiter */
    rate_limiter_free(&worker->rate_limiter);

    /* Free IP ACL context */
    ip_acl_context_free(&worker->ip_acl);

    log_info("Worker cleanup complete");
}

/*
 * Worker main entry point.
 */
void worker_main(int worker_id, Config *config)
{
    WorkerProcess worker = {0};
    char identity[32];
    int listen_fd;

    /* Set up identity for logging */
    snprintf(identity, sizeof(identity), "worker[%d]", worker_id);
    log_set_identity(identity);
    log_set_json_mode(config->json_logging);
    log_set_verbose(config->verbose);

    worker.worker_id = worker_id;
    worker.config = config;
    worker.cpu_core = worker_id % get_num_cpus();

    /* Record start time for uptime metric */
    clock_gettime(CLOCK_MONOTONIC, &worker.start_time);
    worker.start_wallclock = time(NULL);

    /* Pin to CPU */
    if (pin_to_cpu(worker.cpu_core) == 0) {
        log_info("Pinned to CPU %d", worker.cpu_core);
    }

    /* Initialize slot manager */
    slot_manager_init(&worker.slots,
                      config->slots_normal_max,
                      config->slots_large_max,
                      config->slots_huge_max);

    /* Initialize rate limiter */
    if (rate_limiter_init(&worker.rate_limiter,
                          config->rate_limit_rps,
                          config->rate_limit_burst) < 0) {
        log_error("Failed to initialize rate limiter");
        exit(1);
    }

    /* Initialize IP ACL context */
    if (ip_acl_context_init(&worker.ip_acl) < 0) {
        log_error("Failed to initialize IP ACL context");
        exit(1);
    }

    /* Load blocklist if configured */
    if (config->blocklist_file[0]) {
        if (ip_acl_load_file(&worker.ip_acl.blocklist, config->blocklist_file) < 0) {
            log_warn("Failed to load blocklist from %s", config->blocklist_file);
        }
    }

    /* Load allowlist if configured */
    if (config->allowlist_file[0]) {
        if (ip_acl_load_file(&worker.ip_acl.allowlist, config->allowlist_file) < 0) {
            log_warn("Failed to load allowlist from %s", config->allowlist_file);
        }
    }

    /* Load static files from configured directory */
    if (static_files_load(&worker.static_files, config->static_dir, config) < 0) {
        log_error("Failed to load static files from %s", config->static_dir);
        exit(1);
    }

    /* Create event base BEFORE RPC init so async manager has a loop to use */
    worker.base = event_base_new();
    if (!worker.base) {
        log_error("Failed to create event base");
        static_files_free(&worker.static_files);
        exit(1);
    }

    /* Initialize RPC manager for Bitcoin node connections (async mode).
     * Pre-resolves hostnames before seccomp locks down DNS. */
    if (rpc_manager_init_async(&worker.rpc, worker.base,
                                &config->rpc_mainnet,
                                &config->rpc_testnet,
                                &config->rpc_signet,
                                &config->rpc_regtest) < 0) {
        log_warn("RPC manager initialization failed - broadcasting disabled");
    } else {
        /* Test connections and log status */
        if (config->chain == CHAIN_MIXED) {
            log_info("Mixed mode: testing all enabled RPC connections");
        }
        rpc_manager_log_status(&worker.rpc);
    }

    /* Create SO_REUSEPORT socket */
    listen_fd = create_reuseport_socket(config);
    if (listen_fd < 0) {
        log_error("Failed to create listener socket");
        exit(1);
    }

    /* Create libevent listener from our socket */
    worker.listener = evconnlistener_new(
        worker.base,
        accept_cb,
        &worker,
        LEV_OPT_CLOSE_ON_FREE,
        -1,  /* Already listening */
        listen_fd
    );

    if (!worker.listener) {
        log_error("Failed to create evconnlistener");
        close(listen_fd);
        exit(1);
    }

    evconnlistener_set_error_cb(worker.listener, accept_error_cb);

    /* Initialize TLS if enabled */
    if (config->tls_enabled) {
        if (tls_context_init(&worker.tls, config) < 0) {
            log_error("Failed to initialize TLS context");
            exit(1);
        }

        /* Create TLS listener socket */
        int tls_fd = create_tls_reuseport_socket(config);
        if (tls_fd < 0) {
            log_error("Failed to create TLS listener socket");
            exit(1);
        }

        /* Create TLS listener */
        worker.tls_listener = evconnlistener_new(
            worker.base,
            tls_accept_cb,
            &worker,
            LEV_OPT_CLOSE_ON_FREE,
            -1,
            tls_fd
        );

        if (!worker.tls_listener) {
            log_error("Failed to create TLS evconnlistener");
            close(tls_fd);
            exit(1);
        }

        evconnlistener_set_error_cb(worker.tls_listener, accept_error_cb);
        log_info("TLS listener started on port %d", config->tls_port);
    }

    /* Set up signal handling */
    g_worker = &worker;
    setup_worker_signals(&worker);

    /* Set up periodic cleanup timer for rate limiter (every 30 seconds) */
    struct timeval cleanup_interval = {30, 0};
    worker.cleanup_event = event_new(worker.base, -1, EV_PERSIST,
                                     cleanup_timer_cb, &worker);
    if (worker.cleanup_event) {
        event_add(worker.cleanup_event, &cleanup_interval);
    }

    log_info("Started on port %d (SO_REUSEPORT)", config->listen_port);

    /* Apply security restrictions (seccomp) if enabled */
    if (config->seccomp_enabled) {
        security_apply_worker_restrictions();
    }

    /* Run event loop */
    event_base_dispatch(worker.base);

    /* Cleanup and exit */
    worker_cleanup(&worker);

    log_info("Exiting with %lu connections processed", (unsigned long)worker.requests_processed);
    exit(0);
}
