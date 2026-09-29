# libkrun vsock tail loss

## Problem

When a client connects to a vsock port backed by a host Unix socket, writes a
payload, and closes the socket, bytes that are still unread in the socket buffer
are discarded. The guest either receives nothing and an immediate EOF, or receives
a prefix of the payload followed by EOF. The kernel behaviour behind this sequence
is legal in both orderings; the loss happens because libkrun treats a socket hangup
as a reason to tear the connection down instead of draining the remaining bytes
first.

The behaviour is present in the 1.19.0 release and unchanged on `main`
(2.0.0-dev), as the measurements below show.

## Modes under test

| Mode | VMM during write/close | Per round |
| --- | --- | --- |
| `close` | suspended with `SIGSTOP` | write the payload, close the socket, resume the VM |
| `keep-open` | running | write the payload, hold the socket open until the guest acknowledges, then close |
| `race` | running | write the payload, close immediately, on a fresh connection |

`close` reproduces the failure deterministically: with every VMM thread stopped,
the kernel observes the write and the hangup together, so the payload is always
lost. It is the original test's mode and the recommended one for verifying a fix,
since a single run is conclusive.

`keep-open` is the control. It performs the same write and keeps the socket open
until the guest has acknowledged the complete payload, which isolates the close as
the cause of the loss. In `keep-open` the guest acknowledges only a complete,
uncorrupted payload, so a lost payload would fail the run.

`race` does not suspend the VM. It replays the ordinary client sequence - write,
close, no delay - and reports the fraction of fresh connections that lose data. It
measures how often the failure occurs in practice and how wide the timing window is.

## Environment

| | |
| --- | --- |
| Host kernel | `7.2.8-300.fc45.x86_64` |
| libkrun 1.x backend | system `libkrun` 1.19.0 |
| libkrun 2.x backend | `main` at `1f5dd028`, built locally as 2.0.0 |
| Compiler | GCC 16.2.1 |
| CPUs | 20 |
| Guest | one static binary, 1 vCPU, 256 MiB |
| `net.core.wmem_default` / `rmem_default` | 212992 (208 KiB) |

All measurements ran inside this repository's `toolbox` container with access to
`/dev/kvm`. Every mode boots a single VM and the guest opens one fresh connection
per round, so a 500-round run is 500 connections served by one VM.

## Results on 1.19.0

`close`, VM suspended between write and close:

| Payload | Result | Bytes received |
| --- | --- | --- |
| 1 KiB | FAIL | 0 |
| 16 KiB | FAIL | 0 |
| 64 KiB | FAIL | 0 |
| 128 KiB | FAIL | 0 |

`keep-open`, VM running and not signalled, 30 runs per size:

| Payload | Runs | Passed |
| --- | --- | --- |
| 1 KiB | 30 | 30 |
| 16 KiB | 30 | 30 |
| 32 KiB | 30 | 30 |
| 64 KiB | 30 | 30 |
| 384 KiB | 30 | 30 |
| 512 KiB | 30 | 30 |
| 1 MiB | 30 | 30 |
| 4 MiB | 30 | 30 |

The two tables differ only in whether the socket survives until the guest has
drained it.

`race`, no suspension, 500 rounds per payload size:

| Payload | Triggered | Rate | Guest received | Write cost, mean / max |
| --- | --- | --- | --- | --- |
| 1 KiB | 496 | 99.2% | 0 bytes | 2 µs / 29 µs |
| 4 KiB | 495 | 99.0% | 0 bytes | 2 µs / 76 µs |
| 16 KiB | 490 | 98.0% | 0 bytes | 4 µs / 53 µs |
| 32 KiB | 499 | 99.8% | 0 bytes | 8 µs / 47 µs |
| 48 KiB | 148 | 29.6% | 0 bytes | 8 µs / 31 µs |
| 64 KiB | 20 | 4.0% | 0 bytes | 8 µs / 52 µs |
| 128 KiB | 5 | 1.0% | 0 bytes | 13 µs / 53 µs |
| 256 KiB | 6 | 1.2% | partial | 50 µs / 307 µs |
| 384 KiB | 414 | 82.8% | partial | 144 µs / 557 µs |
| 512 KiB | 443 | 88.6% | partial | 404 µs / 1086 µs |
| 1 MiB | 429 | 85.8% | partial | 1746 µs / 5604 µs |
| 4 MiB | 428 | 85.6% | partial | 9408 µs / 12235 µs |

