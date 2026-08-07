# LLVM flag reuse on Penumbra: what exists, a live miscompile, and what to add next

## TL;DR

Flag reuse is further along than remembered. There is **no standalone
flag-sink pass** — the "sinking" lives inside the compare-elimination
peephole (`PenumbraInstrInfo::optimizeCompareInstr`), which walks
backward from a redundant `CMPi Rx, 0`, finds the flag-setting ALU op
that defines `Rx`, **splices it down** to the compare site, and erases
the compare. It is wired through the generic `PeepholeOptimizer` via
`isCompare = 1` + the `analyzeCompare`/`optimizeCompareInstr` hooks and
bought ~6% Dhrystone when extended (commit `5cceecebb019`).

**That extension shipped a miscompile** (confirmed 2026-08-02 on the
current build): the backward walk skips instructions that *read* SR,
so the splice can move a carry-producing `ADD`/`SUB` past the
`ADC`/`SBC` that consumes its carry, breaking i64 arithmetic. Repro
and one-line fix below. A second, latent hazard sits in
`eliminateFrameIndex` (post-RA `ADD` clobbers SR for frames > 32 KiB).

## Inventory: flag machinery that exists today

| What | Where | Notes |
|------|-------|-------|
| Compare-elim peephole | `PenumbraInstrInfo.cpp:302-494` | `analyzeCompare` (:410) recognises `CMPi` only; `optimizeCompareInstr` (:426) elides `CMPi Rx, 0`; forward gate restricts consumers to Z/N readers (BEQ/BNE/BMI/BPL) |
| Producer splice ("the sink") | `PenumbraInstrInfo.cpp:483-490` | Mandatory: intervening SR clobbers would otherwise supply wrong flags. Producer allow-list `definesFlagsFromResult` (:385) excludes ADC/SBC (they read carry-in) |
| PeepholeOptimizer wiring | `PenumbraInstrInfo.td:269,337,343` | `isCompare = 1` on CMP/CMPi/TEST/TESTi dispatches them to the hooks; pass runs at -O1+ via `addMachineSSAOptimization` (works under pure GISel — it is selector-agnostic MIR) |
| Regression test | `test/CodeGen/Penumbra/compare-elim.ll` | adjacent, intervening-clobber, and read-in-gap-bails cases |
| Carry-chain selection | commit `792764306e22` | i64 add/sub → ADD/ADC, SUB/SBC threading carry through SR.C as adjacent pairs |
| Carry keep-alive | commit `c628cbc427d5` | carry-producing adds survive when only their carry is used |
| Branchless icmp-to-value | commits `11d2b1e5d149`, `96505b77e766` | `selectICmpToValue` materialises i1 via the carry flag; `2801fdea0be8` folds zext-of-icmp to a copy |
| Fused select pseudos | `PenumbraInstrInfo.td:556-581` | `SELECT_CC_GPR`/`SELECT_CCi_GPR` carry the CMP operands *inside* the pseudo; SR is never live across a pseudo boundary (the m68k-recommended design) |
| Compare emission routing | commits `94fa7d66ae81`, `8a23e2222632` | selectBrCond goes through `emitCompare`; constant operands folded into select pseudos |
| Localizer experiment | `PenumbraTargetMachine.cpp:196-197` | sinks constant/global materialisations to use blocks (GISel `Localizer`) — this plus the splice is probably the remembered "sink pass" |
| `G_PTR_ADD` sink combine | `PenumbraCombine.td:100-104` | unrelated to flags: frees destructive 2-op `ADDi` around memory ops |

History: `16d9f8ee0795` added the peephole (immediate-predecessor
only — safe); `5cceecebb019` extended it to walk through unrelated SR
clobbers with the mandatory splice (introduced the bug below).

## CONFIRMED MISCOMPILE: splice past an SR *reader* breaks carry chains

### Mechanism

The backward walk (`PenumbraInstrInfo.cpp:442-457`) bails only when a
gap instruction **reads or re-defines `SrcReg`** (the GPR). It walks
straight past instructions that **read SR** — `ADC`/`SBC` consuming
the producer's carry. The splice then moves the producer *below* its
own carry consumer, so the consumer observes a stale carry-in.

