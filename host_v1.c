/* Host half for libkrun 1.x: the flat krun_* API with a single global context.
 *
 * The guest root is installed with krun_set_root() and the payload runs through
 * libkrun's own init, so the guest binary only has to exist in the root.
 */
/* accept4(), nftw(), MSG_NOSIGNAL and friends are GNU extensions, so this
 * must come before any system header. */
#define _GNU_SOURCE 1
#include "harness.h"
#include <libkrun.h>

static void check(int result, const char *what)
{
    if (result < 0) { errno = -result; die(what); }
}

static void vm_run(const char *root, const char *socket_path, const char *mode,
                   unsigned long rounds, unsigned long payload_size)
{
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
}

int main(int argc, char **argv)
{
    static const struct vm_backend backend = { .run = vm_run, .name = "libkrun 1.x" };
    return harness_main(argc, argv, &backend);
}
