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

## Compiler: struct-by-value varargs stack-overflow ABI

When a variadic call passes an aggregate by value and the slot
overflows R1-R4 to the stack, `PenumbraCallLowering` is
inconsistent with how the register slots were lowered.  Clang's
`DefaultABIInfo` classifies aggregates as `indirect` (caller
passes a pointer to the struct; callee dereferences).  Our
caller-side code honors this for register slots but falls back
to copying the struct bytes to the stack slot instead of a
pointer.  The callee reads every va_list slot as a pointer and
dereferences random bytes from stack as an address.

Register-slot-only tests (strct-varg-1, strct-stdarg-1, 931004-*)
pass fine because they never overflow.

Fix: in `PenumbraCallLowering::lowerCall`, mirror the register-
slot's `indirect` decision onto the stack slot — spill the struct
to a temporary and place its address at the stack position, not
the struct contents.

Tracked test: `920625-1.c` (excluded in `test/compiler/excludes.txt`).