Write cost is the time the host spends inside `send()`. It stays in the
microsecond range up to 256 KiB and then grows by orders of magnitude, which is
where the payload stops fitting in the 208 KiB socket buffer and backpressure
appears.

When the failure is partial, the lost region is bounded by the socket buffer:

| Payload | Failed rounds | Delivered | Lost |
| --- | --- | --- | --- |
| 384 KiB | 232 / 300 | 66.7% .. 98.9% | 4 KiB .. 128 KiB |
| 1 MiB | 256 / 300 | 80.2% .. 99.6% | 4.6 KiB .. 203 KiB |
| 4 MiB | 261 / 300 | 95.0% .. 100.0% | 1.8 KiB .. 205 KiB |

The largest loss observed was 209796 bytes, against `net.core.rmem_default` of
212992. Above 32 KiB the loss is always a prefix followed by EOF.

`race` with a busy-wait between `send()` and `close()`, 16 KiB, 500 rounds each:

| Gap | Triggered | Rate |
| --- | --- | --- |
| 0 µs | 499 | 99.8% |
| 1 µs | 466 | 93.2% |
| 2 µs | 131 | 26.2% |
| 3 µs | 44 | 8.8% |
| 4 µs | 14 | 2.8% |
| 5 µs | 13 | 2.6% |
| 10 µs | 7 | 1.4% |
| 20 µs | 1 | 0.2% |
| 50 µs | 0 | 0.0% |

The delay is a busy-wait on `CLOCK_MONOTONIC`; `nanosleep` rounds sub-millisecond
delays up to the timer slack (tens of microseconds) and would hide the window. The
window is roughly 20-50 µs wide.

## Results on main (2.0.0-dev)

The same program rebuilt against `main` using the 2.x backend.

`close` fails on every run (guest received 0 of 16384 bytes, three of three), and
`keep-open` passed 20 of 20 runs at 1 KiB, 16 KiB, 256 KiB, 1 MiB and 4 MiB.

`race`, no suspension, 300 rounds per payload size:

| Payload | Triggered | Rate | Write cost, mean / max |
| --- | --- | --- | --- |
| 1 KiB | 299 | 99.7% | 2 µs / 25 µs |
| 4 KiB | 296 | 98.7% | 3 µs / 20 µs |
| 16 KiB | 292 | 97.3% | 5 µs / 30 µs |
| 32 KiB | 296 | 98.7% | 8 µs / 37 µs |
| 64 KiB | 12 | 4.0% | 9 µs / 68 µs |
| 128 KiB | 1 | 0.3% | 13 µs / 50 µs |
| 256 KiB | 2 | 0.7% | 50 µs / 156 µs |
| 512 KiB | 270 | 90.0% | 396 µs / 1333 µs |
| 1 MiB | 255 | 85.0% | 1807 µs / 3056 µs |
| 4 MiB | 259 | 86.3% | 9280 µs / 11753 µs |

The write-to-close window on `main` has the same shape (16 KiB, 300 rounds): 98.7%
at 0 µs, 53.7% at 2 µs, 25.0% at 3 µs, 7.0% at 5 µs, 1.0% at 20 µs, 0.7% at 50 µs.

## Root cause

`main`: `src/devices/src/virtio/vsock/unix_proxy/unix.rs`, function
`process_event()`. The 1.19.0 release has the same function in
`src/devices/src/virtio/vsock/unix.rs`; the code is identical apart from the file
split.

