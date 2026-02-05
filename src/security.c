/*
 * Security Hardening Module
 *
 * Implements OS-level security restrictions for worker processes.
 * Currently supports Linux seccomp-bpf syscall filtering.
 *
 * The seccomp filter whitelists only the syscalls needed for:
 * - Network I/O (read, write, accept, etc.)
 * - Memory management (mmap, munmap, brk)
 * - File operations (open, close, fstat - limited)
 * - Event handling (epoll_*, poll)
 * - Time functions (gettimeofday, clock_gettime)
 * - Signals (rt_sigaction, rt_sigprocmask)
 */

#include "security.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>

#ifdef SECCOMP_MODE_FILTER

/* Architecture for BPF filter */
#if defined(__x86_64__)
#define AUDIT_ARCH_CURRENT AUDIT_ARCH_X86_64
#elif defined(__i386__)
#define AUDIT_ARCH_CURRENT AUDIT_ARCH_I386
#elif defined(__aarch64__)
#define AUDIT_ARCH_CURRENT AUDIT_ARCH_AARCH64
#elif defined(__arm__)
#define AUDIT_ARCH_CURRENT AUDIT_ARCH_ARM
#else
#define AUDIT_ARCH_CURRENT 0
#endif

#define ALLOW_SYSCALL(name) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

