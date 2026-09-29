# libkrun vsock tail-loss reproducer

A standalone Linux C project that reproduces data loss when a vsock-backed Unix
socket is closed while unread data is still queued in it. The directory can be
copied out and built on its own; it does not depend on agent-vm sources,
configuration or build products.

The host half exists twice because libkrun 2.x replaced the 1.x API. CMake
selects one from the installed libkrun version:

| File | API | Guest root | Payload entry |
| --- | --- | --- | --- |
| `host_v1.c` | libkrun 1.19+ (`krun_create_ctx`, `krun_set_root`, `krun_start_enter`) | `krun_set_root()` | `krun_set_exec()` |
| `host_v2.c` | libkrun 2.x (`krun_vmm_builder_new`, devices, `krun_vmm_run`) | virtiofs `KrunFsDevice` over a host directory | `KrunInitConfig` injected through a `KrunFsOverlay` |

The guest, the modes, option parsing, timing and reporting are shared, so the two
backends produce comparable numbers. Measurements and the root-cause analysis are
in [REPORT.md](REPORT.md).

## Dependencies and build

Requirements: a C17 compiler, CMake 3.20+, Ninja, pkg-config, libkrun development
files, and the static C library for your compiler (usually `glibc-static` on
GCC/glibc). The 2.x backend additionally needs `libkrun_init`, which is installed
with libkrun. Running the reproducer needs libkrun's runtime dependencies and
read/write access to `/dev/kvm`.

Build in this directory. Local development in this repository uses toolbox:

```sh
toolbox run cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
toolbox run cmake --build build
```

After copying the directory elsewhere, drop the `toolbox run` prefix if the
dependencies are installed in the current environment.

To build against a libkrun source tree instead of the system copy, point
`PKG_CONFIG_PATH` at its install prefix:

```sh
toolbox run make -C /var/home/yan/code/libkrun FFI=1
toolbox run make -C /var/home/yan/code/libkrun install FFI=1 PREFIX=/tmp/krun-main-prefix
toolbox run env PKG_CONFIG_PATH=/tmp/krun-main-prefix/lib64/pkgconfig \
    cmake -S . -B build-main -G Ninja
toolbox run env PKG_CONFIG_PATH=/tmp/krun-main-prefix/lib64/pkgconfig cmake --build build-main
toolbox run env LD_LIBRARY_PATH=/tmp/krun-main-prefix/lib64 \
    ./build-main/vsock-tail ./build-main/vsock-tail-guest race 300 --payload 16384
```

The build produces two executables:

- `build/vsock-tail` - the host program that links libkrun and acts as the client.
- `build/vsock-tail-guest` - a static guest program that acts as the server. It
  needs no rootfs, since the host shares its directory with virtiofs.

## Modes

All modes boot a single VM. The guest opens one fresh connection per round, so a
500-round run is 500 connections served by one VM.

```sh
toolbox run ./build/vsock-tail ./build/vsock-tail-guest MODE [ROUNDS] [DELAY_US] [--payload SIZE]
```

| Mode | VMM during write/close | Per round | Purpose |
| --- | --- | --- | --- |
| `close` | suspended with `SIGSTOP` | write the payload, close the socket, resume the VM | determines the failure on every run |
| `keep-open` | running | write the payload, hold the socket open until the guest acknowledges, then close | control |
| `race` | running | write the payload, close immediately, then repeat on a fresh connection | probability of the failure on an ordinary close |

`close` suspends every VMM thread, so the kernel observes the write and the hangup
together and the unread bytes are dropped. This is the original test's behaviour
and it fails reproducibly on every run. `keep-open` performs the same write but
keeps the socket alive until the guest has drained it, which isolates the close as
the cause. `race` sends no signal to the VM and records how often an ordinary
write-then-close loses data.

Arguments:

- `ROUNDS` - rounds for `race`, default 20. `close` and `keep-open` always run one
  round.
- `DELAY_US` - `race` only. Busy-waits between `send()` and `close()` to widen the
  window in which the drain can complete.
- `--payload SIZE` - payload bytes per round, default 16384. `close` cannot drain
  while the VM is suspended, so it accepts at most 131072 bytes. Other modes accept
  up to 64 MiB.

## Output

Each round prints what the guest received, and the host prints a per-run summary:

```sh
toolbox run ./build/vsock-tail ./build/vsock-tail-guest race 200 --payload 16384
```

```
HOST: round 1/200: FAIL (payload loss) received=0/16384
...
HOST: summary: mode=race payload=16384 rounds=200 triggered=197 complete=3 partial=0 \
      trigger_rate=98.5% delay_us=0 mean_round_us=384.0 mean_write_us=4.2 max_write_us=53
```

`triggered` counts rounds with lost or corrupt data and `partial` counts those that
received some bytes rather than none. `mean_round_us` is the mean per-connection
cost, and `mean_write_us` / `max_write_us` are the time spent inside `send()`,
which shows when the payload exceeds the socket buffer and backpressure appears.

Exit codes:

- `0` - every round delivered the full payload intact.
- `1` - data was lost or corrupt; the failure was reproduced.
- `2` - harness, boot or protocol failure; not a reproduction.
- `124` - the host timed out.

The program creates its temporary guest root and Unix socket under
`/tmp/krun-tail-*` and removes them on a clean exit. A timeout or a forced kill can
leave them behind. Only local Unix sockets and vsock are used.

## Files

| File | Contents |
| --- | --- |
| `CMakeLists.txt` | selects the backend from the libkrun version, builds both executables |
| `common.h` | socket helpers and constants shared by host and guest |
| `guest.c` | guest program: receives each payload, verifies it, writes the result file |
| `harness.h` | shared host logic: modes, arguments, timing, round loop, result summary |
| `host_v1.c` | libkrun 1.x backend |
| `host_v2.c` | libkrun 2.x backend |