```rust
if evset.contains(EventSet::HANG_UP) {
    if proxy.status == ProxyStatus::Connecting {
        push_connect_rsp(proxy, -libc::ECONNREFUSED);
    } else {
        proxy.push_reset();          // unread bytes in the socket are dropped
    }
    proxy.status = ProxyStatus::Closed;
    update.remove_proxy = ProxyRemoval::Deferred;
    return update;                   // returns before the IN branch runs
}

if evset.contains(EventSet::IN) {
    ...                              // drains the socket
}
```

`EventSet::HANG_UP` is `EPOLLHUP`, which epoll reports together with `EPOLLIN`
whenever the peer has closed. If the peer writes and closes while bytes are still
queued, the event set is `IN|HUP`, the hangup branch runs first, and the queued
bytes are dropped instead of drained.

The payload-size dependence follows from how much of the payload the drain has
already taken when the event is handled:

- Up to 32 KiB nothing has been drained, so the whole payload is dropped and the
  guest sees an immediate EOF.
- 48 KiB to 256 KiB part of the payload reaches the guest first, so the drain
  usually wins and the loss rate drops to 1-30%.
- 384 KiB and above the payload exceeds the socket buffer, so `send()` only
  completes after the drain has taken part of it; the tail still queued at close is
  dropped, bounded by the socket buffer size (208 KiB).

### Deferring the hangup

A local probe moved the `HANG_UP` block after the `IN` block, rebuilt libkrun, ran
the test, and then restored the source (the checkout was left byte-identical):

| Case | Unpatched | `IN` before `HANG_UP` |
| --- | --- | --- |
| `close`, 16 KiB | 0 / 16384 bytes, FAIL | 16384 / 16384 bytes, PASS |
| `race`, 16 KiB, 100 rounds | 97.3% lost | 0.0% lost |
| `race`, 1 KiB, 100 rounds | 98.0% lost | 0.0% lost |
| `race`, 1 MiB, 50 rounds | 78.0% lost | 80.0% lost |

Reordering removes the failure when the payload fits in the receive virtqueue, which
identifies the ordering as the cause. It does not fix larger payloads: a burst
bigger than the available receive queue is drained over several passes, and
`recv_pkt()` returns as soon as the queue is empty, leaving the remainder in the
socket, so the hangup still discards data.

A complete fix has to keep the proxy alive and continue draining until `recv()`
reports the socket is empty (the existing `RecvPkt::Close` path) instead of treating
`EPOLLHUP` as a teardown condition.

## Reproduce

```sh
# 1.x backend, system libkrun
toolbox run cmake -S tests/repro/vsock-tail -B build-v1 -G Ninja
toolbox run cmake --build build-v1
toolbox run ./build-v1/vsock-tail ./build-v1/vsock-tail-guest close
toolbox run ./build-v1/vsock-tail ./build-v1/vsock-tail-guest race 500 --payload 16384

# 2.x backend against libkrun main, installed into /tmp/krun-main-prefix
toolbox run make -C /var/home/yan/code/libkrun FFI=1
toolbox run make -C /var/home/yan/code/libkrun install FFI=1 PREFIX=/tmp/krun-main-prefix
toolbox run env PKG_CONFIG_PATH=/tmp/krun-main-prefix/lib64/pkgconfig \
    cmake -S tests/repro/vsock-tail -B build-v2 -G Ninja
toolbox run env PKG_CONFIG_PATH=/tmp/krun-main-prefix/lib64/pkgconfig cmake --build build-v2
toolbox run env LD_LIBRARY_PATH=/tmp/krun-main-prefix/lib64 \
    ./build-v2/vsock-tail ./build-v2/vsock-tail-guest race 300 --payload 16384
```

Exit code 1 means the run reproduced the data loss, 2 means the harness or the
runtime failed, and 124 means it hung. With the proxy fixed, `close` should pass and
`race` should report `triggered=0` for every payload size and delay.
