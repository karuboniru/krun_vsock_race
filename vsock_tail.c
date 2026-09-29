/* Standalone libkrun Unix/vsock "close with unread data" reproducer.
 *
 * The same source is built twice (see CMakeLists.txt / README.md):
 *   vsock-tail        host process that embeds libkrun and plays the client
 *   vsock-tail-guest  statically linked guest that plays the server
 *
 * Host modes:
 *   close      freeze the VMM, write the payload, close the socket, resume it
 *   keep-open  write the payload and keep the socket open until the guest ACKs
 *   race       write the payload and close immediately, repeating ROUNDS times
 *              on fresh connections of a single VM
 *
 * `close` is the deterministic reproduction: freezing every VMM thread makes
 * the kernel see the write and the hangup together, so the tail is always lost.
 * `keep-open` is its control: the same write, but the socket survives until the
 * guest has drained it, so nothing is lost. `race` never signals the VMM at all;
 * it replays the same write-then-close pattern an ordinary client performs and
 * reports how often the tail is lost.
 *
 * No agent-vm code is used.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
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

/* ------------------------------------------------------------------ guest */

#ifdef REPRO_GUEST
#include <linux/vm_sockets.h>

static unsigned long guest_payload_size = DEFAULT_PAYLOAD_SIZE;
static unsigned long guest_rounds = 1;
static int guest_acknowledge;
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

/* Drain one payload from fd. Returns non-zero when bytes were lost or corrupt.
 * The result is appended to the file so the host can tell a data loss apart
 * from a VM that failed to boot. */
static int drain_payload(int fd, unsigned long round)
{
    unsigned char chunk[4096];
    size_t received = 0;
    int saved_errno = 0, bad_data = 0;
    const char *ended = "complete";

    while (received < guest_payload_size) {
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

    int failed = received != guest_payload_size || bad_data;
    printf("GUEST: round=%lu expected=%lu received=%zu missing=%zu end=%s errno=%d (%s) bad_data=%d\n",
           round, guest_payload_size, received, guest_payload_size - received,
           ended, saved_errno, strerror(saved_errno), bad_data);
    fprintf(result_file, "%lu %d %zu\n", round, failed ? 1 : 0, received);
    if (fflush(result_file)) die("guest result flush");
    if (!failed && guest_acknowledge) (void)send(fd, "A", 1, MSG_NOSIGNAL);
    return failed;
}

/* libkrun may or may not prepend exec_path as argv[0], so read the trailing
 * arguments the host always supplies and ignore any leading program path. */
int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *mode = "close";
    if (argc >= 4) {
        mode = argv[argc - 3];
        guest_rounds = strtoul(argv[argc - 2], NULL, 10);
        guest_payload_size = strtoul(argv[argc - 1], NULL, 10);
    }
    if (!guest_rounds || !guest_payload_size) return 2;
    guest_acknowledge = !strcmp(mode, "keep-open");

    result_file = fopen(RESULT_PATH, "w");
    if (!result_file) die("guest result file");

    int failures = 0;
    for (unsigned long round = 1; round <= guest_rounds; ++round) {
        int fd = connect_to_host();
        failures += drain_payload(fd, round);
        close(fd);
    }

    if (fclose(result_file)) die("guest result close");
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------- host */

#else
#include <ftw.h>
#include <limits.h>
#include <sys/prctl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <libkrun.h>

enum mode { MODE_CLOSE, MODE_KEEP_OPEN, MODE_RACE, MODE_COUNT };

static const char *const mode_names[MODE_COUNT] = {"close", "keep-open", "race"};

static const char temporary_template[] = "/tmp/krun-tail-XXXXXX";
static char temporary[sizeof(temporary_template)];
static pid_t vm_pid = -1;
static int have_temporary;

struct round_result {
    unsigned long round;
    int failed;
    size_t received;
};

/* The payload is a fixed byte pattern so the guest can detect corruption. */
static void fill_payload(unsigned char *payload, size_t length)
{
    for (size_t i = 0; i < length; ++i) payload[i] = (unsigned char)(i % 251);
}

static unsigned long long now_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) die("clock_gettime");
    return (unsigned long long)now.tv_sec * 1000000ULL + (unsigned long long)now.tv_nsec / 1000ULL;
}

/* Busy-wait instead of sleeping: nanosleep rounds sub-millisecond delays up to
 * the timer slack (tens of microseconds), which would hide the race window. */
static void spin_us(unsigned long delay_us)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) die("clock_gettime");
    for (;;) {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) die("clock_gettime");
        long long elapsed = (long long)(now.tv_sec - start.tv_sec) * 1000000LL +
                            (long long)(now.tv_nsec - start.tv_nsec) / 1000LL;
        if (elapsed >= (long long)delay_us) return;
    }
}

