#ifndef MASTER_H
#define MASTER_H

#include "config.h"
#include <stdbool.h>
#include <sys/types.h>

/*
 * Master process - manages worker lifecycle.
 *
 * Responsibilities:
 * - Fork worker processes
 * - Monitor workers (restart on crash)
 * - Handle SIGHUP for graceful reload
 * - Handle SIGTERM for graceful shutdown
 */

typedef struct MasterProcess {
    /* Configuration */
    Config *config;
    const char *config_path;

    /* Workers */
    int num_workers;
    pid_t *worker_pids;

    /* Draining workers (old workers during reload) */
    pid_t *draining_pids;
    int num_draining;

    /* Signal flags (set by signal handlers) */
    volatile bool shutdown_requested;
    volatile bool reload_requested;
} MasterProcess;

/*
 * Initialize master process.
 * Returns 0 on success, -1 on error.
 */
int master_init(MasterProcess *master, const char *config_path);

/*
 * Run master main loop.
 * Forks workers and monitors them.
 * Returns exit code (0 = success).
 */
int master_run(MasterProcess *master);

/*
 * Clean up master resources.
 */
void master_cleanup(MasterProcess *master);

/*
 * Fork a single worker process.
 * Returns child PID to parent, or -1 on error.
 * In child: calls worker_main() and exits.
 */
pid_t fork_worker(int worker_id, Config *config);

/*
 * Send graceful shutdown signal to all workers.
 */
void master_shutdown_workers(MasterProcess *master);

/*
 * Graceful reload: fork new workers, drain old ones.
 */
void master_reload(MasterProcess *master);

#endif /* MASTER_H */
