# Penumbra -- TODO

Items needed for improved userland testing and interactive use.

## ISS Raw TTY Mode — DONE

Implemented: `+raw` flag, Ctrl-A escape prefix (X=exit, C=CPU
state, H=help), `make simulate RAW=1`.

## Boot Arguments — DONE

Implemented: bootloader reads `boot.cfg` from FAT32 via libsa
`perform_bootcfg()`, `root=ld0f` emits `BTINFO_ROOTDEVICE`,
kernel `cpu_rootconf()` auto-selects root device.
`make sdimage-rootfs` includes `boot.cfg` automatically.

## SPI v2 Hardware — DONE

Implemented: `spi.sv` (real) and `sim_spi.sv` (sim) with hardware
TX/RX FIFO and transfer engine. 7-register interface: CAP, STATUS,
CONTROL, DATA, XFER_COUNT, IRQ_STATUS, IRQ_ENABLE.

## Kernel IRQ Dispatch — DONE

Implemented: `netbsd/sys/arch/penumbra/penumbra/intr.c` shared-IRQ
dispatcher. `com(4)` UART is IRQ-driven.

## MI sdmmc Kernel Driver — DONE (polled)

Implemented: `netbsd/sys/arch/penumbra/penumbra/pmci.c` host
controller driver. Kernel mounts FFS root from `ld0f`.

---

## Roadmap

### Phase 3.5: SPI FIFO data phase — DONE (polled completion)

`pmci_burst()` in `netbsd/sys/arch/penumbra/penumbra/pmci.c` shifts
the 512-byte SD data phase through the SPI v2 FIFO engine.  Both push
and drain loops are 8×-unrolled.  XFER_DONE is **polled**, not IRQ-
driven — see "IRQ-driven completion deferred" below.

Throughput improvement: 61 → 85 KB/s on `dd if=/dev/ld0 of=/dev/null
bs=32k count=100` (ULX3S FPGA).  Smaller than first-principles modeling
predicted because the per-sector kernel/sdmmc-layer cost (~3.7 ms) now
dominates the SD path, not the SPI byte-shifting that this change
addressed.

### Phase 3.6: CMD18/CMD25 multi-block — DONE

`pmci_read` and `pmci_write` handle `MMC_READ_BLOCK_MULTIPLE` /
`MMC_WRITE_BLOCK_MULTIPLE` natively as a per-block FIFO-burst loop
inside the single CMD18 / CMD25 envelope.  The MI sdmmc layer issues
CMD12 (STOP_TRANSMISSION) as a separate exec_command after we return
(we don't set `SMC_CAPS_AUTO_STOP`).  CMD25 uses the
0xFC inter-block data token and the 0xFD stop-tran token at the end,
with a strict busy-wait variant between blocks to avoid catching the
gap byte between data response and busy assertion.

### Phase 3.7: block-device reads still go single-block

`dd if=/dev/ld0 of=/dev/null bs=32k` issues 64 separate 512-byte
`bread()` calls per syscall (`DEV_BSIZE = 512`), so each turns into
a single-block read in `sdmmc_mem_read_block_subr`
(`c_datalen / c_blklen == 1` → `MMC_READ_BLOCK_SINGLE`).  CMD18
never engages on the block-device path.  This is why
`dd if=/dev/ld0` shows ~85 KB/s while
`dd if=/dev/rld0 of=/dev/null bs=32k` and FS-mediated reads see the
~190 KB/s CMD18 number.

Options if/when this matters:
1. Teach `ld_sdmmc_dobio` to coalesce adjacent `bread()` requests
   before forwarding to `sdmmc_mem_read_block`.  Touches the buf
   queue path; modest complexity.
2. Bump `DEV_BSIZE` for `ld(4)` so block-device reads come in
   larger chunks by default.  Affects every consumer of the block
   interface; broader blast radius.
3. Convert callers that care to use `/dev/rld0` (raw) instead.
   No code change, but documentation/convention only — easy to
   forget.

Most filesystem-mediated I/O already takes the larger-buffer FFS
path (16 KB FS blocks → 32-sector CMD18/CMD25), so the user-visible
impact is mostly for raw block-device tools (`dd if=/dev/ld0`,
`disklabel`, etc.).  Leave the block-device path single-block until
something on the read path notably matters.

### IRQ-driven completion deferred

The Phase 3.5 plan originally included `intr_establish_xname()` +
`cv_wait` on XFER_DONE.  Implemented and benchmarked: **3× slower**
than polled (85 KB/s → 28 KB/s).  Root cause is the cv_wait → IRQ →
cv_signal round-trip costing ~12 ms — more than 10× the 660 µs SPI
burst it's waiting for.  See also `pbench pipe_pingpong` (~19 ms for
2 context switches + 4 syscalls) and `pbench fork_exit` (~600 ms).

Polled busy-wait remains the right primitive for sub-ms device waits
on the current scheduler.  Revisit once one of:
- Context switch cost drops to ≪ 1 ms (would benefit fork, pipe, signals
  too — scratch SPRs landed in 02de545943e7 and shaved a few percent off
  trap-entry, but the cv_wait round-trip is still ~10 ms; the rest of
  the gap is trap_common / pmap_activate / copyin)
- CMD18 multi-block (Phase 3.6 above) — **DONE**.  Each `pmci_burst`
  now covers up to N × 660 µs of wire time on the FS-mediated path
  (16 KB FS blocks → 32-sector CMD18), so re-measuring IRQ-driven
  completion against the new envelope is worth a fresh attempt