static int apply_seccomp_filter(void)
{
    static struct sock_filter filter[] = {
        /* Load architecture */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
#if AUDIT_ARCH_CURRENT != 0
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_CURRENT, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#endif
        /* Load syscall number */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),

        /* Network I/O */
        ALLOW_SYSCALL(read),
        ALLOW_SYSCALL(write),
        ALLOW_SYSCALL(readv),
        ALLOW_SYSCALL(writev),
        ALLOW_SYSCALL(recvfrom),
        ALLOW_SYSCALL(sendto),
        ALLOW_SYSCALL(recvmsg),
        ALLOW_SYSCALL(sendmsg),
#ifdef __NR_recvmmsg
        ALLOW_SYSCALL(recvmmsg),
#endif
#ifdef __NR_sendmmsg
        ALLOW_SYSCALL(sendmmsg),
#endif
#ifdef __NR_accept4
        ALLOW_SYSCALL(accept4),
#endif
#ifdef __NR_accept
        ALLOW_SYSCALL(accept),
#endif
        ALLOW_SYSCALL(socket),
        ALLOW_SYSCALL(bind),
        ALLOW_SYSCALL(listen),
        ALLOW_SYSCALL(getsockname),
        ALLOW_SYSCALL(getpeername),
        ALLOW_SYSCALL(setsockopt),
        ALLOW_SYSCALL(getsockopt),
        ALLOW_SYSCALL(shutdown),
        ALLOW_SYSCALL(close),
#ifdef __NR_pipe
        ALLOW_SYSCALL(pipe),
#endif
#ifdef __NR_pipe2
        ALLOW_SYSCALL(pipe2),
#endif
#ifdef __NR_dup
        ALLOW_SYSCALL(dup),
#endif
#ifdef __NR_dup2
        ALLOW_SYSCALL(dup2),
#endif
#ifdef __NR_dup3
        ALLOW_SYSCALL(dup3),
#endif
#ifdef __NR_eventfd
        ALLOW_SYSCALL(eventfd),
#endif
#ifdef __NR_eventfd2
        ALLOW_SYSCALL(eventfd2),
#endif
#ifdef __NR_socketpair
        ALLOW_SYSCALL(socketpair),
#endif

        /* Memory management */
        ALLOW_SYSCALL(brk),
        ALLOW_SYSCALL(mmap),
        ALLOW_SYSCALL(munmap),
        ALLOW_SYSCALL(mprotect),
        ALLOW_SYSCALL(mremap),
#ifdef __NR_madvise
        ALLOW_SYSCALL(madvise),
#endif

        /* File operations (limited) */
        ALLOW_SYSCALL(openat),
        ALLOW_SYSCALL(open),
        ALLOW_SYSCALL(fstat),
#ifdef __NR_stat
        ALLOW_SYSCALL(stat),
#endif
#ifdef __NR_lstat
        ALLOW_SYSCALL(lstat),
#endif
#ifdef __NR_newfstatat
        ALLOW_SYSCALL(newfstatat),
#endif
#ifdef __NR_access
        ALLOW_SYSCALL(access),
#endif
#ifdef __NR_faccessat
        ALLOW_SYSCALL(faccessat),
#endif
        ALLOW_SYSCALL(lseek),
#ifdef __NR_pread64
        ALLOW_SYSCALL(pread64),
#endif
#ifdef __NR_pwrite64
        ALLOW_SYSCALL(pwrite64),
#endif
        ALLOW_SYSCALL(ioctl),

        /* Event handling */
        ALLOW_SYSCALL(epoll_create1),
        ALLOW_SYSCALL(epoll_ctl),
        ALLOW_SYSCALL(epoll_wait),
#ifdef __NR_epoll_pwait
        ALLOW_SYSCALL(epoll_pwait),
#endif
#ifdef __NR_epoll_pwait2
        ALLOW_SYSCALL(epoll_pwait2),
#endif
        ALLOW_SYSCALL(poll),
#ifdef __NR_ppoll
        ALLOW_SYSCALL(ppoll),
#endif
        ALLOW_SYSCALL(select),
#ifdef __NR_pselect6
        ALLOW_SYSCALL(pselect6),
#endif

        /* Time functions */
        ALLOW_SYSCALL(gettimeofday),
        ALLOW_SYSCALL(clock_gettime),
#ifdef __NR_clock_getres
        ALLOW_SYSCALL(clock_getres),
#endif
#ifdef __NR_timerfd_create
        ALLOW_SYSCALL(timerfd_create),
#endif
#ifdef __NR_timerfd_settime
        ALLOW_SYSCALL(timerfd_settime),
#endif
#ifdef __NR_timerfd_gettime
        ALLOW_SYSCALL(timerfd_gettime),
#endif

        /* Signals */
        ALLOW_SYSCALL(rt_sigaction),
        ALLOW_SYSCALL(rt_sigprocmask),
        ALLOW_SYSCALL(rt_sigreturn),
        ALLOW_SYSCALL(sigaltstack),

        /* Process */
        ALLOW_SYSCALL(exit),
        ALLOW_SYSCALL(exit_group),
        ALLOW_SYSCALL(getpid),
        ALLOW_SYSCALL(gettid),
#ifdef __NR_getuid
        ALLOW_SYSCALL(getuid),
#endif
#ifdef __NR_geteuid
        ALLOW_SYSCALL(geteuid),
#endif

        /* Misc */
        ALLOW_SYSCALL(futex),
#ifdef __NR_getrandom
        ALLOW_SYSCALL(getrandom),
#endif
#ifdef __NR_prlimit64
        ALLOW_SYSCALL(prlimit64),
#endif
        ALLOW_SYSCALL(fcntl),
#ifdef __NR_fcntl64
        ALLOW_SYSCALL(fcntl64),
#endif
#ifdef __NR_rseq
        ALLOW_SYSCALL(rseq),
#endif
#ifdef __NR_statx
        ALLOW_SYSCALL(statx),
#endif
#ifdef __NR_getrlimit
        ALLOW_SYSCALL(getrlimit),
#endif
#ifdef __NR_clock_nanosleep
        ALLOW_SYSCALL(clock_nanosleep),
#endif
#ifdef __NR_nanosleep
        ALLOW_SYSCALL(nanosleep),
#endif
#ifdef __NR_sendfile
        ALLOW_SYSCALL(sendfile),
#endif
#ifdef __NR_sendfile64
        ALLOW_SYSCALL(sendfile64),
#endif
#ifdef __NR_uname
        ALLOW_SYSCALL(uname),
#endif
#ifdef __NR_arch_prctl
        ALLOW_SYSCALL(arch_prctl),
#endif
#ifdef __NR_set_tid_address
        ALLOW_SYSCALL(set_tid_address),
#endif
#ifdef __NR_set_robust_list
        ALLOW_SYSCALL(set_robust_list),
#endif
#ifdef __NR_connect
        ALLOW_SYSCALL(connect),
#endif
#ifdef __NR_getppid
        ALLOW_SYSCALL(getppid),
#endif

        /* Default: kill on disallowed syscall */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    };

    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        log_warn("prctl(PR_SET_NO_NEW_PRIVS) failed: %s", strerror(errno));
    }

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
        log_warn("seccomp filter failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int security_seccomp_available(void)
{
#if AUDIT_ARCH_CURRENT != 0
    if (prctl(PR_GET_SECCOMP) < 0 && errno == EINVAL) {
        return 0;
    }
    return 1;
#else
    return 0;
#endif
}

int security_apply_worker_restrictions(void)
{
    if (!security_seccomp_available()) {
        log_info("Seccomp not available on this platform");
        return 0;
    }

    if (apply_seccomp_filter() < 0) {
        log_warn("Failed to apply seccomp filter - continuing without syscall restrictions");
        return -1;
    }

    log_info("Seccomp syscall filter applied");
    return 0;
}

void security_log_status(void)
{
    if (security_seccomp_available()) {
        log_info("Security: seccomp available");
    } else {
        log_info("Security: seccomp not available");
    }
}

#else /* SECCOMP_MODE_FILTER not defined */

int security_seccomp_available(void) { return 0; }
int security_apply_worker_restrictions(void) { return 0; }
void security_log_status(void) { log_info("Security: seccomp headers not available"); }

#endif /* SECCOMP_MODE_FILTER */

#else /* Not Linux */

int security_seccomp_available(void) { return 0; }
int security_apply_worker_restrictions(void) { return 0; }
void security_log_status(void) { log_info("Security: non-Linux platform"); }

#endif /* __linux__ */
