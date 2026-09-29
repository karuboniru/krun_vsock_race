/* Guest half of the reproducer: a static binary that lives in the VM root as
 * /guest and drains one payload per connection.
 *
 * The guest is deliberately dumb. It connects to VSOCK_PORT on the host, sends
 * a readiness byte, then reads exactly the payload size the host announced and
 * appends the result to RESULT_PATH so the host can tell a data loss apart from
 * a VM that failed to boot.
 */
/* accept4(), nftw(), MSG_NOSIGNAL and friends are GNU extensions, so this
 * must come before any system header. */
#define _GNU_SOURCE 1
#include <linux/vm_sockets.h>
#include "common.h"

static unsigned long payload_size = DEFAULT_PAYLOAD_SIZE;
static unsigned long rounds = 1;
static int acknowledge;
static FILE *result_file;

static int connect_to_host(void)
{
    int fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) die("guest socket");
    timeout_socket(fd);
    struct sockaddr_vm address = {
        .svm_family = AF_VSOCK, .svm_port = VSOCK_PORT, .svm_cid = VMADDR_CID_HOST
    };
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) die("guest connect");
    send_all(fd, "R", 1); /* readiness byte: the host waits for it before writing */
    return fd;
}

/* Drain one payload from fd. Returns non-zero when bytes were lost or corrupt. */
static int drain_payload(int fd, unsigned long round)
{
    unsigned char chunk[4096];
    size_t received = 0;
    int saved_errno = 0, bad_data = 0;
    const char *ended = "complete";

    while (received < payload_size) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            saved_errno = n < 0 ? errno : 0;
            ended = n == 0 ? "EOF" : "recv error";
            break;
        }
        for (ssize_t i = 0; i < n; ++i)
            if (chunk[i] != (unsigned char)((received + (size_t)i) % 251)) bad_data = 1;
        received += (size_t)n;
    }

    int failed = received != payload_size || bad_data;
    printf("GUEST: round=%lu expected=%lu received=%zu missing=%zu end=%s errno=%d (%s) bad_data=%d\n",
           round, payload_size, received, payload_size - received,
           ended, saved_errno, strerror(saved_errno), bad_data);
    fprintf(result_file, "%lu %d %zu\n", round, failed ? 1 : 0, received);
    if (fflush(result_file)) die("guest result flush");
    if (!failed && acknowledge) (void)send(fd, "A", 1, MSG_NOSIGNAL);
    return failed;
}

/* Both libkrun versions may prepend the executable path to argv, so read the
 * trailing arguments the host always supplies and ignore any leading ones. */
int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *mode = "close";
    if (argc >= 4) {
        mode = argv[argc - 3];
        rounds = strtoul(argv[argc - 2], NULL, 10);
        payload_size = strtoul(argv[argc - 1], NULL, 10);
    }
    if (!rounds || !payload_size) return 2;
    acknowledge = !strcmp(mode, "keep-open");

    result_file = fopen(RESULT_PATH, "w");
    if (!result_file) die("guest result file");

    int failures = 0;
    for (unsigned long round = 1; round <= rounds; ++round) {
        int fd = connect_to_host();
        failures += drain_payload(fd, round);
        close(fd);
    }

    if (fclose(result_file)) die("guest result close");
    return failures ? 1 : 0;
}