### Phase 4: Hardware MUL/DIV

Implement hardware multiplier and divider in the ALU. Currently
trapped as illegal instructions and emulated in software
(`__mulsi3` in libc).

### Phase 5: FPU

Add a floating-point unit to the ALU. Currently using soft-float.

## Compiler: graceful-fail on unsupported inline asm and vector IR

Today the GlobalISel IRTranslator crashes (`fatal error: unable to
translate instruction: call/ret`) when it encounters:

- Inline-asm constraint classes we don't implement -- `"g"` (any
  register/memory/immediate), `"m"` (memory operand), and tied
  `"0"` constraints where the tied operands have mismatched
  widths (e.g. i32 tied to i64).
- Vector-typed IR values (`__attribute__((vector_size(N)))`).
  Penumbra is a scalar target and rightly has no vector
  legalization, but the frontend still accepts vector types from
  GCC-extension source and hands them to the translator.

Impact is low on real code -- the crashes are reproducible only
from hand-written GCC-style sources that use these features
explicitly -- but the *right* behavior is a clean frontend
diagnostic, not a backend assertion.  Tracked tests are excluded
in `test/compiler/excludes.txt` under the corresponding sections.

Two fixes, independent:

1. **Inline-asm graceful fail.**  Teach
   `PenumbraTargetLowering::getConstraintType` to reject
   unsupported constraints with a diagnostic via
   `LLVMContext::diagnose` / `report_fatal_error` with the user
   source location, rather than letting IRTranslator assert.
   Optional follow-up: implement `"g"` as "treat as `r`" so the
   optimization-barrier idiom works everywhere.

2. **Vector graceful fail.**  Add a frontend-level check (or
   LegalizerInfo with a clear "unsupported" action + diagnostic)
   so `vector_size` attributes produce a compile error naming the
   source file, not a backend crash.

## Compiler: s128 legalization for wide packed bitfields

`#pragma pack(1)` structs whose total bitfield width exceeds 64
bits (e.g. `long long a:43; b:22;` or `int a:18; b:1; c:24; d:15;
e:14;`) get lowered by clang to integer-wide load/store on a
non-power-of-two scalar.  LLVM's legalizer widens to the next
power of 2 — s128 — which our backend does not handle.

Peer 32-bit GISel ports are in the same state: ARM's GISel
legalizer has no s128 rules at all, and RV32's is gated behind
`ST.is64Bit()` with an explicit `FIXME` for libcall return
handling.  The feature isn't implemented anywhere in the 32-bit
GISel ecosystem.

Plausible implementation: add
`.narrowScalarIf(typeIs(0, s128), changeTo(0, s64))` to the
LOAD/STORE/ZEXTLOAD/SEXTLOAD/SHIFT/ADD-family/TRUNC rule groups,
so s128 chains through our existing s64→s32 narrowing.  Needs
care around G_ZEXTLOAD when the memory size equals the narrowed
scalar size — LegalizerHelper may try to split the memory access
too, which is not what we want here.

Tracked tests: `testcase-InstCombine-1.c`, `pr57344-3.c`,
`pr57344-4.c` (excluded in `test/compiler/excludes.txt`).

## Compiler: G_SCMP / G_UCMP three-way compare -- DONE

`G_SCMP`/`G_UCMP` hooked into `PenumbraLegalizerInfo` with `.lower()`,
which dispatches to `LegalizerHelper::lowerThreewayCompare()`.  The
helper emits two `G_ICMP`s plus a subtract; both ride our existing
s32/s64 rules (s64 narrows to multi-word compare via
`clampScalar(1, s32, s32)` on `G_ICMP`).  Regression test at
`llvm/llvm/test/CodeGen/Penumbra/threeway-cmp.ll`; `qsort_int` re-enabled
in `benchmark/netbsd-bench/`.

The i64 expansion is verbose (16 BBs — two s64 ICMPs each become a
three-block hi/lo/eq diamond, materialized as four `mov` selects).
A peephole that shares the hi-word compare across the two ICMPs is
a plausible follow-up but not on the critical path for qsort's int
comparator, which is the i32 case.

## Kernel: vmapbuf / vunmapbuf for raw device access

`vmapbuf` and `vunmapbuf` in `penumbra/machdep.c` are still
`TODO(stub)` — calls trap into DDB with `.long 0x6f400000` rather
than executing.  This blocks every code path that goes through
`physio()`: the raw character devices (`/dev/rld0`, `/dev/rsd0`,
etc.), and anything that opens them — `dd if=/dev/rld0`,
`disklabel`, `fsck` on an unmounted partition, `dump`/`restore`,
and similar.  Filesystem-mediated I/O is unaffected because it
goes through the buffer cache without needing user-VA → kernel-VA
mapping.

`vmapbuf` walks the calling process's pmap to find the physical
pages backing the user buffer, then maps them into kernel VA so
the driver can treat `bp->b_data` as a kernel address.
`vunmapbuf` undoes that mapping after I/O completes.  Both are
already implemented in every other 32-bit NetBSD port — see e.g.
`netbsd/sys/arch/mips/mips/vm_machdep.c:vmapbuf()` for the
canonical pattern: `uvm_km_alloc(kernel_map, len, …)` for a fresh
kernel VA range, then loop `pmap_extract(curproc's pmap, user va)`
→ `pmap_kenter_pa(kernel va, paddr, prot)` to populate it.

