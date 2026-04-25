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

### Phase 3.5: SPI FIFO + IRQ-driven pmci

Extend `pmci_exec_command` in `netbsd/sys/arch/penumbra/penumbra/pmci.c`
to use the SPI v2 FIFO-burst engine for the 512-byte data phase, with
`intr_establish_xname()` wakeups on XFER_DONE (large-FIFO) or
TX_THRESH/RX_THRESH (small-FIFO, discrete build). Polled baseline
remains the reference implementation.

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

## Compiler: trailing MOVs at the back-edge of pointer-bump loops

Tight pointer-bump loops (`strcpy`, `memcpy` shapes) compile
to 8 instructions/iter; the ideal lower bound is 6.  The
remaining 2 are register-to-register MOVs at the back-edge.

(Historical note: until `isLSRCostLess` was overridden these
loops compiled to 9 instr/iter because LSR rewrote the two
pointer PHIs into a single integer IV and materialised
`base+index` per iteration.  Default `isLSRCostLess` tuple-
sorts by `NumRegs` first — a register-pressure prior that
fits 1990s x86 with 8 GPRs and free `[base+index*scale]`
folded in the addressing mode.  On Penumbra, with 12 free
GPRs and no scaled addressing, that trade is always worse.
The override mirrors PowerPC: make `Insns` the primary sort
key.  Cause 1 is now closed; Cause 2 below is what remains.)

Today's strcpy inner loop after the LSR fix:

```
.loop:                                  ; 8 instructions
  add  r3, 1        ; bump src into r3 (a new vreg, %6 in IR)
  ldb  r2, [r2 + 0] ; load *src — using r2 = OLD src
  add  r4, 1        ; bump dst into r4 (new vreg, %8 in IR)
  stb  r2, [r11 + 0]; store via OLD dst
  cmp  r2, 0
  mov  r2, r3       ; copy new src into r2 for next iter's PHI
  mov  r11, r4      ; copy new dst into r11 for next iter's PHI
  bne  .loop
```

A hand-coded version would order `ldb`/`stb` *before* the
`add`, so the destructive 2-operand `add Rd, 1` recycles the
same register the load read from — no coalescing conflict, no
trailing MOV.

Root cause: Clang lowers `*d++ = *src++` to `getelementptr`
*before* `load`/`store` in the IR, and our MI-level pipeline
doesn't reorder them.  The PHI's "old pointer" vreg and the
GEP's "new pointer" vreg can't be coalesced because they're
simultaneously live across the load/store pair.

Possible directions:

1. MI-level sinking pass that moves the GEP/ADDi past the
   load/store when there's no aliasing concern (the GEP only
   modifies the pointer; the load/store reads through the
   *previous* value of the same pointer).
2. Pre-RA hint to the coalescer that the PHI input and the
   GEP output should share a physical register, with a
   reschedule when that's only possible by reordering.
3. Earlier IR-level fix in CodeGenPrepare to emit the GEP in
   post-increment position when the pointer's only other use
   is the immediately-preceding load/store.

Option 1 is cleanest: a local transform with simple safety
conditions, matching what most RISC backends do implicitly
via MachineSink.

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