/* Remove the whole temporary tree, including the guest root. */
static int remove_entry(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st; (void)type; (void)ftw;
    return remove(path);
}

static void cleanup(void)
{
    if (vm_pid > 0) {
        kill(vm_pid, SIGKILL);
        while (waitpid(vm_pid, NULL, 0) < 0 && errno == EINTR) {}
        vm_pid = -1;
    }
    if (have_temporary) {
        nftw(temporary, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
        have_temporary = 0;
    }
}

static void expired(int sig)
{
    (void)sig;
    if (vm_pid > 0) kill(vm_pid, SIGKILL);
    const char message[] = "HOST: timed out; temporary files may remain in /tmp/krun-tail-*\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

/* libkrun entry points return a negative errno. */
static void check(int result, const char *what)
{
    if (result < 0) { errno = -result; die(what); }
}

static void copy_file(const char *source, const char *target)
{
    int in = open(source, O_RDONLY | O_CLOEXEC);
    if (in < 0) die("open static guest executable");
    int out = open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0755);
    if (out < 0) die("create guest executable");
    char buffer[65536];
    ssize_t n;
    while ((n = read(in, buffer, sizeof(buffer))) != 0) {
        if (n < 0) { if (errno == EINTR) continue; die("read guest executable"); }
        ssize_t offset = 0;
        while (offset < n) {
            ssize_t written = write(out, buffer + offset, (size_t)(n - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) die("write guest executable");
            offset += written;
        }
    }
    close(in);
    if (close(out)) die("close guest executable");
}

static void unix_address(struct sockaddr_un *address, const char *path)
{
    if (strlen(path) >= sizeof(address->sun_path)) {
        fprintf(stderr, "HOST: unix socket path too long: %s\n", path);
        exit(2);
    }
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, strlen(path) + 1);
}

/* Fork the VMM process. The guest runs all rounds on its own, so every mode
 * boots exactly one VM no matter how many connections it uses. */
static void start_vm(const char *root, const char *socket_path, int listener,
                     const char *mode, unsigned long rounds, unsigned long payload_size)
{
    vm_pid = fork();
    if (vm_pid < 0) die("fork");
    if (vm_pid == 0) {
        /* Never run the parent's cleanup against the live guest root. */
        have_temporary = 0;
        pid_t parent = getppid();
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || parent == 1 || getppid() != parent) _exit(2);
        close(listener);
        /* One guest line per round is noise in a session run; the host reads
         * the per-round results from the result file instead. */
        if (rounds > 1) {
            int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (devnull >= 0) {
                if (dup2(devnull, STDOUT_FILENO) < 0) _exit(2);
                close(devnull);
            }
        }

        char rounds_text[32], payload_text[32];
        snprintf(rounds_text, sizeof(rounds_text), "%lu", rounds);
        snprintf(payload_text, sizeof(payload_text), "%lu", payload_size);

        int ctx = krun_create_ctx();
        check(ctx, "krun_create_ctx");
        check(krun_set_vm_config(ctx, 1, 256), "krun_set_vm_config");
        check(krun_set_root(ctx, root), "krun_set_root");
        check(krun_disable_implicit_vsock(ctx), "krun_disable_implicit_vsock");
        check(krun_add_vsock(ctx, 0), "krun_add_vsock");
        check(krun_add_vsock_port2(ctx, VSOCK_PORT, socket_path, false), "krun_add_vsock_port2");
        const char *args[] = {mode, rounds_text, payload_text, NULL};
        const char *env[] = {"PATH=/", "HOME=/", "panic=-1", "oops=panic", NULL};
        check(krun_set_workdir(ctx, "/"), "krun_set_workdir");
        check(krun_set_exec(ctx, "/guest", args, env), "krun_set_exec");
        check(krun_start_enter(ctx), "krun_start_enter");
        _exit(2);
    }
}

static int create_listener(const char *socket_path)
{
    struct sockaddr_un address;
    unix_address(&address, socket_path);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) die("host socket");
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 8))
        die("host listen");
    return listener;
}

/* Accept one guest connection and wait for its readiness byte. */
static int accept_ready(int listener)
{
    int stream = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (stream < 0) die("accept");
    timeout_socket(stream);
    char ready;
    if (recv(stream, &ready, 1, MSG_WAITALL) != 1 || ready != 'R') {
        fprintf(stderr, "HOST: guest readiness byte missing\n");
        close(stream);
        cleanup();
        exit(2);
    }
    return stream;
}

/* Stop every VMM thread so libkrun cannot drain the Unix socket between the
 * write and the close. This is what makes the IN|HUP combination deterministic
 * for the close/keep-open control modes. */
static void stop_vm(void)
{
    if (kill(vm_pid, SIGSTOP)) die("SIGSTOP");
    int status;
    if (waitpid(vm_pid, &status, WUNTRACED) != vm_pid || !WIFSTOPPED(status)) {
        fprintf(stderr, "HOST: VMM did not stop\n");
        cleanup();
        exit(2);
    }
}