The commit message's safety argument ("pre-RA SSA gives splice-safety
for vreg uses for free") was correct for virtual registers and
overlooked the physreg SR: the producer's implicit SR def can have
readers inside the walked-over gap.

### Repro (current build, 2026-08-02)

```llvm
define void @sbc_gap(i64 %x, ptr %p, ptr %q) {
entry:
  %dec = sub i64 %x, 1
  %hi64 = lshr i64 %dec, 32
  %hi = trunc i64 %hi64 to i32
  store i32 %hi, ptr %p          ; uses only the hi half
  %lo = trunc i64 %dec to i32
  %cmp = icmp eq i32 %lo, 0      ; CMPi lo, 0 -> peephole target
  br i1 %cmp, label %a, label %b
a:
  store i32 1, ptr %q
  br label %b
b:
  ret void
}
```

`llc -mtriple=penumbra -O2` emits (lo half in r1, hi in r2):

```asm
	llis	r11, -1
	llis	r5, -1
	adc	r2, r5          ; consumes carry — but its producer moved BELOW
	stw	r2, [r3 + 0]
	add	r1, r11         ; spliced here to replace CMPi r1, 0
	bne	.LBB0_2
```

The `adc` reads whatever SR.C held at function entry; the hi word is
wrong whenever that differs from the carry `add r1, -1` produces
(i.e. almost always, data-dependent). With `--disable-peephole` the
order is correct (`add`, `adc`, `stw`, `cmp r1, 0`, `bne`) —
attribution is airtight.

Reachable from plain C (`uint64_t x; x--; … if ((uint32_t)x == 0)`),
in the tree since `5cceecebb019` (2026-04-25). NetBSD builds since
then are suspect until rebuilt with the fix.

### Fix

Bail when the gap contains any SR reader (the producer's flag def
must not be moved past one). In the backward-walk loop, alongside the
`readsRegister(SrcReg)` check:

```cpp
    // The splice moves the producer's implicit SR def downward; any
    // gap instruction reading SR (ADC/SBC carry-in, RDSPR) would then
    // observe different flags.
    if (It->readsRegister(Penumbra::SR, &TRI))
      return false;
```

This preserves the memcpy-loop win (pointer-bump `ADDi`s only *write*
SR) and the Dhrystone gain. Add the repro above to `compare-elim.ll`
as a negative test. Sanity: re-run
`llvm-lit test/CodeGen/Penumbra/`, `make test-compiler`, and the
compiler-correctness PIC leg.

## Latent hazard: `eliminateFrameIndex` large-offset path clobbers SR post-RA

`PenumbraRegisterInfo.cpp:117-124`: offsets outside simm16 expand to
`LLI + LUI + ADD scratch, base` immediately before the memory op —
and `ADD` sets all four flags. Post-RA, a spill/reload placed inside
a live-SR window (e.g. between `ADD lo`/`ADC hi` of a carry chain, or
between a spliced producer and its `Bcc`) with a > 32 KiB frame
offset silently corrupts the flags.

Requires big frames *and* an unluckily placed frame access, so it has
never fired in practice — but `WRSPR SR` traps by design
(`doc/system/architecture.md`), so **flags can never be
saved/restored in software**; the only legal fix is relocation:
check SR liveness (`LiveRegUnits`) at the insertion point and, when
live, emit the materialisation above the nearest earlier SR def
(base reg is SP/FP — stable anywhere in the block). Same audit
applies to any future `expandPostRAPseudo` that emits ALU ops.

## Hazard model (why flag windows are safe here — the durable part)

Every LLVM insertion point that lands inside a live-SR window, and
its safety on this ISA (flags set by ALU/MUL/DIV only; MOV, LLI/LLIS/
LUI, loads, stores, branches flag-transparent):

