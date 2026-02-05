/*
 * RawRelay Server v6 - Multi-Process Architecture
 *
 * Entry point for the master process.
 * The master forks worker processes, each with:
 * - Own SO_REUSEPORT socket (kernel load-balances connections)
 * - Own event loop
 * - CPU affinity
 * - No shared state (no locks needed)
 *
 * Signals:
 *   SIGTERM/SIGINT - Graceful shutdown
 *   SIGHUP         - Graceful reload
 */

#include "master.h"
#include "worker.h"
#include "log.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

static void print_banner(void)
{
    printf("\n");
    printf("  ____                ____       _             \n");
    printf(" |  _ \\ __ ___      _|  _ \\ ___| | __ _ _   _ \n");
    printf(" | |_) / _` \\ \\ /\\ / / |_) / _ \\ |/ _` | | | |\n");
    printf(" |  _ < (_| |\\ V  V /|  _ <  __/ | (_| | |_| |\n");
    printf(" |_| \\_\\__,_| \\_/\\_/ |_| \\_\\___|_|\\__,_|\\__, |\n");
    printf("                                        |___/ \n");
    printf("\n");
    printf("  sendrawtx.com Production Server v6\n");
    printf("  Multi-Process Architecture with SO_REUSEPORT\n");
    printf("\n");
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options] [config_file]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help      Show this help message\n");
    printf("  -t, --test      Test configuration and exit\n");
    printf("  -w, --workers N Override number of workers\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  config_file     Path to configuration file (default: config.ini)\n");
    printf("\n");
    printf("Signals:\n");
    printf("  SIGTERM, SIGINT - Graceful shutdown\n");
    printf("  SIGHUP          - Graceful reload (re-read config)\n");
    printf("  SIGUSR1         - (to workers) Graceful drain\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                      # Use default config.ini\n", prog);
    printf("  %s /etc/rawrelay.ini    # Use custom config\n", prog);
    printf("  %s -w 8                 # Force 8 workers\n", prog);
    printf("\n");
}

int main(int argc, char **argv)
{
    MasterProcess master;
    const char *config_path = "config.ini";
    int test_mode = 0;
    int override_workers = 0;
    int ret;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
            test_mode = 1;
            continue;
        }
        if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--workers") == 0) && i + 1 < argc) {
            /* Security: use strtol with proper error checking instead of atoi */
            char *endptr = NULL;
            const char *arg = argv[++i];
            errno = 0;
            long val = strtol(arg, &endptr, 10);
            if (errno == ERANGE || endptr == arg || *endptr != '\0' ||
                val <= 0 || val > 64) {
                fprintf(stderr, "Invalid worker count: %s (must be 1-64)\n", arg);
                return 1;
            }
            override_workers = (int)val;
            continue;
        }
        /* Assume it's the config path */
        config_path = argv[i];
    }

    /* Initialize logging */
    log_init(LOG_INFO);
    log_set_identity("master");

    print_banner();
    fflush(stdout);  /* Flush before forking to prevent duplicate output */

    /* Initialize master */
    if (master_init(&master, config_path) < 0) {
        log_error("Failed to initialize master process");
        return 1;
    }

    /* Set JSON logging mode from config */
    log_set_json_mode(master.config->json_logging);

    /* Override workers if specified */
    if (override_workers > 0) {
        master.num_workers = override_workers;
        log_info("Overriding worker count to %d", override_workers);
    }

    /* Test mode - just verify config and exit */
    if (test_mode) {
        printf("Configuration OK:\n");
        config_print(master.config);
        printf("\nWorkers: %d\n", master.num_workers);
        printf("CPUs available: %d\n", get_num_cpus());
        master_cleanup(&master);
        return 0;
    }

    /* Print startup info */
    log_info("Configuration loaded from %s", config_path);
    log_info("Workers: %d, CPUs: %d", master.num_workers, get_num_cpus());
    log_info("Listen port: %d", master.config->listen_port);

    /* Run master (forks workers, monitors them) */
    ret = master_run(&master);

    /* Cleanup */
    master_cleanup(&master);

    log_info("Master process exiting");
    return ret;
}