/* Read the per-round lines the guest wrote before exiting. */
static void read_results(const char *result_path, struct round_result *results,
                         unsigned long rounds)
{
    FILE *report = fopen(result_path, "r");
    if (!report) {
        fprintf(stderr, "HOST: guest result file missing; infrastructure failure\n");
        cleanup();
        exit(2);
    }
    unsigned long count = 0;
    while (count < rounds &&
           fscanf(report, "%lu %d %zu", &results[count].round, &results[count].failed,
                  &results[count].received) == 3)
        count++;
    fclose(report);
    if (count != rounds) {
        fprintf(stderr, "HOST: guest reported %lu of %lu rounds; infrastructure failure\n",
                count, rounds);
        cleanup();
        exit(2);
    }
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s STATIC_GUEST_BINARY MODE [ROUNDS] [DELAY_US] [--payload SIZE]\n"
            "  close      one round; freeze the VMM, write the payload, close, resume\n"
            "  keep-open  one round; write the payload, keep the socket open until the ACK\n"
            "  race       ROUNDS (default %lu) fresh connections on one VM; the VMM is\n"
            "             never frozen and each socket is closed right after the write\n"
            "  DELAY_US   spin between send and close to widen the drain window (race only)\n"
            "  --payload SIZE   payload bytes per round (default %lu)\n",
            program, DEFAULT_ROUNDS, DEFAULT_PAYLOAD_SIZE);
}

static unsigned long parse_number(const char *text, const char *what, unsigned long maximum)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || *end || value > maximum) {
        fprintf(stderr, "HOST: invalid %s: '%s' (maximum %lu)\n", what, text, maximum);
        exit(2);
    }
    return value;
}

struct options {
    const char *guest_binary;
    enum mode mode;
    unsigned long rounds;
    unsigned long delay_us;
    unsigned long payload_size;
};

static void parse_options(int argc, char **argv, struct options *options)
{
    if (argc < 3) { usage(argv[0]); exit(2); }
    options->guest_binary = argv[1];
    options->rounds = 0;
    options->delay_us = 0;
    options->payload_size = DEFAULT_PAYLOAD_SIZE;

    int mode_index = -1;
    for (int i = 0; i < MODE_COUNT; ++i)
        if (!strcmp(argv[2], mode_names[i])) { mode_index = i; break; }
    if (mode_index < 0) { usage(argv[0]); exit(2); }
    options->mode = (enum mode)mode_index;

    unsigned long positional = 0;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--payload")) {
            if (++i == argc) { usage(argv[0]); exit(2); }
            options->payload_size = parse_number(argv[i], "payload size", MAX_PAYLOAD_SIZE);
        } else if (positional == 0) {
            options->rounds = parse_number(argv[i], "ROUNDS", MAX_ROUNDS);
            positional++;
        } else if (positional == 1) {
            options->delay_us = parse_number(argv[i], "DELAY_US", MAX_DELAY_US);
            positional++;
        } else {
            usage(argv[0]);
            exit(2);
        }
    }

    if (options->mode == MODE_CLOSE || options->mode == MODE_KEEP_OPEN) {
        if (options->rounds > 1 || positional > 1) {
            fprintf(stderr, "HOST: %s runs exactly one round; ROUNDS/DELAY_US do not apply\n",
                    argv[2]);
            exit(2);
        }
        options->rounds = 1;
        options->delay_us = 0;
    } else if (!options->rounds) {
        options->rounds = DEFAULT_ROUNDS;
    }
    if (!options->payload_size) { usage(argv[0]); exit(2); }
    if (options->mode == MODE_CLOSE && options->payload_size > CLOSE_MAX_PAYLOAD) {
        fprintf(stderr, "HOST: close freezes the VMM while writing, so its payload must be at most %lu bytes\n",
                CLOSE_MAX_PAYLOAD);
        exit(2);
    }
}

struct run_stats {
    unsigned long long transfer_us; /* whole loop, excluding VM boot */
    unsigned long long total_send_us;
    unsigned long long max_send_us; /* slowest single round's write */
};

