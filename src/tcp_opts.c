#include "tcp_opts.h"
#include "log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <string.h>

int tcp_nodelay_enable(int fd)
{
    int flag = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        log_warn("TCP_NODELAY failed on fd %d: %s", fd, strerror(errno));
        return -1;
    }
    return 0;
}

int tcp_cork_enable(int fd)
{
#ifdef TCP_CORK
    int flag = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag)) < 0) {
        log_warn("TCP_CORK enable failed on fd %d: %s", fd, strerror(errno));
        return -1;
    }
    return 0;
#else
    /* TCP_CORK not available (non-Linux) */
    (void)fd;
    return 0;
#endif
}

int tcp_cork_disable(int fd)
{
#ifdef TCP_CORK
    int flag = 0;
    if (setsockopt(fd, IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag)) < 0) {
        log_warn("TCP_CORK disable failed on fd %d: %s", fd, strerror(errno));
        return -1;
    }
    return 0;
#else
    /* TCP_CORK not available (non-Linux) */
    (void)fd;
    return 0;
#endif
}
