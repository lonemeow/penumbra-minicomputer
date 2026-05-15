# pbench baselines

Captured `pbench` results across runs.  Add new snapshots at the **top**,
so the most recent state is what someone reading from line 1 sees first.

## Reading the table

- **min** — fastest trial (lowest-noise observation, headline number)
- **med** — median trial (typical, captures jitter)
- **mean** — average (sensitive to outliers)
- **iters** — per-trial iteration count, chosen by harness calibration
  to target ~50 ms of wall-clock per trial

Each row reports nanoseconds-per-operation, auto-scaled to ns / us / ms / s
based on the median.

## Snapshot index

- [2026-05-15 — initial baseline](#2026-05-15--initial-baseline)

---

## 2026-05-15 — initial baseline

**Captured**: 2026-05-15
**Repository state**: commit `08436ea8ffcc` plus uncommitted benchmark
 work (this file is part of the commit that anchors here).  Future
 snapshots should record both the date and the HEAD SHA so a `git
 log <sha>..` shows what changed between snapshots.
**Platform**: ULX3S FPGA, 25 MHz CPU clock, running NetBSD off SD card.
**Binary**: `pbench` (dynamically linked against libc.so)
**Notes**: First successful run after fixing
 - kernel `CLOCK_MONOTONIC` monotonicity,
 - clang value-propagation through `static volatile` function pointers
   (moved volatile reads into per-iter loop body).

```
--- kernel/getpid ---
  kernel   getpid             -               min=     2.64 ms  med=     2.67 ms  mean=     3.21 ms  iters=15

--- kernel/clock_gettime ---
  kernel   clock_gettime      -               min=     1.13 ms  med=     1.30 ms  mean=     1.26 ms  iters=44

--- kernel/pipe_pingpong ---
  kernel   pipe_pingpong      1B              min=    19.34 ms  med=    19.71 ms  mean=    19.70 ms  iters=1

--- kernel/fork_exit ---
  kernel   fork_exit          -               min=   600.91 ms  med=   690.41 ms  mean=   664.65 ms  iters=1

--- libc/memcpy ---
  libc     memcpy             size=1          min=     5.77 us  med=     6.03 us  mean=     6.13 us  iters=8409
  libc     memcpy             size=16         min=     9.46 us  med=     9.86 us  mean=    10.20 us  iters=4431
  libc     memcpy             size=64         min=    26.95 us  med=    26.97 us  mean=    26.98 us  iters=359
  libc     memcpy             size=256        min=    82.40 us  med=    82.63 us  mean=    86.03 us  iters=509
  libc     memcpy             size=1024       min=   330.79 us  med=   348.66 us  mean=   389.02 us  iters=123
  libc     memcpy             size=4096       min=     1.76 ms  med=     2.00 ms  mean=     2.26 ms  iters=29
  libc     memcpy             size=16384      min=     6.88 ms  med=     7.35 ms  mean=     8.07 ms  iters=5
  libc     memcpy             size=65536      min=    28.81 ms  med=    29.31 ms  mean=    40.28 ms  iters=1

--- libc/memset ---
  libc     memset             size=1          min=     5.24 us  med=     5.98 us  mean=     6.80 us  iters=7479
  libc     memset             size=16         min=     8.70 us  med=     9.06 us  mean=     9.88 us  iters=4929
  libc     memset             size=64         min=    19.43 us  med=    20.52 us  mean=    21.06 us  iters=2153
  libc     memset             size=256        min=    62.86 us  med=    63.46 us  mean=    63.85 us  iters=653
  libc     memset             size=1024       min=   238.30 us  med=   247.48 us  mean=   308.36 us  iters=211
  libc     memset             size=4096       min=     0.97 ms  med=     1.14 ms  mean=     1.29 ms  iters=47
  libc     memset             size=16384      min=     3.83 ms  med=     4.03 ms  mean=     4.28 ms  iters=12
  libc     memset             size=65536      min=    15.18 ms  med=    15.33 ms  mean=    15.90 ms  iters=2

--- libc/strlen ---
  libc     strlen             len=8           min=     8.24 us  med=     8.63 us  mean=     8.77 us  iters=5333
  libc     strlen             len=64          min=    37.02 us  med=    39.67 us  mean=    40.67 us  iters=828
  libc     strlen             len=256         min=   135.42 us  med=   140.29 us  mean=   144.82 us  iters=362
  libc     strlen             len=1024        min=   538.27 us  med=   569.49 us  mean=   603.49 us  iters=77
  libc     strlen             len=4096        min=     2.53 ms  med=     2.70 ms  mean=     2.91 ms  iters=16
```

### System I/O (dd, not pbench)

Throughput probes via `dd(1)` from the single-user shell, captured
on the same platform as the pbench results above (ULX3S FPGA,
25 MHz CPU clock).  These are not part of `pbench` and are recorded
here because the per-syscall costs measured above don't capture
*streaming* throughput, and a "how fast does the disk read?" /
"how fast does the kernel copy?" number is useful baseline context.

```
# dd if=/dev/ld0 of=/dev/null bs=32k count=100
100+0 records in
100+0 records out
3276800 bytes transferred in 53.564 secs (61175 bytes/sec)

# dd if=/dev/zero of=/dev/null bs=32k count=1000
1000+0 records in
1000+0 records out
32768000 bytes transferred in 23.107 secs (1418098 bytes/sec)
```

Interpretation:
- `/dev/ld0` (SD card via polled `pmci`): **61 KB/s read** — the
  end-to-end SD-MMC stack is the bottleneck, not the CPU.  Once
  SPI FIFO + IRQ-driven `pmci` lands (Phase 3.5 in `doc/TODO.md`),
  this should jump substantially.
- `/dev/zero` → `/dev/null`: **1.4 MB/s** of pure read+write syscall
  bandwidth, no real I/O, no userland data touch.  This is the
  upper bound for any disk-to-disk `dd` until syscall overhead
  drops.  At 32 KB per syscall, that's ~22 ms per read+write pair,
  which lines up with the ~1.13 ms `clock_gettime` floor scaled
  for the larger uiomove path.

### Reading this snapshot

- **Per-byte throughput (steady state, biggest size)**
  - `memcpy`  size=65536 ⇒ 28.81 ms / 65536 B ≈ **440 ns/byte**
  - `memset`  size=65536 ⇒ 15.18 ms / 65536 B ≈ **232 ns/byte** (~half of memcpy — no read traffic, write-through cache)
  - `strlen`  len=4096   ⇒  2.53 ms /  4096 B ≈ **618 ns/byte** (byte-at-a-time scan)
- **Per-call floor (smallest size)**
  - `memcpy`  size=1: 5.77 µs — overhead dominates: volatile load + indirect call + clock-syscall pair
  - `getpid`:        2.64 ms — bare syscall round-trip
  - `clock_gettime`: 1.13 ms — common-path syscall; faster than getpid (anomaly worth understanding later)
- **Heavyweight paths**
  - `fork_exit`:     600 ms per fork+wait — pmap setup + page-table teardown
  - `pipe_pingpong`:  19 ms per round-trip — 2 context switches + 4 syscalls
- **`getpid` is slower than `clock_gettime` here**, which is upside-down vs. typical Unix expectations.
  `getpid` should be the cheapest possible syscall.  Worth tracing once kernel work resumes —
  possibly indicates extra work on the getpid path (locking? curlwp lookup?) that doesn't
  apply to clock_gettime.
