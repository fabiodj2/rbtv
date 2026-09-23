/* netshim.c - LD_PRELOAD shim: intercept AF_PACKET sockets.
 *
 * The PrimeBox player opens an AF_PACKET/SOCK_DGRAM socket to do ARP
 * discovery (Pro DJ Link peer detection).  Inside the RK3399 chroot,
 * unprivileged users cannot create AF_PACKET sockets (needs CAP_NET_RAW),
 * so socket() returns EPERM.  The player does not check the return value,
 * then calls recv() on fd -1, gets EBADF in a loop, and SIGSEGVs.
 *
 * This shim intercepts socket() and, for AF_PACKET only, either:
 *   RX3_NET_MODE=unsupported (default)
 *       -> return -1 with errno=EAFNOSUPPORT
 *   RX3_NET_MODE=unix
 *       -> return an AF_UNIX SOCK_DGRAM socket instead
 *
 * All other families pass through unchanged.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>

#ifndef AF_PACKET
#define AF_PACKET 17
#endif

typedef int (*socket_fn)(int, int, int);

int socket(int domain, int type, int protocol)
{
    static socket_fn real_socket;

    if (!real_socket)
        real_socket = (socket_fn)dlsym(RTLD_NEXT, "socket");

    if (domain == AF_PACKET) {
        const char *mode = getenv("RX3_NET_MODE");
        if (mode && strcmp(mode, "unix") == 0) {
            /* Return a live AF_UNIX SOCK_DGRAM socket.  bind/sendto on it
             * will fail with EINVAL, and recvfrom will block.  The player
             * sees a valid fd instead of -1, so it does not crash on recv. */
            return real_socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        }
        errno = EAFNOSUPPORT;
        return -1;
    }

    return real_socket(domain, type, protocol);
}
