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

- [2026-05-25 — userland -O2 rebuild](#2026-05-25--userland--o2-rebuild)
- [2026-05-19 — L2 cache + SPI FIFO + pmap speedups](#2026-05-19--l2-cache--spi-fifo--pmap-speedups)
- [2026-05-15 — initial baseline](#2026-05-15--initial-baseline)

---

## 2026-05-25 — userland -O2 rebuild

**Captured**: 2026-05-25
**Repository state**: the commit that anchors this snapshot drops
 `DBG=-O0` from `minimal-mk.conf`, which lets NetBSD's default
 `DBG` (`-O2 -g`) take over for the userland build — so libc and
 `pbench` itself are now -O2 instead of -O0.  That single line
 removal is the headline change driving the deltas below.  The
 previous tip was `e61fb9766d21`; `git log d5d929f9d011..e61fb9766d21`
 covers the rest of the window, mostly compiler/kernel/doc work
 that should not move these particular benchmarks much:
 - `llvm: legalize G_UMULO at i64` — unblocks i64 overflow paths.
 - `netbsd: route lwp_trampoline through the vector-page trap_return`,
   `netbsd: implement kcopy fault recovery via pcb_onfault`,
   `netbsd: wire up msgbuf so dmesg(8) works` — correctness/ergonomics.
 - `netbsd: align TLB miss handler to L1 cache line`,
   `netbsd: pick TLB way from cycle counter, drop scratch memory` —
   small TLB-refill polish.
 - `hw: fix JALR microcode to capture Rd before writing R13` — bug fix
   for `JALR rd, r13`, not a perf change.
**Platform**: ULX3S FPGA, 25 MHz CPU clock, running NetBSD off SD card.
**Binary**: `pbench` (dynamically linked against libc.so), both
 rebuilt at `-O2`.
**Notes**: Two back-to-back runs captured.  Numbers below are run 1;
 run 2 was within ~1–2% on every line *except* `clock_gettime`, whose
 min wobbled 622 µs → 860 µs (the timecounter read path is still
 jittery — flagged as an open question below).  No new `dd` numbers
 captured this round; SD/zero throughput should be unchanged since
 the storage and uio paths weren't touched.

```
--- kernel/getpid ---
  kernel   getpid             -               min=   237.74 us  med=   378.67 us  mean=   328.76 us  iters=1024  trials=15

--- kernel/clock_gettime ---
  kernel   clock_gettime      -               min=   622.21 us  med=   637.24 us  mean=   649.56 us  iters=512  trials=16

--- kernel/pipe_pingpong ---
  kernel   pipe_pingpong      1B              min=     9.36 ms  med=     9.59 ms  mean=     9.89 ms  iters=32  trials=16

--- kernel/fork_exit ---
  kernel   fork_exit          -               min=   384.58 ms  med=   410.47 ms  mean=   410.64 ms  iters=1  trials=13

--- libc/memcpy ---
  libc     memcpy             size=1          min=     5.17 us  med=     5.27 us  mean=     5.38 us  iters=65536  trials=15
  libc     memcpy             size=16         min=     8.35 us  med=     9.60 us  mean=    10.17 us  iters=32768  trials=16
  libc     memcpy             size=64         min=    20.65 us  med=    24.90 us  mean=    26.62 us  iters=8192  trials=23
  libc     memcpy             size=256        min=    72.83 us  med=    74.87 us  mean=    76.23 us  iters=4096  trials=16
  libc     memcpy             size=1024       min=   277.70 us  med=   287.37 us  mean=   292.41 us  iters=1024  trials=17
  libc     memcpy             size=4096       min=     1.24 ms  med=     1.27 ms  mean=     1.28 ms  iters=256  trials=16
  libc     memcpy             size=16384      min=     4.96 ms  med=     5.02 ms  mean=     5.15 ms  iters=64  trials=16
  libc     memcpy             size=65536      min=    23.71 ms  med=    24.92 ms  mean=    25.16 ms  iters=16  trials=13

--- libc/memcpy_align ---
  libc     memcpy_align       n=256,src=1,dst=0  min=   250.54 us  med=   260.24 us  mean=   262.85 us  iters=1024  trials=19
  libc     memcpy_align       n=256,src=0,dst=1  min=   249.59 us  med=   258.78 us  mean=   263.19 us  iters=1024  trials=19
  libc     memcpy_align       n=256,src=1,dst=1  min=   249.22 us  med=   258.34 us  mean=   262.23 us  iters=1024  trials=19
  libc     memcpy_align       n=256,src=1,dst=3  min=   250.27 us  med=   260.10 us  mean=   264.14 us  iters=1024  trials=19
  libc     memcpy_align       n=7             min=     8.27 us  med=     8.40 us  mean=     8.54 us  iters=32768  trials=18
  libc     memcpy_align       n=31            min=    14.79 us  med=    14.87 us  mean=    15.15 us  iters=16384  trials=21
  libc     memcpy_align       n=127           min=    39.88 us  med=    41.08 us  mean=    41.78 us  iters=8192  trials=15
  libc     memcpy_align       n=255           min=    73.27 us  med=    75.69 us  mean=    77.18 us  iters=4096  trials=16

--- libc/memset ---
  libc     memset             size=1          min=     6.69 us  med=     6.90 us  mean=     7.03 us  iters=32768  trials=22
  libc     memset             size=16         min=     9.83 us  med=    10.00 us  mean=    10.24 us  iters=32768  trials=15
  libc     memset             size=64         min=    19.22 us  med=    19.65 us  mean=    20.04 us  iters=16384  trials=16
  libc     memset             size=256        min=    56.16 us  med=    57.75 us  mean=    59.32 us  iters=4096  trials=21
  libc     memset             size=1024       min=   205.93 us  med=   212.72 us  mean=   216.28 us  iters=1024  trials=23
  libc     memset             size=4096       min=   802.46 us  med=   833.87 us  mean=   845.78 us  iters=256  trials=23
  libc     memset             size=16384      min=     3.20 ms  med=     3.31 ms  mean=     3.37 ms  iters=64  trials=23
  libc     memset             size=65536      min=    12.83 ms  med=    13.33 ms  mean=    13.54 ms  iters=16  trials=23

--- libc/strlen ---
  libc     strlen             len=8           min=     7.65 us  med=     7.88 us  mean=     8.03 us  iters=32768  trials=19
  libc     strlen             len=64          min=    33.15 us  med=    34.37 us  mean=    34.89 us  iters=8192  trials=18
  libc     strlen             len=256         min=   121.18 us  med=   126.17 us  mean=   127.98 us  iters=2048  trials=19
  libc     strlen             len=1024        min=   481.98 us  med=   496.11 us  mean=   503.59 us  iters=512  trials=20
  libc     strlen             len=4096        min=     2.06 ms  med=     2.09 ms  mean=     2.13 ms  iters=128  trials=19

--- libc/qsort_int ---
  libc     qsort_int          n=256           min=    16.61 ms  med=    17.40 ms  mean=    17.58 ms  iters=16  trials=18
  libc     qsort_int          n=4096          min=   342.15 ms  med=   349.27 ms  mean=   355.66 ms  iters=1  trials=15
```

### Reading this snapshot

Deltas vs. the 2026-05-19 baseline (min trial, lower is better).
The dominant variable this round is **userland -O2**; kernel was
already -O2, so the syscall path barely moves.

- **Comparator-heavy code — the big -O2 win**
  - `qsort_int` n=256:   44.65 ms → **16.61 ms** (~2.69× faster)
  - `qsort_int` n=4096: 960.03 ms → **342.15 ms** (~2.81× faster)
  - This is the only line where the optimizer can really stretch its
    legs: the qsort hot loop is integer compares + small swaps + an
    indirect call to a tiny comparator.  At -O0 each of those is a
    spill/reload festival; at -O2 the comparator inlines into the
    sort and registers stay live.
- **Syscall path — essentially flat**
  - `getpid`:        249.25 µs → 237.74 µs (~1.05×)
  - `clock_gettime`: 626.96 µs → 622.21 µs (run 1; run 2 was 860 µs —
    treat as noise-dominated rather than a real delta)
  - `pipe_pingpong`:   9.13 ms →   9.36 ms (within noise)
  - `fork_exit`:     426.55 ms → 384.58 ms (~1.11×; probably the only
    real userland benefit — `execve` re-runs the dynamic linker, and
    `ld.elf_so` at -O2 is meaningfully smaller/faster)
- **libc bulk routines — mostly bandwidth-bound, small gains only**
  - `memcpy` size=65536: 25.05 ms → 23.71 ms (~1.06×) ⇒ **362 ns/byte**
    (was 382)
  - `memset` size=65536: 12.92 ms → 12.83 ms (essentially flat) ⇒
    **196 ns/byte** (was 197)
  - `strlen` len=4096:    2.01 ms →  2.06 ms (within noise)
  - These loops live at the SDRAM-streaming floor; -O2 can't move
    physics.  The small `memcpy` improvement is consistent with a
    tighter prologue, not a faster steady state.
- **The small-`memset` regression worth investigating**
  - `memset` size=1:  4.72 µs → **6.69 µs** (~42% *slower*)
  - `memset` size=16: 7.72 µs → **9.83 µs**
  - `memset` size=64: 17.26 µs → 19.22 µs
  - `memcpy` at the same sizes is unchanged, so it isn't a generic
    per-call overhead bump.  The libc `memset` codegen shape almost
    certainly changed under -O2 — likely a wider word-at-a-time
    prologue that pays back at size=256+ but loses on tiny n.  Worth
    a quick disassembly diff next time we're touching libc.
- **`memcpy_align` — alignment tax unchanged**
  - Aligned `n=255`: 73.27 µs.  Misaligned `n=256` any offset: ~250 µs
    (still the same ~3.4× misalignment penalty as last snapshot).
    Confirms the tax lives in the cache/SDRAM access pattern, not in
    code the compiler could fix.
- **Open questions for next snapshot**
  - Why `clock_gettime` min wobbles ~620 µs ↔ ~860 µs across runs —
    timecounter contention with the periodic timer interrupt is the
    leading hypothesis.
  - The small-`memset` floor regression — diff the generated libc
    `memset` between -O0 and -O2 builds.
  - Once L2 hit/miss counters are wired through, re-run to attribute
    the `fork_exit` win between dynamic-linker shrinkage and any L2
    behavior change.

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