Surfaced as a wall when validating CMD18 multi-block reads via
`dd if=/dev/rld0`.  Block-device path (`/dev/ld0`) still works
because it goes through the buffer cache, not physio.

## Kernel: guard page for kernel stack overflow

The kernel u-area (`UPAGES = 4`, 16 KB) has no guard page, so stack
overflows corrupt whatever lives immediately below the u-area before
eventually manifesting as a confusing nested TLB miss inside
`_trap_common` (and a double-fault BREAK).  We already sized USPACE
above the worst -O0+DIAGNOSTIC frames, but a silent corruption
window still exists.

NetBSD supports per-arch opt-in guard pages via the
`__HAVE_CPU_UAREA_ROUTINES` hook.  See
`netbsd/sys/arch/x86/x86/vm_machdep.c:cpu_uarea_alloc()`: it
allocates `USPACE + PAGE_SIZE` from `kernel_map`, then
`pmap_kremove()`s the redzone page and `uvm_pagefree()`s the backing
PA so any touch bus-faults immediately.  `cpu_uarea_free()` is the
inverse.  amd64 uses UPAGES=5 (4 real + 1 redzone); i386 with
redzone enabled uses 3 (2 + 1); KASAN/KMSAN builds use more.

Implementation sketch for Penumbra:
1. Define `__HAVE_CPU_UAREA_ROUTINES` in `include/cpu.h`.
2. Add `cpu_uarea_alloc(bool system)` / `cpu_uarea_free()` to
   `penumbra/machdep.c`, mirroring the x86 pattern.  We need a
   leading guard (fault on underflow from the top of stack growing
   down) — allocate `USPACE + PAGE_SIZE`, strip the *first* page.
3. Optionally add a trailing guard too — allocate `USPACE + 2*PAGE_SIZE`
   and strip both.  amd64 does both.
4. Adjust `penumbra_lwp0_init()` in `startup.c` to use the same
   layout for lwp0's uarea so the boot stack is guarded from the
   start, not only from first `fork()`.

After this lands, kernel stack overflow produces a clean bus fault
with EPC pointing at the offending instruction, not a nested TLB
miss in the trap handler.

## Kernel: fork() is unreasonably slow

`pbench kernel fork_exit` reports **~670 ms per fork+exit+wait round
trip** on the ULX3S FPGA at 25 MHz (see
`benchmark/netbsd-bench/BASELINE.md`).  That's two orders of magnitude
beyond what the workload should cost, and it makes the system painful
to use: `/etc/rc` runs many short commands sequentially, single-user
boot stalls visibly, and any shell pipeline is sluggish.  For
comparison, `pipe_pingpong` (which exercises 2 context switches + 4
syscalls per round trip) reports ~19 ms — so context-switch cost
alone is ~30× cheaper than a fork.  The bulk of the 670 ms is
fork-specific work, not generic scheduler/syscall overhead.

### Where the time goes — full attribution

Nine MD-side phases are instrumented via the `machdep.fork_timing`
sysctl (`struct fork_timing_snapshot` in `<machine/fork_timing.h>`).
Two waves of measurement, both on the ULX3S FPGA @ 25 MHz, median
over 8 trials × 10 forks per trial:

| Phase                  | per-fork    | calls/fork | % of total |
|------------------------|-------------|-----------:|-----------:|
| `cpu_lwp_fork`         | 0.11 ms     | 1          | 0.02 %     |
| `pmap_create`          | 5.74 ms     | 1          | 0.86 %     |
| `pmap_remove_all`      | 117.4 ms    | 1          | 17.5 %     |
| `pmap_destroy`         | 4.14 ms     | 1          | 0.62 %     |
| `pmap_enter` (agg)     | 200.3 ms    | 181        | 29.9 %     |
| `pmap_protect` (agg)   | 0.86 ms     | 4          | 0.13 %     |
| `pmap_copy_page` (agg) | 28.0 ms     | 10         | 4.19 %     |
| `pmap_kenter_pa` (agg) | 0.07 ms     | 1          | 0.01 %     |
| `uvm_fault` (agg)      | 586.8 ms    | 51         | 87.6 %     |

`uvm_fault` is the *inclusive* outer bracket (the call site in
`trap.c`).  `pmap_enter` and `pmap_copy_page` are nested inside it.
Subtracting the nested leaves:

  **`uvm_fault` exclusive (MI internals) = 587 − 200 − 28 = 359 ms.**

That 359 ms is the page-fault machinery `uvm_anon_alloc`,
`uvm_pagealloc`, `amap_add`, trap-entry/exit overhead — all MI code,
unreachable from MD without forking the upstream NetBSD tree.

### What lives where

**MD-attributable share — ~54 % of total fork+wait time:**

- `pmap_enter` × 181 calls × ~28 k cyc each (29.9 %)
- `pmap_remove_all` 117 ms (17.5 %)
- `pmap_copy_page` 28 ms (4.2 %)
- `pmap_create` + `pmap_destroy` + others (~1.6 %)

**MI-bound share — ~46 % of total:**

- `uvm_fault` exclusive (359 ms): page-fault dispatch internals
- Scheduler enqueue, struct proc/lwp setup, filedesc dup, signal
  machinery, wait reaping — not separately instrumented, but
  bounded above by `total − sum(MD)` ≈ 670 − 360 = 310 ms.

