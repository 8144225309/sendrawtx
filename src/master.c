#include "master.h"
#include "worker.h"
#include "security.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/resource.h>

/* Global master pointer for signal handlers */
static MasterProcess *g_master = NULL;

/*
 * Check and adjust file descriptor limits.
 * Returns 0 on success, -1 if limits are critically low.
 */
static int check_fd_limits(int num_workers, int slots_per_worker)
{
    struct rlimit rlim;

    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
        log_warn("getrlimit(RLIMIT_NOFILE) failed: %s", strerror(errno));
        return 0;  /* Continue anyway - non-fatal */
    }

    /* Calculate required FDs per worker:
     * - 2 listen sockets (HTTP + TLS)
     * - slots for connections
     * - ~15 for events, logging, TLS, etc.
     */
    rlim_t per_worker = (rlim_t)slots_per_worker + 15;
    rlim_t required = (rlim_t)num_workers * per_worker + 50;
    rlim_t minimum = (rlim_t)num_workers * 20 + 20;

    log_info("FD limits: soft=%lu, hard=%lu, required~=%lu",
             (unsigned long)rlim.rlim_cur,
             (unsigned long)rlim.rlim_max,
             (unsigned long)required);

    if (rlim.rlim_cur < required) {
        /* Try to raise soft limit */
        rlim_t new_limit = required < rlim.rlim_max ? required : rlim.rlim_max;
        struct rlimit new_rlim = { new_limit, rlim.rlim_max };

        if (setrlimit(RLIMIT_NOFILE, &new_rlim) < 0) {
            log_warn("Could not raise FD limit to %lu: %s",
                     (unsigned long)new_limit, strerror(errno));
            getrlimit(RLIMIT_NOFILE, &rlim);
        } else {
            log_info("Raised FD soft limit to %lu", (unsigned long)new_limit);
            rlim.rlim_cur = new_limit;
        }
    }

    if (rlim.rlim_cur < minimum) {
        log_error("FATAL: FD limit %lu is below minimum %lu for %d workers",
                  (unsigned long)rlim.rlim_cur, (unsigned long)minimum, num_workers);
        log_error("Increase limit with: ulimit -n %lu", (unsigned long)required);
        return -1;
    }

    if (rlim.rlim_cur < required) {
        log_warn("FD limit %lu is below recommended %lu - may reject connections under load",
                 (unsigned long)rlim.rlim_cur, (unsigned long)required);
    }

    return 0;
}

/*
 * Signal handlers - just set flags, don't do work in handler.
 */
static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_master) {
        g_master->shutdown_requested = 1;
    }
}

static void sighup_handler(int sig)
{
    (void)sig;
    if (g_master) {
        g_master->reload_requested = 1;
    }
}

static void sigchld_handler(int sig)
{
    (void)sig;
    /* Just interrupt the sleep/pause in main loop */
}

/*
 * Set up master signal handlers.
 */