/* Drive the host half of every round; the guest reads the matching results. */
static void run_rounds(const struct options *options, int listener,
                       const unsigned char *payload, struct run_stats *stats)
{
    unsigned long long started = now_us();
    unsigned long long total_send_us = 0, max_send_us = 0;

    for (unsigned long round = 1; round <= options->rounds; ++round) {
        int stream = accept_ready(listener);

        if (options->mode == MODE_CLOSE) {
            stop_vm();
            unsigned long long send_started = now_us();
            send_all(stream, payload, options->payload_size);
            unsigned long long send_us = now_us() - send_started;
            total_send_us += send_us;
            if (send_us > max_send_us) max_send_us = send_us;
            printf("HOST: mode=close sent=%lu bytes while the VMM was frozen (write took %llu us)\n",
                   options->payload_size, send_us);
            close(stream);
            if (kill(vm_pid, SIGCONT)) die("SIGCONT");
            continue;
        }

        if (options->mode == MODE_KEEP_OPEN) {
            unsigned long long send_started = now_us();
            send_all(stream, payload, options->payload_size);
            unsigned long long send_us = now_us() - send_started;
            total_send_us += send_us;
            if (send_us > max_send_us) max_send_us = send_us;
            printf("HOST: mode=keep-open sent=%lu bytes, waiting for the ACK\n",
                   options->payload_size);
            char ack; /* the guest only acknowledges a complete payload */
            if (recv(stream, &ack, 1, MSG_WAITALL) != 1 || ack != 'A') {
                fprintf(stderr, "HOST: complete-payload ACK missing\n");
                cleanup();
                exit(2);
            }
            close(stream);
            continue;
        }

        /* race: no signal is sent to the VMM, the close races libkrun's drain */
        unsigned long long send_started = now_us();
        send_all(stream, payload, options->payload_size);
        unsigned long long send_us = now_us() - send_started;
        total_send_us += send_us;
        if (send_us > max_send_us) max_send_us = send_us;
        if (options->delay_us) spin_us(options->delay_us);
        close(stream);
    }
    close(listener);
    stats->transfer_us = now_us() - started;
    stats->total_send_us = total_send_us;
    stats->max_send_us = max_send_us;
}

int main(int argc, char **argv)
{
    struct options options;
    parse_options(argc, argv, &options);

    setbuf(stdout, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, expired);
    atexit(cleanup);

    /* Boot plus transfers; generous enough for slow CI, bounded for the hang. */
    unsigned long timeout = 60 + (options.rounds > 600 ? 600 : options.rounds);
    timeout += options.payload_size / (1024 * 1024);
    alarm((unsigned)timeout);

    memcpy(temporary, temporary_template, sizeof(temporary));
    if (!mkdtemp(temporary)) die("mkdtemp");
    have_temporary = 1;

    char root[PATH_MAX], guest[PATH_MAX], result_path[PATH_MAX], socket_path[PATH_MAX];
    snprintf(root, sizeof(root), "%s/root", temporary);
    snprintf(guest, sizeof(guest), "%s/root/guest", temporary);
    snprintf(socket_path, sizeof(socket_path), "%s/socket", temporary);
    /* The guest writes RESULT_PATH inside its root, which is this directory. */
    snprintf(result_path, sizeof(result_path), "%s/root%s", temporary, RESULT_PATH);
    if (mkdir(root, 0755)) die("mkdir root");
    copy_file(options.guest_binary, guest);

    unsigned char *payload = malloc(options.payload_size);
    if (!payload) die("malloc payload");
    fill_payload(payload, options.payload_size);

    struct round_result *results = calloc(options.rounds, sizeof(*results));
    if (!results) die("calloc results");

    int listener = create_listener(socket_path);
    start_vm(root, socket_path, listener, mode_names[options.mode],
             options.rounds, options.payload_size);
    struct run_stats stats = {0, 0, 0};
    run_rounds(&options, listener, payload, &stats);

    int status;
    pid_t waited;
    do { waited = waitpid(vm_pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) die("waitpid");
    vm_pid = -1;
    alarm(0);
    read_results(result_path, results, options.rounds);
    cleanup();

    unsigned long failed = 0, partial = 0;
    for (unsigned long i = 0; i < options.rounds; ++i) {
        if (results[i].failed) {
            failed++;
            if (results[i].received) partial++;
        }
        if (options.rounds > 1)
            printf("HOST: round %lu/%lu: %s received=%zu/%lu\n",
                   results[i].round, options.rounds,
                   results[i].failed ? "FAIL (payload loss)" : "PASS",
                   results[i].received, options.payload_size);
    }

    if (options.rounds == 1) {
        printf("HOST: %s: expected=%lu received=%zu; VM wait status=%d\n",
               failed ? "FAIL (payload loss)" : "PASS", options.payload_size,
               results[0].received, status);
    } else {
        printf("HOST: summary: mode=%s payload=%lu rounds=%lu triggered=%lu complete=%lu "
               "partial=%lu trigger_rate=%.1f%% delay_us=%lu mean_round_us=%.1f "
               "mean_write_us=%.1f max_write_us=%llu\n",
               mode_names[options.mode], options.payload_size, options.rounds, failed,
               options.rounds - failed, partial,
               100.0 * (double)failed / (double)options.rounds, options.delay_us,
               (double)stats.transfer_us / (double)options.rounds,
               (double)stats.total_send_us / (double)options.rounds, stats.max_send_us);
    }
    return failed ? 1 : 0;
}
#endif