### What I got wrong on the first wave

- **`pmap_protect` was hypothesised to dominate** as the "COW
  marking pass."  Measured at 0.86 ms / 4 calls / fork — NetBSD
  uses *lazy* COW, the protect-on-fork pass doesn't exist.  Dead
  end; don't optimise `pmap_protect`.
- **`pmap_kenter_pa` was hypothesised to soak uarea-allocation
  cost.**  Measured at 66 µs / 1 call / fork.  uarea pool caching
  is already doing its job; one kenter per fork is the L1 KVA
  wire from `pmap_create`, not the uarea pages (those are
  inherited from the pool warm).

### Why `pmap_enter` is so expensive per call

181 calls × ~155 µs each = 28 ms × 51 faults * 3.5 entries/fault
≈ the observed 200 ms.  Per-call 28 k cycles = 1.1 ms is a lot for
a single PTE install.  Plausible contributors, in order of suspicion:

1. **`pool_get` for the PV entry.**  Mutex acquisition + freelist
   walk + occasional pool growth path.  Could be hundreds of cycles
   even on the warm path.
2. **`pmap_pte_lookup` scratch-window reload.**  Every call does a
   WRSYS to remap pinned TLB slot 3 to the L2 page, even when the
   previous lookup was for the same L2.  At 181 calls/fork with
   most adjacent VAs sharing an L2, an L2-cache here could fold
   the WRSYS away on most calls.
3. **`pmap_tlb_invalidate`** — one WRSYS per call.
4. **`icache_invalidate`** — full-cache invalidate if the mapping is
   executable.  Many of the child's mapped pages are executable
   (libc text), so this could fire on most calls.

A second-stage instrumentation inside `pmap_enter` (bracket the four
sub-phases above) is the cleanest way to attribute the 1.1 ms.

### Why `pmap_remove_all` is suspiciously large on its own

117 ms ≈ 2.94 M cycles for an init-sized address space (~2000 valid
pages, sparse over ~10 valid L1 slots).  That works out to ~1500
cycles per visited *valid* PTE.  Same culprits as `pmap_enter`'s
per-call cost — per-L2 cached SDRAM walk, `pv_remove` SLIST
traversal — but here the scratch-window cache would help directly
since adjacent L2 walks share PA.

### Why `pmap_remove_all` is suspiciously large on its own

118.6 ms ≈ 2.97 M cycles for an init-sized address space (~2000 valid
pages typical, sparse over ~10 valid L1 slots).  That works out to
~1500 cycles per visited *valid* PTE, far more than the algorithm
describes — a load+branch on invalid entries and a PV-list walk +
stat decrement on valid ones.  Likely culprits: the per-L2 cached
SDRAM walk over 1024 entries each (256 cache lines × first-touch
SDRAM latency), and `pv_remove`'s SLIST traversal hitting cold lines.
Worth a closer look in its own right after the bigger gap is closed.

### Quickest wins, ranked by leverage

Now that attribution is complete, candidates ordered by leverage on
MD-attributable time:

1. **Attribute `pmap_enter`'s 1.1 ms/call.**  Bracket the four
   sub-phases (PV pool_get, scratch-window reload, TLB invalidate,
   icache invalidate) inside `pmap_enter` and re-run the bench.
   Without that breakdown we don't know which fix lands the biggest
   share of the 200 ms (~30 % of total).  Cheap follow-up; should be
   the next move.
2. **Scratch-window caching across `pmap_l2_map`.**  A one-entry
   cache that skips `pmap_scratch_map` when the target L2's PA
   matches the previous lookup.  Applies to `pmap_enter`,
   `pmap_remove_all`, and `pmap_extract`/`pmap_clear_modify`.
   Likely worth tens of ms.
3. **`pmap_remove_all` early-out per L2.**  Stop walking once
   `pm_stats_resident` reaches zero; most empty L2 slots beyond
   that point don't need scanning.  Probably a few-ms win.
4. **Pre-populating the child's pmap with parent's R-O pages.**
   Would reduce the 51 faults / fork to a handful.  Bigger refactor
   (shared L2 or eager L1-half copy of R-O entries) and partially
   requires touching MI assumptions about `pmap_remove_all`.
5. **`icache_invalidate` granularity.**  We do a full-cache flush
   per `pmap_enter` on executable pages — if 51 page-faults all do
   this, it's a sizeable hidden cost.  A line-granularity invalidate
   would help, but probably needs RTL changes.

Previously listed as suspects but ruled out by measurement (do not
chase further without new data):

- `pmap_copy` over parent's full page table — already a no-op.
- `pmap_create` zero/copy cost — measured at 5.74 ms, fine.
- `cpu_lwp_fork` trapframe setup — 0.11 ms, negligible.
- `pmap_protect` "COW-marking pass" — 0.86 ms / 4 calls; NetBSD's
  COW is lazy, this pass doesn't exist.
- `pmap_kenter_pa` uarea cost — 66 µs / 1 call; uarea pool warms.

### What's not fixable in MD code (~46 % of total fork+wait time)

The `uvm_fault` exclusive bracket (~359 ms / fork) is MI internals:
`uvm_anon_alloc`, `uvm_pagealloc`, `amap_add`, trap-entry/exit
overhead, scheduler enqueue, struct proc/lwp setup, filedesc dup,
signal machinery, wait reaping.  Without forking the upstream
NetBSD tree, that's the floor we can't drop below.  Even an
arbitrarily-good MD layer can only halve the 670 ms or so.