static void setup_master_signals(MasterProcess *master)
{
    struct sigaction sa;

    g_master = master;

    /* SIGTERM/SIGINT - graceful shutdown */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* SIGHUP - graceful reload */
    sa.sa_handler = sighup_handler;
    sigaction(SIGHUP, &sa, NULL);

    /* SIGCHLD - child status change */
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    /* Ignore SIGPIPE */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

int master_init(MasterProcess *master, const char *config_path)
{
    memset(master, 0, sizeof(MasterProcess));
    master->config_path = config_path;

    /* Load configuration */
    master->config = config_load(config_path);
    if (!master->config) {
        log_error("Failed to load configuration from %s", config_path);
        return -1;
    }

    /* Determine number of workers - always based on CPU count */
    master->num_workers = get_num_cpus();

    /* Sanity limits */
    if (master->num_workers < 1) {
        master->num_workers = 1;
    }
    if (master->num_workers > 64) {
        master->num_workers = 64;
    }

    /* Check and adjust FD limits */
    int total_slots = master->config->slots_normal_max +
                      master->config->slots_large_max +
                      master->config->slots_huge_max;
    if (check_fd_limits(master->num_workers, total_slots) < 0) {
        log_error("Insufficient file descriptor limits - cannot start");
        config_free(master->config);
        return -1;
    }

    /* Allocate worker PID array */
    master->worker_pids = calloc(master->num_workers, sizeof(pid_t));
    if (!master->worker_pids) {
        log_error("Failed to allocate worker PID array");
        config_free(master->config);
        return -1;
    }

    return 0;
}

pid_t fork_worker(int worker_id, Config *config)
{
    pid_t pid = fork();

    if (pid < 0) {
        log_error("fork() failed for worker %d: %s", worker_id, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process - become worker */
        worker_main(worker_id, config);
        /* worker_main calls exit(), should never reach here */
        exit(1);
    }

    /* Parent - return child PID */
    return pid;
}

/*
 * Start all workers.
 */
static int start_workers(MasterProcess *master)
{
    log_info("Starting %d worker processes", master->num_workers);

    for (int i = 0; i < master->num_workers; i++) {
        master->worker_pids[i] = fork_worker(i, master->config);
        if (master->worker_pids[i] < 0) {
            log_error("Failed to start worker %d", i);
            /* Continue trying to start other workers */
        } else {
            log_info("Started worker %d (pid %d)", i, master->worker_pids[i]);
        }
    }

    return 0;
}

/*
 * Find worker index by PID.
 */
static int find_worker_by_pid(MasterProcess *master, pid_t pid)
{
    for (int i = 0; i < master->num_workers; i++) {
        if (master->worker_pids[i] == pid) {
            return i;
        }
    }
    return -1;
}

/*
 * Check if PID is a draining worker (old worker during reload).
 * Returns 1 and removes from list if found, 0 otherwise.
 */
static int remove_draining_worker(MasterProcess *master, pid_t pid)
{
    for (int i = 0; i < master->num_draining; i++) {
        if (master->draining_pids[i] == pid) {
            /* Remove by swapping with last element */
            master->draining_pids[i] = master->draining_pids[master->num_draining - 1];
            master->num_draining--;
            return 1;
        }
    }
    return 0;
}

/*
 * Handle worker exit - restart if not shutting down.
 */
static void handle_worker_exit(MasterProcess *master, pid_t pid, int status)
{
    /* Check if this is a draining worker from reload (expected exit) */
    if (remove_draining_worker(master, pid)) {
        log_info("Draining worker (pid %d) exited cleanly", pid);
        return;
    }

    int worker_id = find_worker_by_pid(master, pid);

    if (worker_id < 0) {
        log_warn("Unknown child process %d exited", pid);
        return;
    }

    if (WIFEXITED(status)) {
        log_warn("Worker %d (pid %d) exited with status %d",
                 worker_id, pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log_warn("Worker %d (pid %d) killed by signal %d",
                 worker_id, pid, WTERMSIG(status));
    }

    /* Restart worker if not shutting down */
    if (!master->shutdown_requested) {
        log_info("Restarting worker %d", worker_id);
        master->worker_pids[worker_id] = fork_worker(worker_id, master->config);
        if (master->worker_pids[worker_id] > 0) {
            log_info("Restarted worker %d (new pid %d)",
                     worker_id, master->worker_pids[worker_id]);
        }
    } else {
        master->worker_pids[worker_id] = 0;
    }
}

void master_shutdown_workers(MasterProcess *master)
{
    log_info("Sending SIGUSR1 to all workers (graceful drain)");

    for (int i = 0; i < master->num_workers; i++) {
        if (master->worker_pids[i] > 0) {
            kill(master->worker_pids[i], SIGUSR1);
        }
    }
}

/*
 * Wait for all workers to exit.
 */
static void wait_for_workers(MasterProcess *master, int timeout_sec)
{
    time_t deadline = time(NULL) + timeout_sec;
    int remaining = 0;

    /* Count remaining workers */
    for (int i = 0; i < master->num_workers; i++) {
        if (master->worker_pids[i] > 0) {
            remaining++;
        }
    }

    log_info("Waiting for %d workers to exit (timeout: %ds)", remaining, timeout_sec);

    while (remaining > 0 && time(NULL) < deadline) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0) {
            int worker_id = find_worker_by_pid(master, pid);
            if (worker_id >= 0) {
                master->worker_pids[worker_id] = 0;
                remaining--;
                log_info("Worker %d exited, %d remaining", worker_id, remaining);
            }
        } else if (pid == 0) {
            /* No child exited yet */
            usleep(100000);  /* 100ms */
        } else if (errno != EINTR) {
            break;
        }
    }

    /* Force kill any remaining workers */
    if (remaining > 0) {
        log_warn("Force killing %d workers", remaining);
        for (int i = 0; i < master->num_workers; i++) {
            if (master->worker_pids[i] > 0) {
                kill(master->worker_pids[i], SIGKILL);
                waitpid(master->worker_pids[i], NULL, 0);
                master->worker_pids[i] = 0;
            }
        }
    }
}

void master_reload(MasterProcess *master)
{
    log_info("Initiating graceful reload");

    /* Reload configuration */
    Config *new_config = config_load(master->config_path);
    if (!new_config) {
        log_error("Failed to reload config, keeping old configuration");
        return;
    }

    /* Allocate draining_pids if needed */
    if (!master->draining_pids) {
        master->draining_pids = calloc(master->num_workers, sizeof(pid_t));
    }
    master->num_draining = 0;

    /* Collect old worker PIDs before signaling them */
    for (int i = 0; i < master->num_workers; i++) {
        if (master->worker_pids[i] > 0) {
            master->draining_pids[master->num_draining++] = master->worker_pids[i];
        }
    }

    /* Signal old workers to drain */
    master_shutdown_workers(master);

    /* Wait briefly for old workers to start draining */
    usleep(100000);  /* 100ms */

    /* Update config and restart workers */
    Config *old_config = master->config;
    master->config = new_config;

    /* Fork new workers (they'll use new config) */
    for (int i = 0; i < master->num_workers; i++) {
        pid_t old_pid = master->worker_pids[i];

        /* Start new worker (even if old worker was dead) */
        master->worker_pids[i] = fork_worker(i, new_config);
        if (old_pid > 0) {
            log_info("Started new worker %d (pid %d), old worker %d draining",
                     i, master->worker_pids[i], old_pid);
        } else {
            log_info("Started new worker %d (pid %d), replacing dead slot",
                     i, master->worker_pids[i]);
        }
    }

    /* Free old config */
    config_free(old_config);

    log_info("Reload complete");
}

int master_run(MasterProcess *master)
{
    /* Set up signals */
    setup_master_signals(master);

    /* Log security status */
    security_log_status();

    /* Start workers */
    if (start_workers(master) < 0) {
        return 1;
    }

    log_info("Master running, %d workers active", master->num_workers);
    log_info("Send SIGTERM for graceful shutdown, SIGHUP for reload");

    /* Main loop - monitor workers */
    while (!master->shutdown_requested) {
        /* Check for reload request */
        if (master->reload_requested) {
            master->reload_requested = 0;
            master_reload(master);
        }

        /* Wait for child events */
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0) {
            handle_worker_exit(master, pid, status);
        } else if (pid < 0 && errno != ECHILD && errno != EINTR) {
            log_error("waitpid failed: %s", strerror(errno));
        }

        /* Sleep briefly to avoid busy loop */
        usleep(100000);  /* 100ms */
    }

    /* Graceful shutdown */
    log_info("Shutdown requested, draining workers");
    master_shutdown_workers(master);
    wait_for_workers(master, 30);  /* 30 second timeout */

    log_info("All workers stopped");
    return 0;
}

void master_cleanup(MasterProcess *master)
{
    if (master->worker_pids) {
        free(master->worker_pids);
        master->worker_pids = NULL;
    }

    if (master->draining_pids) {
        free(master->draining_pids);
        master->draining_pids = NULL;
    }

    if (master->config) {
        config_free(master->config);
        master->config = NULL;
    }
}
