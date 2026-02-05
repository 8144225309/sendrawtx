#ifndef SECURITY_H
#define SECURITY_H

/*
 * Security Hardening Module
 *
 * Provides OS-level security mechanisms:
 * - seccomp syscall filtering (Linux)
 *
 * All functions are no-ops on unsupported platforms.
 */

/*
 * Initialize security restrictions for worker processes.
 * Call after opening sockets but before accepting connections.
 *
 * On Linux: applies seccomp-bpf filter to whitelist only needed syscalls.
 * On other platforms: no-op.
 *
 * Returns 0 on success, -1 on error.
 */
int security_apply_worker_restrictions(void);

/*
 * Check if seccomp is available on this system.
 * Returns 1 if available, 0 otherwise.
 */
int security_seccomp_available(void);

/*
 * Log security status (for diagnostics).
 */
void security_log_status(void);

#endif /* SECURITY_H */
