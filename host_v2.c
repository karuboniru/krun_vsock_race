/* Host half for libkrun 2.x: the builder API.
 *
 * 2.x has no single global context. The VM is assembled from a payload (the
 * krunfw kernel bundle), an init configuration that names the guest payload, and
 * an MMIO device manager holding the filesystem, console and vsock devices.
 *
 * The guest root is a plain host directory shared with virtiofs, which keeps the
 * test free of image files: the guest binary is simply present in that
 * directory, and the init configuration injected into it execs /guest.
 */
/* accept4(), nftw(), MSG_NOSIGNAL and friends are GNU extensions, so this
 * must come before any system header. */
#define _GNU_SOURCE 1
#include "harness.h"
#include <libkrun.h>
#include <libkrun_init.h>

static bool push_to_stderr(void *userdata, KrunStr s)
{
    (void)userdata;
    fwrite(s.data, 1, s.len, stderr);
    return true;
}

/* KRUN_VTABLE_HANDLE() wraps a compound literal, which is not a constant
 * expression for a file-scope initializer, so spell the two handles out. */
static const KrunPushStrVtable vmm_error_vtable = { .drop = NULL, .push = push_to_stderr };
static KrunVtableHandle vmm_stderr_writer = {
    .type_tag = KRUN_PUSH_STR_TYPE_TAG,
    .vtable_ptr = &vmm_error_vtable,
    .user_data = NULL,
    .vtable_size = sizeof(vmm_error_vtable),
};

static const KrunInitPushStrVtable init_error_vtable = { .drop = NULL, .push = push_to_stderr };
static KrunVtableHandle init_stderr_writer = {
    .type_tag = KRUN_INIT_PUSH_STR_TYPE_TAG,
    .vtable_ptr = &init_error_vtable,
    .user_data = NULL,
    .vtable_size = sizeof(init_error_vtable),
};

static KrunError krun_error;
static KrunInitError init_error;

static void fail_krun(const char *what)
{
    flockfile(stderr);
    fprintf(stderr, "HOST: %s failed: ", what);
    if (krun_error) {
        krun_error_message(krun_error, &vmm_stderr_writer);
        krun_error_destroy(krun_error);
        krun_error = NULL;
    }
    fputc('\n', stderr);
    funlockfile(stderr);
    _exit(2);
}

static void fail_init(const char *what)
{
    flockfile(stderr);
    fprintf(stderr, "HOST: %s failed: ", what);
    if (init_error) {
        krun_init_error_message(init_error, &init_stderr_writer);
        krun_init_error_destroy(init_error);
        init_error = NULL;
    }
    fputc('\n', stderr);
    funlockfile(stderr);
    _exit(2);
}

/* Run a call that reports its failure through krun_error. */
#define KRUN_CHECK(call) do { krun_error = NULL; call; if (krun_error) fail_krun(#call); } while (0)

static void vm_run(const char *root, const char *socket_path, const char *mode,
                   unsigned long rounds, unsigned long payload_size)
{
    char rounds_text[32], payload_text[32];
    snprintf(rounds_text, sizeof(rounds_text), "%lu", rounds);
    snprintf(payload_text, sizeof(payload_text), "%lu", payload_size);

    KRUN_CHECK(krun_init_log(-1, KRUN_LOG_LEVEL_WARN, KRUN_LOG_STYLE_AUTO, 0, &krun_error));

    /* Kernel bundle (kernel plus the init that mounts the root below). */
    krun_error = NULL;
    KrunPayload payload = krun_payload_load_krunfw(&krun_error);
    if (!payload) fail_krun("krun_payload_load_krunfw");

    /* The root filesystem carries the guest binary; the overlay carries the
     * init configuration that tells the guest init to exec it. */
    KrunFsOverlay overlay = krun_fs_overlay_new();
    if (!overlay) fail_krun("krun_fs_overlay_new");

    KrunInitBuilder init_builder = krun_init_config_builder();
    if (!init_builder) fail_krun("krun_init_config_builder");
    krun_init_builder_arg(&init_builder, KRUN_STR("/guest"));
    krun_init_builder_arg(&init_builder, KRUN_STR(mode));
    krun_init_builder_arg(&init_builder, KRUN_STR(rounds_text));
    krun_init_builder_arg(&init_builder, KRUN_STR(payload_text));
    krun_init_builder_env_var(&init_builder, KRUN_STR("PATH=/"));
    krun_init_builder_env_var(&init_builder, KRUN_STR("HOME=/"));
    krun_init_builder_workdir(&init_builder, KRUN_STR("/"));
    KrunInitConfig init_config = krun_init_builder_build(&init_builder);
    if (!init_config) fail_krun("krun_init_builder_build");
    init_error = NULL;
    if (krun_init_config_apply(init_config, overlay, payload, &init_error) != KRUN_RESULT_SUCCESS)
        fail_init("krun_init_config_apply");

    KrunMmioDeviceManager devices = krun_mmio_device_manager_new();
    if (!devices) fail_krun("krun_mmio_device_manager_new");

    /* Root filesystem: the host directory holding the guest binary. */
    krun_error = NULL;
    KrunFsDevice rootfs = krun_fs_device_new(KRUN_STR("/dev/root"), KRUN_STR(root), &krun_error);
    if (!rootfs) fail_krun("krun_fs_device_new");
    krun_fs_device_set_overlay(rootfs, overlay);
    krun_mmio_device_manager_add(devices, rootfs);

    /* Console, so the guest can report what it received. */
    KrunConsoleBuilder console_builder = krun_console_device_builder();
    if (!console_builder) fail_krun("krun_console_device_builder");
    KRUN_CHECK(krun_console_builder_add_default_console(console_builder, STDIN_FILENO,
                                                        STDOUT_FILENO, STDERR_FILENO,
                                                        &krun_error));
    krun_error = NULL;
    KrunConsoleDevice console = krun_console_builder_build(console_builder, &krun_error);
    if (!console) fail_krun("krun_console_builder_build");
    krun_mmio_device_manager_add(devices, console);

    /* Built-in vsock with the Unix socket port mapping the test needs. */
    krun_error = NULL;
    KrunVsockDevice vsock = krun_vsock_device_new(3, 0, &krun_error);
    if (!vsock) fail_krun("krun_vsock_device_new");
    krun_vsock_device_add_unix_port(vsock, VSOCK_PORT, KRUN_STR(socket_path), false);
    krun_mmio_device_manager_add(devices, vsock);

    KrunVmmBuilder builder = krun_vmm_builder_new();
    if (!builder) fail_krun("krun_vmm_builder_new");
    KRUN_CHECK(krun_vmm_builder_vcpus(&builder, 1, &krun_error));
    KRUN_CHECK(krun_vmm_builder_ram_mib(&builder, 256, &krun_error));
    krun_vmm_builder_payload(&builder, payload);
    krun_vmm_builder_devices(&builder, devices);
    KRUN_CHECK(krun_vmm_builder_split_irqchip(&builder, false, &krun_error));
#if defined(__x86_64__)
    KRUN_CHECK(krun_vmm_builder_acpi(&builder, false, &krun_error));
#endif

    krun_error = NULL;
    KrunVmm vmm = krun_vmm_builder_build(&builder, &krun_error);
    if (!vmm) fail_krun("krun_vmm_builder_build");
    krun_vmm_run(vmm); /* never returns */
}

int main(int argc, char **argv)
{
    static const struct vm_backend backend = { .run = vm_run, .name = "libkrun 2.x" };
    return harness_main(argc, argv, &backend);
}