## Hardware: UART RX FIFO — paste-friendliness — DONE

`hw/rtl/io/uart.sv` now implements NS16550A semantics with 16-byte
RX and TX FIFOs (reusing `spi_fifo.sv`).  FCR wired: `[0]` FIFO
enable (true bypass to 16450 single-byte mode when cleared), `[1]`
RX reset, `[2]` TX reset, `[7:6]` RX trigger level (1/4/8/14).  IIR
reports `[7:6]=11` in FIFO mode (com(4) detect signature) and
priority-encodes RX-above-trigger / character-timeout / THRE.

Character timeout: in FIFO mode and non-empty, an RX interrupt
fires after 4 character-times of idle (640 baud16x ticks) — partial
pastes deliver promptly instead of waiting for the trigger
threshold.  Counter is gated on `baud16x_tick` so the timeout
window is fixed in baud-clock units regardless of `CLK_FREQ`.

NetBSD `com(4)` autodetects the FIFOs via the IIR signature; no
kernel-side change required.  Regression coverage in
`hw/sim/tb_uart.cpp` (37/37 passing), including an explicit
spurious-IRQ regression for the empty-FIFO timeout case.  Verified
on the ULX3S — pastes deliver reliably for bursts that fit in the
FIFO; longer bursts still lose characters (see next entry).

## Hardware: UART hardware flow control (RTS/CTS)

With the 16-byte RX FIFO landed, paste loss only happens when an
inbound burst overruns the FIFO before the kernel can drain it — at
115200 baud the FIFO holds ~1.4 ms of data, and kernel ISR latency
tails (TLB miss chains during the paste itself, copyin from a long
`tty` queue update, etc.) occasionally exceed that on a 25 MHz CPU.
The proper fix is RTS/CTS hardware flow control: the FPGA tells the
host "stop sending" before the FIFO overflows.  Verified
2026-05-15 — the ULX3S board exposes the FTDI's modem-control
lines, but asymmetrically:

| Signal      | FTDI pin | FPGA pin | Direction         | Status                             |
|-------------|----------|----------|-------------------|------------------------------------|
| `ftdi_nrts` | RTS#     | M3       | FPGA reads        | wired, unconditionally available   |
| `ftdi_ndtr` | DTR#     | N1       | FPGA reads        | wired, unconditionally available   |
| `ftdi_txden`| TXDEN    | L3       | FPGA reads        | wired (RS-485 TX-enable hint)      |
| FTDI_nCTS   | CTS#     | V4       | **FPGA drives**   | **shared with JTAG_tdo**           |
| FTDI_nRI    | RI#      | R5       | FPGA reads        | shared with JTAG_tdi               |
| FTDI_nDSR   | DSR#     | T5       | FPGA reads        | shared with JTAG_tck               |
| FTDI_nDCD   | DCD#     | U5       | FPGA reads        | shared with JTAG_tms               |

(See the commented-out `LOCATE` block at the bottom of
`hw/constraints/ulx3s_v20.lpf` lines 229-232.)

The asymmetry is the critical detail: the half of flow control that
*matters for paste* — the FPGA telling the host to slow down — needs
the FPGA to drive `FTDI_nCTS` on pin V4, which doubles as
`JTAG_tdo`.  Using that pin requires:

1. Configuring the FTDI's MPSSE/D2XX side so the modem-control
   pins are routed to the UART channel rather than the JTAG channel
   (`fujprog` and ULX3S board design assume the JTAG mode at boot).
2. Either accepting that you can't reflash over JTAG while the
   FPGA is driving CTS (probably fine — `fujprog` resets the FTDI
   into JTAG mode before each flash), or wiring the pin tristate so
   the FPGA backs off during JTAG configuration.

The cheaper, JTAG-safe alternatives that *don't* fully solve the
problem but help:

- Bump `FIFO_DEPTH` from 16 to 64 (parameter is already exposed on
  `uart.sv`).  Buys 4× more buffering time, ~5.6 ms at 115200 baud
  — wide enough to absorb most kernel latency tails.  Caveat:
  com(4) detects the FIFO size from a separate IIR signature
  (TL16C750/16C950 variants) that we don't currently advertise;
  com(4) would still believe it's 16 deep and configure trigger=14
  accordingly.  Useful as breathing room but not a real fix.
- Lower the trigger threshold in com(4) (FCR[7:6]=00 → trigger=1)
  so the ISR fires on every byte instead of waiting for 14.
  Trades IRQ rate for headroom against kernel latency.

Independently of which mitigation lands, the underlying signal is
"kernel ISR latency tails > 1.4 ms occasionally."  That's worth
investigating in its own right (most likely a TLB-miss chain
during the very paste that's filling the FIFO, since the kernel's
input-processing path touches tty buffers and current LWP state).
Hardware flow control papers over the symptom but the latency tail
is its own diagnostic.

## Hardware: uncached MMIO STW is 2.4× slower than LDW

A targeted MMIO microbench in `pmci_attach` (Phase 3.5 diagnostic,
now removed) measured per-op cost for back-to-back uncached accesses
to the SPI device on the ULX3S FPGA @ 25 MHz, with `splhigh()`
disabling interrupts and only the inner instruction varying:

```
baseline (RDSYS CPU_CYCLES x256):  1837 cyc  =  7.17 cyc/iter
STW SPI_DATA x256:                 4429 cyc  = 17.3  cyc/iter
LDW SPI_STATUS x256:               2905 cyc  = 11.3  cyc/iter

per-MMIO-op (baseline subtracted):
  STW = ~11 cyc = 405 ns/op
  LDW =  ~5 cyc = 167 ns/op
```

In the architectural model the costs should be roughly symmetric:
arbiter `IDLE→BUSY→DONE→IDLE` is ~3 cycles regardless of direction,
and the SPI device asserts `o_busy` for 1 cycle on reads (the
`access_pending` pattern) while writes don't stall at all — so writes
should be *cheaper*, not 2.4× more expensive.  Something in the
CPU's STW micro-routine, the arbiter's write-side timing, or the
cache pass-through path is adding cycles that the read path doesn't
pay.

**Why it matters.**  Every device that talks via word-strided MMIO
(UART, SPI, future timer, future Ethernet) pays this cost on every
register access.  At 11 cyc/STW we're spending ~440 ns per single-word
write — a 512-byte SPI burst push pays ~225 µs of that minimum, before
any per-byte instruction overhead.  Halving STW cost would buy a few
percent on every MMIO-heavy workload.

**Investigation order:**
1. Read the CPU's STW micro-routine in `hw/rom/microcode/` (or wherever
   the microcode source lives) and count the µ-ops.  CLAUDE.md says
   "STW (4 µ-ops)" / "LDW (3 µ-ops)" — the 1-µop difference doesn't
   explain a 6-cycle gap, but a per-µop multi-cycle execution would.
2. Add a waveform probe on `o_mem_we` and `o_d_busy` during a tight
   STW loop, count cycles between consecutive `o_mem_we` pulses, and
   compare to the same with LDW.
3. Inspect `cache_vipt.sv` write-update path (S_IDLE, `i_we_q && hit_q`
   branch) — even though it should be a no-op for uncached writes,
   verify the gating doesn't accidentally pipe-stall the bus.

Headline payoff is modest (~10% of `dd` throughput in isolation),
but it's a fundamental latency floor on every other future
microcontroller-class workload.

## Hardware: pmci_burst overshoots microbench prediction by ~3×

On top of the per-MMIO-op floor above, `pmci_burst`'s push and drain
phases each cost ~815 µs per 512-byte block — ~3× what the microbench
predicts (`512 × (cached LDB ~3 cyc + uncached STW ~11 cyc)` ≈ 280 µs).
Most plausible contributors, in rough order of suspicion:

1. **IRQ noise during the burst.**  The microbench runs at `splhigh()`,
   pmci_burst does not.  With dd actively printing diagnostic lines
   through an IRQ-driven UART, the COM IRQ fires whenever the TX
   holding register empties (~87 µs intervals at 115200 baud).  Each
   IRQ entry/`comintr`/exit costs tens of µs.  Easy to verify: add
   `splhigh()` around `pmci_burst` and re-measure.
2. **D-cache misses on `tx_buf`/`rx_buf`.**  512 B buffer, 16 B cache
   lines, 32 lines total.  First access of each line misses to SDRAM
   via the CDC bridge — order of microseconds per line.
3. **TLB churn.**  Other kernel activity between bursts may evict the
   SPI MMIO TLB entry and the per-buffer TLB entry, so each burst
   pays a few TLB misses at startup.

Worth investigating after the STW asymmetry — the absolute payoff is
roughly comparable (a couple of ms per syscall freed) and the two
fixes compound.

## Hardware: scratch SPRs for fast trap entry — DONE

Implemented SCR0–SCR3 (four 32-bit storage SPRs at indices 4–7,
supervisor-only, undefined reset values).  Hardware in
`hw/rtl/core/spr_scratch.sv`, instantiated from `datapath.sv` with
the SCR readback overlaying amux slot 11 (mutually exclusive with
vector-addr fetch).  ISS, pasm.py, and the LLVM integrated
assembler all teach SCR0–3 as SPR names.  Kernel TLB miss fast
path (`_real_miss_handler`) now uses SCR0..2 instead of
PC-relative stores into the pinned scratch page; storage symbols
`_rmh_save_r{1..3}` removed.

Commits: b33891e6d197 (ISA spec + hardware + ISS + test),
62558fd4c635 (LLVM AsmParser), 02de545943e7 (kernel adoption).

Measured impact (median-of-min across 2 consecutive runs, ULX3S
@ 25 MHz, post-pbench harness redesign):

```
  kernel/getpid          565.08 us → 545.80 us  (-3.4%) ★
  kernel/clock_gettime     1.20 ms →   1.16 ms  (-3.3%)
  kernel/pipe_pingpong    18.63 ms →  18.09 ms  (-2.9%)
  kernel/fork_exit       617.01 ms → 604.66 ms  (-2.0%)
```

The cleanest signal is on getpid: 17× the within-side noise floor
(0.02% intra-run variance vs 3.4% delta).  The other three are
individually marginal but all move in the same direction with
similar magnitude — consistent with a cache-pollution / footprint
improvement that leaks beyond the literal TLB miss handler.

Smaller than the original "tens of cycles per trap" projection
because in steady state the old PC-relative stores were cache hits
on the pinned scratch page, so the direct per-miss cycle cost was
already small.  The realized win is mostly second-order: removing
a per-miss cache-line write reduces eviction pressure on adjacent
syscall hot paths.