| Insertion | Instructions | Safe? |
|-----------|--------------|-------|
| PHI elimination copies (placed at `getFirstTerminator()` — i.e. *between* a compare and its `Bcc`) | MOV | yes — flag-transparent |
| Regalloc spill/reload, scavenger | STW/LDW | yes, **except** the > 32 KiB `ADD` path above |
| TwoAddress / copyPhysReg copies | MOV | yes |
| Calls inside a window | — | can't happen: `BL`/`JALR` have `Defs = [R13, SR]`, so SR is never live across calls |
| Branch folding / tail merge | — | respects SR implicit operands |

This table is the answer to "why did m68k need heroics and we
didn't": m68k's `MOVE` sets N/Z/V/C, so LLVM's axiomatically
side-effect-free `COPY` is a lie there and every row above becomes a
clobber (llvm-project issue #213564). Penumbra's MOV-doesn't-set-flags
choice is load-bearing; **never add a flag-setting register move or a
flag-setting load to a future ISA revision.**

## What could still be added (ranked)

1. **Fix the splice bug** (above). Correctness, one line + test.
2. **`eliminateFrameIndex` SR-liveness guard** (above). Correctness,
   small, dormant until big frames appear.
3. **Identical-compare dedup** — allow CMP/CMPi/TEST/TESTi themselves
   as producers. Flags are *identical*, so the Z/N forward gate does
   not apply: unlocks unsigned (BHI/BLS/BCS/BCC) and signed
   (BGE/BLT/BGT/BLE) consumers. Easy; reuses the existing walk.
4. **`CMP Rd, Rs` after `SUB Rd, Rs` / `TESTi` after `ANDi`** — the
   spec (`doc/system/instruction-set.md`) defines CMP/TEST as SUB/AND
   with the F-bit set, so these are also *full four-flag* equivalences,
   not Z/N-only. Caveat: the destructive 2-op form means the SUB's
   result overwrites Rd, so the CMP being elided must compare the
   *pre-sub* LHS — match the exact operand shape carefully. Requires
   extending `analyzeCompare` to CMP/TEST/TESTi (currently CMPi-only,
   `PenumbraInstrInfo.cpp:420` TODO).
5. **Per-producer flag-equivalence mask** — generalise the hardcoded
   `Z|N` forward gate into "which flags does this producer set
   identically to the eliminated compare" (SUBi-vs-CMPi-0 → Z|N;
   identical compare → all; SUB-vs-CMP same operands → all). Falls
   out of 3+4 naturally.
6. **COPY look-through** (`SUB → COPY → CMPi` shapes) — stubbed TODO.
7. **Cross-MBB elision** — Penumbra is unusually well-suited (see
   hazard table: PHI copies and spills are flag-transparent, so SR
   *can* live across a block boundary safely once 1+2 are fixed).
   No in-tree target does this; SystemZ's dedicated
   `SystemZElimCompare.cpp` is the model if the peephole hook gets
   cramped. Measure first: count `CMPi Rx, 0` at block entry whose
   single predecessor ends with a qualifying producer (a cheap stat in
   `optimizeCompareInstr`) before building anything.
8. **Doc gap** — `PenumbraInstrInfo.{h,cpp}` has no row in the backend
   CLAUDE.md file map, and the compare-elim summary was trimmed from
   it in `eead907e15fe`; the .cpp block comment (:302-340) is now the
   only prose. Add a file-map row pointing at it.

## Upstream reference points

- `llvm/lib/CodeGen/PeepholeOptimizer.cpp` `optimizeCmpInstr` (≈:946)
  — the driver; dispatches on `MCID::isCompare`.
- `TargetInstrInfo::analyzeCompare` / `optimizeCompareInstr`
  (`TargetInstrInfo.h` ≈:1836) — the hooks.
- Implementations to steal from: Lanai (smallest), ARM (canonical,
  S-suffix conversion), AArch64, X86, PowerPC, AMDGPU, Sparc.
- `llvm/lib/Target/SystemZ/SystemZElimCompare.cpp` — dedicated-pass
  model for an ISA where (like Penumbra) most instructions set flags
  unconditionally.
- llvm-project issue #213564 / PR #168485 — m68k's COPY-clobbers-CCR
  problem; the inverse design point proving the hazard table above.
