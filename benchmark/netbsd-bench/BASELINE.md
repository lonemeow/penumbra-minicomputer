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

- [2026-05-19 — L2 cache + SPI FIFO + pmap speedups](#2026-05-19--l2-cache--spi-fifo--pmap-speedups)
- [2026-05-15 — initial baseline](#2026-05-15--initial-baseline)

---

## 2026-05-19 — L2 cache + SPI FIFO + pmap speedups

**Captured**: 2026-05-19
**Repository state**: commit `d5d929f9d011`.  `git log 08436ea8ffcc..d5d929f9d011`
 covers the delta from the previous snapshot — headline changes:
 - L2 cache phase 1 brought up (`hw: L2 phase 1 — real l2_cache`,
   `netbsd: enable L2 cache during kernel bring-up`).  64 KiB unified,
   4-way SA, 16-byte lines, write-invalidate-on-hit.
 - SD data phase moved off polled-single-byte onto the SPI FIFO engine
   (`netbsd: drive SD data phase through SPI FIFO engine`,
   `netbsd: handle CMD18/CMD25 multi-block natively in pmci`).
 - UART promoted to NS16550A with 16-byte FIFOs.
 - `pmap_enter` fast path (`netbsd: speed up pmap_enter via cached
   vm_page + same-PA fast path`).
 - `cpu_bus_arbiter` pipelined to eliminate dead cycles on burst fills.
**Platform**: ULX3S FPGA, 25 MHz CPU clock, running NetBSD off SD card.
**Binary**: `pbench` (dynamically linked against libc.so), updated
 harness now reports `trials=` alongside `iters=`.
**Notes**: First snapshot since the L2 cache plus the SD-path and
 pmap rework landed.  Attribute carefully — kernel I-fetch wins are
 L2, dd ld0 is L2 + SPI FIFO + pmci multi-block, fork_exit is L2 +
 pmap_enter fast path.

```
--- kernel/getpid ---
  kernel   getpid             -               min=   249.25 us  med=   261.66 us  mean=   265.93 us  iters=1024  trials=19

--- kernel/clock_gettime ---
  kernel   clock_gettime      -               min=   626.96 us  med=   951.13 us  mean=   916.85 us  iters=512  trials=11

--- kernel/pipe_pingpong ---
  kernel   pipe_pingpong      1B              min=     9.13 ms  med=     9.47 ms  mean=     9.63 ms  iters=32  trials=17

--- kernel/fork_exit ---
  kernel   fork_exit          -               min=   426.55 ms  med=   457.77 ms  mean=   466.28 ms  iters=1  trials=11

--- libc/memcpy ---
  libc     memcpy             size=1          min=     5.16 us  med=     5.27 us  mean=     5.39 us  iters=65536  trials=15
  libc     memcpy             size=16         min=     8.39 us  med=     8.69 us  mean=     8.83 us  iters=32768  trials=18
  libc     memcpy             size=64         min=    21.18 us  med=    22.00 us  mean=    22.21 us  iters=16384  trials=14
  libc     memcpy             size=256        min=    72.52 us  med=    74.94 us  mean=    76.38 us  iters=4096  trials=16
  libc     memcpy             size=1024       min=   279.45 us  med=   286.06 us  mean=   291.63 us  iters=1024  trials=17
  libc     memcpy             size=4096       min=     1.23 ms  med=     1.27 ms  mean=     1.28 ms  iters=256  trials=16
  libc     memcpy             size=16384      min=     4.97 ms  med=     5.05 ms  mean=     5.24 ms  iters=64  trials=15
  libc     memcpy             size=65536      min=    25.05 ms  med=    25.97 ms  mean=    26.59 ms  iters=8  trials=24

--- libc/memcpy_align ---
  libc     memcpy_align       n=256,src=1,dst=0  min=   250.36 us  med=   261.62 us  mean=   263.38 us  iters=1024  trials=19
  libc     memcpy_align       n=256,src=0,dst=1  min=   251.53 us  med=   260.25 us  mean=   263.17 us  iters=1024  trials=19
  libc     memcpy_align       n=256,src=1,dst=1  min=   250.05 us  med=   257.60 us  mean=   262.22 us  iters=1024  trials=19
  libc     memcpy_align       n=256,src=1,dst=3  min=   248.16 us  med=   259.19 us  mean=   262.44 us  iters=1024  trials=19
  libc     memcpy_align       n=7             min=     8.07 us  med=     8.36 us  mean=     8.53 us  iters=32768  trials=18
  libc     memcpy_align       n=31            min=    14.32 us  med=    15.13 us  mean=    15.17 us  iters=16384  trials=20
  libc     memcpy_align       n=127           min=    39.83 us  med=    41.37 us  mean=    41.69 us  iters=8192  trials=15
  libc     memcpy_align       n=255           min=    73.85 us  med=    76.52 us  mean=    77.17 us  iters=4096  trials=16

--- libc/memset ---
  libc     memset             size=1          min=     4.72 us  med=     4.86 us  mean=     4.92 us  iters=65536  trials=16
  libc     memset             size=16         min=     7.72 us  med=     8.11 us  mean=     8.17 us  iters=32768  trials=19
  libc     memset             size=64         min=    17.26 us  med=    17.75 us  mean=    18.01 us  iters=16384  trials=17
  libc     memset             size=256        min=    54.41 us  med=    56.31 us  mean=    57.53 us  iters=4096  trials=22
  libc     memset             size=1024       min=   204.33 us  med=   209.35 us  mean=   214.84 us  iters=1024  trials=23
  libc     memset             size=4096       min=   805.95 us  med=   821.43 us  mean=   847.61 us  iters=256  trials=23
  libc     memset             size=16384      min=     3.22 ms  med=     3.28 ms  mean=     3.38 ms  iters=64  trials=23
  libc     memset             size=65536      min=    12.92 ms  med=    13.17 ms  mean=    13.54 ms  iters=16  trials=23

--- libc/strlen ---
  libc     strlen             len=8           min=     7.70 us  med=     7.81 us  mean=     8.08 us  iters=32768  trials=19
  libc     strlen             len=64          min=    33.29 us  med=    34.77 us  mean=    35.01 us  iters=8192  trials=18
  libc     strlen             len=256         min=   121.69 us  med=   126.80 us  mean=   128.12 us  iters=2048  trials=19
  libc     strlen             len=1024        min=   480.33 us  med=   506.01 us  mean=   506.73 us  iters=512  trials=20
  libc     strlen             len=4096        min=     2.01 ms  med=     2.11 ms  mean=     2.12 ms  iters=128  trials=19

--- libc/qsort_int ---
  libc     qsort_int          n=256           min=    44.65 ms  med=    46.46 ms  mean=    46.93 ms  iters=8  trials=14
  libc     qsort_int          n=4096          min=   960.03 ms  med=   964.62 ms  mean=   964.09 ms  iters=1  trials=6
```

### System I/O (dd, not pbench)

```
# dd if=/dev/ld0 of=/dev/null bs=32k count=100
100+0 records in
100+0 records out
3276800 bytes transferred in 24.697 secs (132680 bytes/sec)

# dd if=/dev/zero of=/dev/null bs=32k count=1000
1000+0 records in
1000+0 records out
32768000 bytes transferred in 17.840 secs (1836771 bytes/sec)
```

### Reading this snapshot

Deltas vs. the 2026-05-15 baseline (min trial, lower is better):

- **Syscall path (kernel I-fetch dominated)**
  - `getpid`:        2.64 ms → 249 µs   (**~10.6× faster**)
  - `clock_gettime`: 1.13 ms → 627 µs   (~1.8× faster)
  - `pipe_pingpong`: 19.34 ms → 9.13 ms (~2.1× faster)
  - `fork_exit`:    600 ms → 426 ms     (~1.4× faster; pmap_enter
    fast path contributes alongside L2)
  - The `getpid` vs `clock_gettime` anomaly **inverted**: getpid is
    now ~2.5× faster than clock_gettime, the expected ordering for a
    near-trivial syscall vs. one that touches the timecounter.
    Previous snapshot's inversion was an L1 I-cache artifact.
- **libc bulk routines (SDRAM-streaming dominated)**
  - memcpy size=65536: 28.81 ms → 25.05 ms (~1.15×) ⇒ **382 ns/byte**
    (was 440)
  - memset size=65536: 15.18 ms → 12.92 ms (~1.17×) ⇒ **197 ns/byte**
    (was 232)
  - strlen len=4096:    2.53 ms →  2.01 ms (~1.26×) ⇒ **491 ns/byte**
    (was 618)
  - Small but real — and notably *not* a kernel I-fetch story: the
    measured loops run entirely in userland (timer-tick excursions
    into the kernel are rare and smoothed out by median/mean).  The
    L2 effect here is on the **data side**, plus possibly the libc
    text itself: 64 KiB working sets still thrash a 64 KiB 4-way L2,
    but the bus path through L2 (when it hits) is cheaper per word
    than the L1-miss→arbiter→SDRAM path, and the arbiter-pipelining
    rework shows up here regardless of cache.  Small sizes (size=1
    through size=256) drop more in relative terms (5.77→5.16 µs,
    82.40→72.52 µs), consistent with the libc text and per-call
    setup landing in L2.
- **dd throughput**
  - `/dev/ld0` (SD): 61 KB/s → 133 KB/s  (~2.17× — combined effect
    of L2 + SPI FIFO engine + pmci CMD18/25 multi-block, not L2
    alone)
  - `/dev/zero → /dev/null`: 1.42 MB/s → 1.84 MB/s (~1.3× — pure
    syscall + uiomove path, L2 + UART FIFOs)
- **New benches in this snapshot**
  - `memcpy_align`: exposes misalignment cost.  Aligned n=255 runs
    at 73.85 µs, while n=256 with any source/dest offset jumps to
    ~250 µs (**~3.5× tax for misaligned 256-byte copies**).
    Worth keeping an eye on once libc memcpy gets a hand-tuned
    inner loop.
  - `qsort_int`: 256 ints → 45 ms, 4096 ints → 960 ms.  Roughly
    log-linear scaling (256→4096 is 16×, time is ~21×) — consistent
    with comparator call overhead dominating at this CPU clock.
- **Open questions for next snapshot**
  - L2 hit/miss counters once they're wired through `SYSDEV_L2_CACHE`
    INFO/STATUS — would let us attribute the syscall win between
    "I-cache miss eliminated" and "D-cache fill faster via L2".
  - Why `clock_gettime` is still ~2.5× slower than `getpid` — likely
    the timecounter read path; worth tracing.
  - Why `memcpy size=1` is faster than the prior snapshot's 5.77 µs
    while `memcpy size=16` is also faster but not proportionally —
    suggests a per-call constant dropped (probably PLT/syscall
    overhead from L2'd libc).

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