Future "rip out the pinned scratch page entirely" work could
extend SCR usage to `_trap_entry_*` stubs and `_trap_common`'s
early state stashing.  Diminishing returns curve is steep and
not currently a priority.

## Libc: memcpy misses same-offset misaligned shortcut

NetBSD's libc memcpy on Penumbra takes the byte-fallback path for
*all* misalignment configurations, even the cases where a word-at-
a-time body would still work.  Measured via `pbench libc memcpy_align`
at n=256 (commit 3decf13fa4b0):

```
  aligned (size=256, main sweep):    ~83 us
  src=1, dst=0:                     ~287 us
  src=0, dst=1:                     ~287 us
  src=1, dst=1 (same offset):       ~290 us  ← could be word body
  src=1, dst=3 (different offsets): ~290 us  ← byte fallback only
```

When src and dst are misaligned by the same offset, after a 3-byte
head prologue both pointers reach word boundaries simultaneously
and the middle can run word-at-a-time.  Current libc misses this
case and lumps it with the genuinely-impossible different-offset
case.

Implementation lives in
`common/lib/libc/arch/penumbra/string/memcpy.S`.  An offset-
comparison check at the top would let the same-offset case fall
into the aligned word loop.

Expected win: ~3.4× on same-offset misaligned calls (~290 us → ~85 us
matching aligned).  Different-offset path is unchanged.  Affects
roughly half of all "misaligned" memcpy calls in practice (anything
where src and dst come from the same allocation pool or share a
base alignment).

## Compiler: support `[R0 + offset]` absolute addressing for low memory

`PenumbraTargetLowering::isLegalAddressingMode` currently rejects
`AddrMode` queries with `HasBaseReg = false` — that is, it tells
LSR / CodeGenPrepare that "just an offset" is not a legal address.
Strictly speaking it *is* legal: R0 is hardwired to zero, so
`LDW Rd, [R0 + offset]` reaches the low ±32 KB of memory in a
single instruction.  The kernel uses this for the trap vectors
(`0xFFFF_0000` is reachable as a negative offset from R0 thanks to
sign extension), and bare-metal MMIO probes commonly do
`*(volatile int *)0x100 = ...`.

The codegen pipeline does not currently materialize this form.  A
`G_LOAD` whose pointer is a small constant address goes through
the constant-materialization path (LLI / LLIS) and emits a real
`MOV` of the constant into a base register first.  Selection-side
work to recognise `(load (constant fits-in-simm16))` → `LDW Rd,
[R0 + offset]` would close this gap; once it does, drop the
`HasBaseReg` reject in `isLegalAddressingMode` so LSR knows the
mode is free.

Low-priority — the only places affected today are kernel
exception-vector reads and explicit MMIO accesses with very small
absolute addresses, both of which are written in inline asm or
hand-tuned C and don't go through optimization-sensitive paths.
But the discrepancy between "what the hardware can encode" and
"what the cost model claims" is worth closing for correctness of
optimization decisions in code we haven't yet seen.

## Compiler: pointer-bump loops -- DONE

Tight pointer-bump loops (`strcpy`, `memcpy`) now compile to
the ideal 6-instruction inner loop: `ldb; add src,1; stb; add
dst,1; cmp; bne`.  Two cooperating fixes get there:

1. `PenumbraTTIImpl::isLSRCostLess` makes `Insns` the primary
   LSR sort key.  Without it, LSR rewrote the natural
   pointer-bump form into a single integer IV plus two
   `add Rb, Ri` per iteration, costing 2 extra ADDs/iter.
2. `penumbra_sink_ptr_add_past_use` post-legalizer combine
   sinks `G_PTR_ADD %new, %old, K` past the immediately-
   following memory op that reads `%old`.  Penumbra's
   destructive 2-operand `add Rd, 1` needs the source
   register to die before the def — without the sink,
   `twoaddressinstruction` inserts a COPY that the register
   coalescer can't eliminate, and the loop ends with one
   trailing MOV per pointer per iteration.

Regression test: `lsr-pointer-bump.ll` asserts the 6-instr
form.  Cycle-accurate Dhrystone (-O2): 2.16 → 2.58 DMIPS
through both fixes.

## Compiler: signed sub-word loads through PHIs

Mirror of the zext-load promote rule for the sign-extending case.

The post-legalizer rule `penumbra_zextload_promote` rewrites plain
`G_LOAD :: (load s8/s16) -> s32` into `G_ZEXTLOAD`, communicating
to known-bits machinery that `LDB`/`LDH` zero-extend in hardware.
That makes the redundant `G_AND %, 0xFF` masks emitted by the
legalizer's widening of unsigned/eq/ne `G_ICMP` dissolve via
`redundant_and`.  The signed analog is still suboptimal: for
`while (*signed_byte > 0 && *a == *b)` shapes the legalizer widens
the signed `G_ICMP` with `G_SEXT`, which lowers to `SHL r,24;
SAR r,24` after the load instead of selecting `LDBS` directly.

Upstream's `extending_loads` combine handles the single-use case
already (folding `G_SEXT (G_LOAD)` → `G_SEXTLOAD`).  The multi-use
case (loaded byte flows through a `G_PHI` to both a signed compare
and another consumer) has the same root cause as the zext case:
the direct user of the load is a `G_PHI`, not a `G_SEXT`, so the
combine bails.

