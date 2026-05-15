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

### Phase 3.5: SPI FIFO + IRQ-driven pmci  — HIGH PRIORITY

**Current pain point.**  `dd if=/dev/ld0 of=/dev/null bs=32k count=100`
reports **61 KB/s** on real hardware (ULX3S FPGA — see
`benchmark/netbsd-bench/BASELINE.md`).  That's the dominant cost of
`/etc/rc` and any disk-touching workload; single-user boot is visibly
slow because of it.  The SPI v2 controller
has a working FIFO engine in hardware, but `pmci_exec_command` in
`netbsd/sys/arch/penumbra/penumbra/pmci.c` still does polled single-byte
transfers for the 512-byte data phase, which is leaving most of the
controller's throughput on the floor.

Extend `pmci_exec_command` to use the SPI v2 FIFO-burst engine for
the 512-byte data phase, with `intr_establish_xname()` wakeups on
XFER_DONE (large-FIFO) or TX_THRESH/RX_THRESH (small-FIFO, discrete
build).  Polled baseline remains the reference implementation.

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

`pbench kernel fork_exit` reports **~600 ms per fork+exit+wait round
trip** (see `benchmark/netbsd-bench/BASELINE.md`).  That's two orders
of magnitude beyond what the workload should cost, and it makes the
system painful to use: `/etc/rc` runs many short commands sequentially,
single-user boot stalls visibly, and any shell pipeline is sluggish.
For comparison, `pipe_pingpong` (which exercises 2 context switches +
4 syscalls per round trip) reports ~19 ms — so context-switch cost
alone is ~30× cheaper than a fork.  The bulk of the 600 ms is
fork-specific work, not generic scheduler/syscall overhead.

Likely contributors, in rough order of suspicion:
- **`pmap_copy` over the parent's full page table.**  Penumbra's pmap
  copies the parent's L1 + L2 entries on fork rather than relying on
  COW from `uvm_fork()`.  For a process with even a moderate address
  space (kernel-faulted vsyscall pages, libc, ld.elf_so, stack, etc.)
  that's a lot of L2 walking, each touch through cached RAM at L1-miss
  cost.  RISC-V/ARM ports defer most of this to uvm_fault on first
  use.
- **`pmap_create`** allocates a fresh L1 page and zeros it via the L2
  window pinned-TLB slot.  Per-byte cost is fine, but if `pmap_destroy`
  on the child's exit is also synchronous and re-walks the L1/L2 to
  free everything, fork+exit pays twice.
- **Software TLB miss on first user-mode return.**  Every fault on
  the child's first instructions traps to the miss handler.  No
  ASID prefetch / pre-warming exists, so every page is faulted in
  the slow way.
- **u-area allocation** (UPAGES=4 = 16 KB) is `uvm_km_alloc(...,
  UVM_KMF_ZERO)` — synchronous zero of 16 KB through the kernel map.
  Minor on its own but adds up.

Investigation order: collect cycle counters around `cpu_lwp_fork`,
`pmap_copy`, and the first user-mode return, then attribute the
600 ms across them.  Quickest likely win: thin out `pmap_copy` to a
COW-style "share the parent's PTEs read-only and let uvm_fault clone
on demand" model, mirroring what other 32-bit GISel ports do.

## Hardware: UART RX FIFO — paste-friendliness

`hw/rtl/io/uart.sv` is NS16450-compatible: no FIFO, single-byte
RX holding register.  Copy/pasting commands into the serial console
loses characters whenever the kernel can't service the RX interrupt
between successive bytes — at 115200 baud, that's a ~86 µs window,
which is tight on a 25 MHz CPU under any moderate load.  In practice this means test runs require typing
commands by hand instead of pasting, which is annoying.

Promote to NS16550A semantics: add a small RX FIFO (16 bytes is the
standard depth and matches what NetBSD's `com(4)` already expects when
it sees the 16550A signature in IIR).  Specifically:

- Add an RX FIFO (~16 deep) and a TX FIFO (same depth or smaller —
  TX is less critical since we can poll-wait for THRE).
- Wire FCR so writes are no longer ignored: FCR[0]=FIFO enable,
  FCR[1]=RX reset, FCR[2]=TX reset, FCR[7:6]=RX trigger level
  (1/4/8/14 bytes).
- Update IIR to report the FIFO-enabled bits (IIR[7:6]=11 when FIFO
  enabled, 00 otherwise) so `com(4)`'s probe sees the upgrade.
- Add LSR/IIR semantics for "character timeout" — the 16550A asserts
  RX-available after 4-character-times of idle even below the trigger
  threshold, so partial bursts still deliver promptly.

NetBSD-side: `com(4)` autodetects 16550A and turns on FIFO mode if
IIR reports it — no kernel change should be needed beyond verifying
the autodetect runs cleanly with the new RTL.

`spi_fifo.sv` already exists for the SPI controller; the same
ring-buffer pattern should drop into `uart.sv` with minor renaming.

## Hardware: scratch SPRs for fast trap entry

**Proposal.**  Add three new scratch special-purpose registers
accessible via `WRSPR`/`RDSPR` (current SPR encoding uses `IR[15:12]`
so there's room — see `doc/system/sysregs.md`).  Name them `SCR0`,
`SCR1`, `SCR2`.  No semantics beyond "general-purpose 32-bit storage
the CPU exposes for fast trap-handler scratch use."

**Why.**  Today the trap entry path immediately stores caller
registers into the kernel scratch page (one of the pinned-TLB slots,
see `netbsd/sys/arch/penumbra/CLAUDE.md`).  Even with the pinned TLB
slot the saves are RAM accesses, which on Penumbra means CDC bridge
+ SDRAM latency on every trap.  The TLB miss fast path — the
hottest trap by far on a software-managed TLB — pays this on every
miss, ahead of any actual TLB work.

With three scratch SPRs, the entry sequence becomes:
```
  WRSPR R1, SCR0       ; save R1 to CPU-internal storage
  WRSPR R2, SCR1
  WRSPR R3, SCR2
  ; ... use R1-R3 freely for PTE walk, vector dispatch, etc.
  RDSPR R3, SCR2
  RDSPR R2, SCR1
  RDSPR R1, SCR0
  RFE
```
No memory traffic on the entry/exit prologues.  The TLB miss handler
can do its entire walk (compute VPN-indexed L2 slot, load PTE, write
TLB entry) without ever spilling.

**Cost.**  Three 32-bit registers in the SPR file (effectively three
flip-flops × 32 = 96 FFs plus mux logic), plus SPR-decode entries
for SCR0/1/2.  Trivial in both the FPGA and the eventual discrete
build.  No microcode or ISA encoding changes — `WRSPR`/`RDSPR` already
exist and decode the SPR number from `IR[15:12]`.

**Expected payoff.**  Largest impact on the TLB miss fast path, which
runs on essentially every userland page-fault and every cold page
read.  Order of impact: tens of cycles per trap eliminated.  At a
25 MHz CPU clock and typical miss rates, that compounds into
measurable end-to-end gains for memory-touching benchmarks
(`memcpy`, `strlen`) and especially for fork-heavy workloads (see
"Kernel: fork() is unreasonably slow" above, where the first
user-mode return after fork pays a flurry of misses).

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
