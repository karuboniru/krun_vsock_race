/* Helpers shared by the host and guest halves of the reproducer.
 *
 * The host exists in two flavours (host_v1.c for libkrun 1.x, host_v2.c for
 * libkrun 2.x); guest.c is the same binary for both.
 */
#ifndef VSOCK_TAIL_COMMON_H
#define VSOCK_TAIL_COMMON_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define VSOCK_PORT 5000
#define RESULT_PATH "/result"
#define DEFAULT_PAYLOAD_SIZE (16UL * 1024)
#define MAX_PAYLOAD_SIZE (64UL * 1024 * 1024)
#define DEFAULT_ROUNDS 20UL
#define MAX_ROUNDS 100000UL
#define MAX_DELAY_US 1000000UL
#define SOCKET_TIMEOUT_SECONDS 15
/* `close` freezes the VMM between write and close, so the payload has to fit in
 * the socket buffer or the write could never be drained. */
#define CLOSE_MAX_PAYLOAD (128UL * 1024)

/* Exit code 2 marks a harness or runtime failure, never a reproduction. */
static void die(const char *what)
{
    perror(what);
    exit(2);
}

/* Write the whole buffer, retrying on short writes and EINTR. */
static void send_all(int fd, const void *data, size_t length)
{
    const char *p = data;
    while (length) {
        ssize_t n = send(fd, p, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) die("send");
        p += n;
        length -= (size_t)n;
    }
}

static void timeout_socket(int fd)
{
    struct timeval timeout = {.tv_sec = SOCKET_TIMEOUT_SECONDS};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)))
        die("socket timeout");
}

#endif /* VSOCK_TAIL_COMMON_H */