Plausible fix: extend the post-legalizer combiner with a sibling
to `penumbra_zextload_promote` that walks transitively through
`G_PHI`/`G_COPY`/`G_TRUNC` users, picks the appropriate extension
opcode (`G_ZEXTLOAD` if no sign-extending user, `G_SEXTLOAD` if a
sign-extending user dominates), and rewrites the load.  The
walk has to handle mixed users sensibly — pessimize to no
promotion if both sext and zext consumers exist, since either
choice forces a software conversion at the other use site.

## Compiler: named byval args overlap on stack

When a fixed (non-variadic) function receives byval struct args
that spill past R1-R4, adjacent stack slots overlap.  Example:
`check_float(int a, _Complex float a1, ..., _Complex float a5)`
— `a4` and `a5` go to stack slots, but CC_Penumbra's
`CCAssignToStack<4,4>` reserves only the pointer size per slot,
while the framework's byval-mem path writes
`Flags.getByValSize()` bytes (8 for `_Complex float`, 16 for
`_Complex double`) at that offset.

Fix: add `CCIfByVal<CCPassByVal<4, 4>>` to
`PenumbraCallingConv.td` before the type-matched rules, so the
CC reserves `Flags.getByValSize()` bytes per byval slot instead
of a pointer-sized slot.  Mips/AMDGPU follow the same pattern.

Tracked test: `complex-7.c` (excluded in `test/compiler/excludes.txt`).
The variadic-byval stack-overflow bug (920625-1.c) was a
separate issue, fixed by `normalizeVarArgByVal()` in
`PenumbraCallLowering.cpp`.

## Benchmark: CoreMark-Pro under NetBSD

Dhrystone is the only modern-era integer benchmark in the bare-metal
harness and is widely understood as unrepresentative of real workloads
(tiny working set, no float, no state machine, trivial branching).
CoreMark-Pro is the natural successor: a 9-workload suite covering
JPEG encode, linear algebra (SP float), 64K-point FFT, SHA, DEFLATE,
neural-net inference, parser, and a 125 KB-working-set workload that
meaningfully exercises the TLB.

**Hosting decision: run it under NetBSD, not bare-metal.**
CoreMark-Pro is licensed Apache 2.0 (compatible with project licensing
— same model as our LLVM and NetBSD imports).  EEMBC ship a
Linux/POSIX reference port (`builds/linux/linux32/`) that wires
`th_malloc`, `th_file_*`, `th_thread_*`, `th_time_*` straight to libc
and pthreads.  This is the *canonical* port; the bare-metal
"embedded" port is the lesser-supported one.  Running under NetBSD
dramatically cuts porting work, and the OS-mediated TLB walking and
pmap behavior we'd measure is the path real workloads actually see.
Published industry numbers are almost universally Linux-hosted, so
our methodology lines up with the field on the comparability axis.

Cost is run-to-run noise from scheduler/interrupt/page-fault jitter,
but with single-context (`-c1`), single-user mode, and median-of-N
reporting (the protocol used by pbench in `benchmark/netbsd-bench/`),
noise floor is a few percent — well below the resolution we care
about at current CPU speeds.

**Trademark caveat.**  EEMBC owns the *"CoreMark"* mark; Apache covers
the code, not the name.  Internal scores must be labeled "unverified,
not an EEMBC-submitted score" in `BASELINE.md` and benchmark output
to stay clean of trademark misuse.

**Implementation sketch:**
1. Vendor upstream under `benchmark/coremark-pro/`, preserve LICENSE
   and NOTICE files verbatim.
2. Add `builds/penumbra/netbsd32/` as a copy of
   `builds/linux/linux32/` with cross-compiler invocation tweaked
   (`clang --target=penumbra-unknown-netbsd --sysroot=…`) and
   pthreads omitted (default `num_contexts=1`).
3. Drive EEMBC's Makefile from a new top-level
   `make benchmark-coremark-pro` target, output into
   `build/coremark-pro/`.
4. Extend `mkrootfs.sh` (or wherever the pbench bundling lives) to
   copy the 9 workload binaries plus their input data files (cjpeg,
   parser, zip) into `/usr/local/bin/coremark-pro/`.
5. Capture baseline in `benchmark/coremark-pro/BASELINE.md`, dated
   snapshot + HEAD SHA, following the pbench precedent.

**Prerequisites / risks:**
- libpthread is currently minimal stubs.  Need to verify single-
  context mode links cleanly without pulling in unimplemented
  thread primitives before committing to the build integration.
- FP-heavy workloads (`linear_alg`, `loops-all`, `nnet`, `radix2`)
  will be dominated by soft-float cost on the current CPU.  Numbers
  are still meaningful as a baseline but the suite gets a lot more
  interesting after Phase 5 (FPU).
- `radix2-big-64k` walks ~512 KB of complex floats — blows past our
  1 KB caches and 64-entry TLB and is the most interesting workload
  from a memory-hierarchy perspective.

**Related, separate item.**  CoreMark (singular, not -Pro) is a
better fit for the *bare-metal* harness — single C file, no float,
no file I/O, drops cleanly alongside dhrystone in `benchmark/`.
Plausible to land it first as the dhrystone successor in the
bare-metal tier, then add CoreMark-Pro to the hosted tier on top of
pbench.  The split gives a clean two-tier story: bare-metal
benchmarks measure the CPU in isolation, hosted benchmarks measure
the system.
