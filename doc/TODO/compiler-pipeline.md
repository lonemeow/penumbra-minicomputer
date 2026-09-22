# Compiler: passes, frames, calls and tuning

## Compiler: outgoing stack arguments address SP through a copy — RESOLVED

A call that passes arguments on the stack stores them through a copy
of SP:

```
mov  r1, r14
stw  r7, [r1 + 0]
```

where `stw r7, [r14 + 0]` needs no copy.  Call lowering forms each
stack slot's address as `G_PTR_ADD (COPY $r14), off`, and the copy is
selected into a `gpr_allocatable` register, a class without R14, so
the register coalescer cannot merge it into SP even though every use —
a store's base — accepts R14.  `PenumbraCallLowering::getStackAddress`
shares one copy across a call's stack arguments, so it costs one
instruction per call.

Reach (see "where the remaining codegen wins are"): 10,361 copies in
the `GENERIC.DEBUG` kernel, 1.45% of its instructions and about a tenth
of its copies; 1,593 in `libc.so`; 3,611 in the `test-compiler` corpus.
The sampled kernel sites are setup and diagnostic calls (`startlwp`,
`penumbra_init`, `cpu_initclocks`); 1,790 sit inside loops.

Resolved by making R14 a reserved member of `GPR_Allocatable`, as R12
already was.  The coalescer's reserved-register join then folds the
copy into its uses, and it refuses wherever an `ADJCALLSTACKDOWN`,
an `ADJCALLSTACKUP` or a call clobbering R14 falls inside the copy's
live range, so the unreserved call frame of a function with a dynamic
`alloca` stays correct (`alloca-call-frame.ll`).  In the
`test-compiler` corpus at `-O2` the shape drops from 3,611 to 17 and
the corpus from 386,627 instructions to 382,926: past the copies
themselves, the freed register sometimes spares a callee-saved save
and restore, as in Doom's `P_UseLines`.  The 17 left are `LEAfi`
expansions, a frame address taken as a value, which form after
register allocation.

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

## Compiler: GISel poison-flag hygiene is an LLVM rebase gate

The known upstream GlobalISel wrap-flag miscompile families are
audited in
[`doc/llvm/gisel-upstream-miscompile-exposure.md`](../llvm/gisel-upstream-miscompile-exposure.md):
both flag-corruption mechanisms actively deposit false `nuw` flags
in our pipeline today and are harmless only because this LLVM's
known-bits code never reads them.  A rebase (or local flag-aware
known-bits work) converts them to live miscompiles with no fallback
to hide behind.  Before any LLVM upgrade, confirm the combiner
poison-flag drop is in the tree or carry it; backport the lowerBswap
mask fix opportunistically; re-run the audit's probes after any
rebase.

## Compiler: aggregate-ABI rework — small structs in registers — RESOLVED

`doc/system/abi.md` ("Argument Passing" / "Return Values") specifies
the slot-based aggregate convention: aggregates ≤ 4 bytes travel by
value in one slot, 5–8 bytes in two slots (register/stack straddle
allowed), > 8 bytes by reference to a caller-owned copy; returns
≤ 8 bytes come back in R1/R1:R2, larger ones via hidden sret pointer
with R1–R4 undefined at return.

Implemented in clang as `PenumbraABIInfo`
(clang/lib/CodeGen/Targets/Penumbra.cpp, commit `8fb7d7dc`), replacing
the earlier `DefaultABIInfo` that made every aggregate indirect and
every aggregate return sret.  The classes follow the RISC-V ILP32
pattern: ≤ 4 bytes → Direct(i32), 5–8 → Direct([2 x i32]), > 8 →
indirect **non-byval**, with the same classes for returns
(`RetCC_Penumbra` assigns i32 to [R1, R2]).  Making the indirect class
non-byval is the keystone: clang materializes the by-value copy in IR
and the backend only ever sees a plain pointer, so the two byval-path
defects below cannot arise from clang-emitted code (`normalizeVarArgByVal`
and the framework byval handling are now dead for it).

The three defects the `test/compiler/penumbra-abi/` tests pinned down:

- **Missing byval copy for register-slot aggregates** (C pass-by-value
  silently became pass-by-reference) — gone with the non-byval
  classification; `abi-aggregate-{mutate,const,value}.c` pass.
- **Stack-positioned byval corrupts trailing arguments** — same root
  cause, gone; `abi-aggregate-boundary.c` (the stack straddle) passes.
- **Outgoing stack stores expand byte-by-byte** — independent backend
  bug: `PenumbraOutgoingValueHandler` tagged the slot with a bare
  `MachinePointerInfo::getStack()`, so `inferAlignFromPtrInfo` returned
  Align(1) and every stack argument (scalars included) lowered to the
  byte-store expansion.  Fixed in `bbd64bffed05` by deriving the
  alignment from the Align(4) SP and the 4-aligned slot offset
  (`commonAlignment`).

Coverage: execution tests `test/compiler/penumbra-abi/*.c`; shape
tests `clang/test/CodeGen/Penumbra/aggregate-abi.c` (frontend
coercion) and `test/CodeGen/Penumbra/{aggregate-args,outgoing-stack-arg-align}.ll`
(backend register placement + stack-store width).  The `struct-ret-1.c`
exclusion was re-triaged and removed — it passes at every opt level.

## Compiler: two scalar miscompiles surfaced by rebuilding compiler-rt — RESOLVED

Both symptoms (`__divdf3` quotients 1 ULP short at -O2/-O3;
`__umodXi3` wrong remainders at -O0) plus the previously separate
`pr23135.c` -O0 failure turned out to be **one bug**: the
instruction selector's carry-in fusion deleted flag-producing
ADD/SUBs whose 32-bit sum was dead but whose carry/borrow was live.
`carryInAlreadyLive` let a `G_UADDE`/`G_USUBE` select to a bare
ADC/SBC reading SR.C directly, dropping the use of the s1 carry
vreg; bottom-up selection then reached the producer `G_UADDO`,
found no remaining def uses, and erased it as trivially dead —
along with its input cone.  In compiler-rt this hit `wideMultiply`
whenever the low product half is unused (`fp_div_impl.inc`'s
`dummy`): the `plolo` multiply and the `r1` carry adds vanished,
making pre-round quotients 2 ULPs short where the error analysis
allows at most ~1.5 (the rounding step itself — the original
suspect — was correct all along, and clawed one ULP back).

Fix: `carryInAlreadyLive` rejects fusion when the producer's sum
register has no uses (the fallback materializes the carry through a
GPR, keeping the producer alive).  Regression test
`test/CodeGen/Penumbra/uaddo-dead-sum.ll` pins all three shapes
(single carry, chained carries, borrow); full investigation notes in
`doc/llvm/divdf3-rounding-bug.md`.

Found along the way, still open: the bare 64×64→128 mulhi idiom
(`wideMultiply` reimplemented as a function returning only the high
half) is recognized by the IR optimizer into an i128 multiply that
crashes the legalizer — see "Compiler: G_ZEXT s128 from the mulhi
idiom fails to legalize" below.

## Compiler: align functions and hot loops in codegen

The Penumbra backend sets no `setMinFunctionAlignment`,
`setPrefFunctionAlignment`, or `setPrefLoopAlignment` (all default to
Align(1)), and nothing in MCAsmInfo.  Compiler-generated functions are
only *implicitly* 4-byte aligned (every instruction is 4 bytes); hot
loops are never aligned to the 16-byte (4-word) L1 I-cache line.  This is
the same straddle/placement problem fixed by hand for the libc string
routines (see "Libc: cache-line-align the memcpy/memset hot loops"), but
latent across all compiled code — kernel syscall loops, the qsort
comparator, etc.  Every comparable port sets min function alignment to
the instruction width (RISC-V/Mips/ARM/AArch64/Sparc = 4, AVR = 2); the
perf-tuned ones also set loop alignment (PowerPC = 16; RISC-V/ARM/AArch64
subtarget-driven).  Penumbra is the outlier that sets neither.

(a) `setMinFunctionAlignment(Align(4))` — hygiene, not perf.  Functions
are already 4-byte aligned because all instructions are 4 bytes, so this
emits a `.p2align 2` that produces no padding and leaves the binary
unchanged.  Worth doing only to make the intent explicit and match every
peer.

(b) `setPrefLoopAlignment(Align(16))` — the real lever, the codegen-wide
analog of the libc fix.  Notes from reading the LLVM machinery
(`MachineBlockPlacement::alignBlocks`):
  - Not blanket.  Only loop backedge destinations that the block-frequency
    model rates hot (≥ ~20% of entry frequency, and ≥ ~20% of their own
    loop header) are aligned; cold loops and optsize/minsize functions are
    skipped.
  - Smarter than the hand-written `.p2align`: it fills with NOPs but only
    aligns a loop when the padding lands off the hot path — in a dead gap
    after an unconditional-branch predecessor, or on a cold fall-through
    edge (loop rotation arranges most loops so this holds).  The libc `.S`
    routines bypass this path entirely, which is why they needed the blunt
    hand-pad and why this knob does not help them.
  - `MaxBytesForAlignment` defaults to 0 = *no cap*, so by default every
    qualifying loop pads to the full 16 bytes regardless of cost.  On the
    1 KiB **direct-mapped** I-cache (64 lines) that unbounded footprint can
    lose more to capacity/conflict misses than the straddle fix saves.
    Pair the knob with an override of `getMaxPermittedBytesForAlignment()`
    returning a small bound (≈4–8) so the assembler skips alignments that
    would need too much padding — align loops already nearly aligned,
    don't pay 15 bytes to move one.

Measure on hardware, do not assume: A/B with `qsort_int` and the kernel
syscall benches (the hot-loop-bound rows).  As with the libc fix, this
only addresses the intra-loop straddle; the cross-routine set-conflict
residual needs associativity.

## Compiler: pr23135.c fails at -O0 (pre-existing, found in opt-level sweep) — RESOLVED

The 2026-06-09 full-suite sweep across opt levels (-O0/-O1/-O2 — the
routine `make test-compiler` gate only runs -O2) found exactly one
failure: `Regression/C/gcc-c-torture/execute/pr23135.c` (GCC
generic-vector arithmetic, `vector_size` attribute) exits 127 on the
ISS at **-O0 only**; -O1 and -O2 pass 1607/1607.  Bisected against the
Localizer change by rebuilding with the pass disabled — fails
identically, so it was pre-existing.  The earlier guess (vector
scalarization gap in `optnone_combines`) was wrong: it was the
dead-sum/live-carry selector bug — see "Compiler: two scalar
miscompiles surfaced by rebuilding compiler-rt — RESOLVED" above.
At -O0 nothing cleans up dead lo-half sums before selection, so the
scalarized i64 arithmetic hit the fusion hazard constantly.  Passes
with the fix.

## Compiler: codegen pass/gate audit — what we leave at the default

Audited 2026-06-09 after the shrink-wrapping discovery: Penumbra
overrides *no* behavior gates (only accessor plumbing in
`PenumbraSubtarget.h`), so several generic optimizations that mature
targets opt into are silently off.  Status per item:

- **GISel Localizer — DONE** (`4c016efc4222`).  Was missing from the
  pipeline entirely; AArch64 runs it at every opt level.  dhry_1.c
  static: stack refs 260→61, insns 816→670.  **HW-validated
  (ULX3S, 100K iterations): DMIPS 3.51→4.03 (+14.8%), CPI 4.29→3.74,
  at identical instruction count** — pure stall reduction: L1-I
  misses 77→44 per iteration (ifetch stalls 20.8%→13.6%), D-read
  misses 200K→597 total (spill reloads were nearly all of the cold
  D-reads), L2 read traffic −44%.  Post-change stall profile: ifetch
  13.6% and store 12.4% are the top terms — the store half is the
  write-through cost (L2 write-back / write-buffer hardware items),
  not compiler-addressable.
- **IRTranslator opt level — DONE.**  The translator was built at
  `CodeGenOptLevel::None`, so no branch probabilities reached block
  placement.  See "IRTranslator runs without branch probabilities".
- **Shrink-wrapping — DONE.**  `enableShrinkWrapping` defaulted to
  false.  See "shrink-wrapping (spill-density lever, part 1)".
- **Pre-RA MachineScheduler** (`enableMachineScheduler`, default
  false): with GlobalISel there is *no* scheduling at all without it —
  instruction order is IR order.  GenericScheduler in in-order mode
  (`MicroOpBufferSize=0`) prioritizes register pressure; flipping the
  hook also flips `enableJoinGlobalCopies` (cross-block copy
  coalescing), which defaults to `enableMachineScheduler()`.
  **Measured 2026-06-13, not pursued for gen1.**  Enablement is a
  one-line `enableMachineScheduler()` override (verified live: the pass
  enters the pipeline and the build is byte-identical to
  `-enable-misched=true`), but a static A/B is flat on gen1: `dhry_1`
  stays at 670 insns / 61 `[r14]` spill refs (≈40 lines merely
  reordered), and a deliberately high-pressure probe — 12 values live
  across a call — is unchanged at 56 insns / 26 spill refs.  No spill,
  instruction, or copy reduction.  Reason: on a single-issue, in-order,
  microcoded core there is no pipeline to schedule for, so the latency
  payoff is moot (`LoadLatency=1` is set precisely so the scheduler
  won't reorder ALU ahead of loads), leaving only register-pressure
  reduction — which the Localizer already captured on Dhrystone and
  which reordering cannot manufacture when spills are forced across a
  call.  The scheduler's real home is a pipelined generation; gate the
  enablement on the per-generation tuning work (the
  SchedMachineModel-per-processor mechanism in that entry below) rather
  than turning it on target-wide.  Revisit for gen1 only if a kernel
  pbench A/B surfaces reducible pressure that Dhrystone lacks.
- **Tail calls**: `PenumbraCallLowering.cpp` hardcodes
  `Info.IsTailCall = false` (`// TODO: tail calls`).  Wrapper-heavy
  kernel code pays a full frame per hop.  Measured opportunity and a
  small first-cut plan are in the dedicated "Compiler: tail-call
  optimization" entry below — ~5% of static call sites, a footprint +
  call-overhead win (not a spill win), ≈0 on Dhrystone.
- **Instruction properties — DONE.**  `guessInstructionProperties`
  was left on, and the selection patterns sit in `PenumbraGISel.td`,
  which the instruction-info backend never reads, so TableGen marked
  every instruction without an explicit `hasSideEffects` as having
  side effects: MachineSink, MachineLICM, MachineCSE and dead-code
  elimination left the whole ALU, every constant load and every
  load/store where selection put them, and each one was a scheduling
  barrier.  Every instruction now states its properties, taken from
  "Instruction Properties" in `doc/system/instruction-set.md`.
  The audit found `DIV`/`DIVU` marked side-effect-free though a zero
  divisor traps, so MachineLICM could lift a guarded divide above its
  guard; they are side-effecting now.  Follow-ups: "Compiler: flag
  definitions stay live after C++ selection", "Compiler: large frame
  offsets write the flags unchecked" and "Compiler: constants are not
  rematerializable".
- **`AllowRegisterRenaming` — DONE.**  Unset, every operand carried
  `ExtraSrcRegAllocReq`/`ExtraDefRegAllocReq` and `isRenamable()` was
  false everywhere, so MachineCopyPropagation could neither forward a
  copy's source into its readers nor rename a definition into a
  copy's destination, and PostRA machine sinking moved no copy.
  Setting it promises that no instruction needs a register its
  operand classes and tied constraints do not state.  Checked first:
  the PIC anchor sequence is a tied chain, which renaming never
  touches; the single-result `MUL`/`DIV` forms fix Rdh in the
  encoding, not as an operand; and no inline asm in the tree writes
  an undeclared input or relies on a register it does not name — the
  boot jumps that clobber R1/R2 are covered by copy propagation
  refusing to forward a register the instruction early-clobbers.
  The effect is small: the `test-compiler` corpus at `-O2` loses 46
  instructions (47 functions smaller, 3 larger where a copy sunk past
  an early exit leaves the exit its own copy), Doom's twelve hot files
  one.  The visible case is a variadic function spilling R1 without a
  copy (`varargs.ll`'s `sum_counted`).  A copy whose reader is in
  another block stays, among them the zero copies of "a constant zero
  is held in a register", which meet their reader only after block
  placement; RISC-V and AArch64 add a copy-propagation run after block
  placement (`createMachineCopyPropagationPass(true)` in their
  pre-emit passes).
- **`enableSpillageCopyElimination`** (MachineCopyPropagation
  extension, X86 enables): cheap flip + measure.
- **MachineOutliner / RISC-V-style save-restore millicode**: nothing
  implemented (`getOutliningCandidateInfo` etc.).  Code-size tools —
  attractive under the 1 KB I$ regime (21% of hot text is spill code;
  identical prologue/epilogue sequences outline well), but each
  outlined call costs BL+JMP at runtime.  Bigger project; consider
  RISC-V's `-msave-restore` shape (dedicated prologue/epilogue
  routines) rather than the general outliner first.
- **PostRA scheduler** (no Penumbra scheduling model sets
  `PostRAScheduler`): pointless for gen1 (single-cycle, no pipeline);
  belongs in a pipelined generation's own scheduling model (hide
  load-use and divmul latency).
- **MachineCombiner** (`getMachineCombinerPatterns` + sched model):
  reassociation for ILP — little to gain at IssueWidth=1; revisit
  with gen2.
- Not applicable: EarlyIfConversion (no predicated execution),
  MachinePipeliner (needs deep sched model, ILP machine),
  `enableSubRegLiveness` (no subregisters), GISel LoadStoreOpt
  (no wider load/store ops to merge into).
- **TTI capability/cost audit** (opened by the SDAG action-table
  removal): with no SelectionDAG path, IR-level passes read machine
  capabilities from `PenumbraTTIImpl`, and un-overridden defaults can
  answer wrongly in both directions.  `hasDivRemOp` — **DONE**
  (reports the fused DIV_P/DIVU_P; DivRemPairs keeps `x/y; x%y`
  pairs for the GISel divrem fusion instead of decomposing the rem
  into mul+sub — one ~33-cycle divmul op per pair instead of two).
  Remaining candidate: `getArithmeticInstrCost`, which prices
  MUL/DIV like single-cycle ALU ops for the inliner/unroller instead
  of ~33-cycle divmul iterations — a cheap override, but cost-model
  shifts move inlining decisions broadly, so it needs a kernel-bench
  A/B before landing.

## Compiler: shrink-wrapping (spill-density lever, part 1) — ENABLED

LLVM's ShrinkWrap pass (runs post-RA, just before PrologEpilogInserter)
computes a save point / restore point pair in `MachineFrameInfo` so PEI
places the *whole* prologue (CSR saves + stack adjust) at the earliest
block dominating all frame uses instead of the entry block — early-exit
paths then run zero frame code.  It is gated per-target on
`TargetFrameLowering::enableShrinkWrapping`; every major target
(AArch64, ARM, LoongArch, Mips, PowerPC, RISC-V, X86) opts in, and
`PenumbraFrameLowering` now does.

Two correctness problems had to go first.  The force-flag trials
(`-enable-shrink-wrap=true`, 1607/1607 on the compiler suite in June)
never reached either shape:

- **Epilogue placement.**  `emitEpilogue` released SP before the
  block's *last* instruction, and PEI places the callee-saved reloads
  before its *first terminator*.  In a return block those coincide.  A
  restore point ending in `bge; b` released SP on the fall-through path
  only, and one falling through with no terminator released it before
  the last reload, which then read the wrong slot.  Two kernel files
  also failed to assemble.  The release now goes before the first
  terminator, after the reloads.
- **Clobbered state.**  `canUseAsPrologue`/`canUseAsEpilogue` refuse a
  block where the SP adjustment overwrites a value still read: SR
  between a compare and its branch — every ALU op sets NZCV and the
  ISA has no flag-preserving add, so X86's `LEA` escape does not exist
  — or R11 when the frame may need the register form of `adjustSP`.
  Liveness comes from `LiveRegUnits` over the successors' live-in
  lists.  SR is block-local by construction today; code that ever
  splits a block while flags are live must add the SR live-in itself,
  as X86's select lowering does for EFLAGS.  A refused restore point
  that is the only candidate leaves the frame in the entry block.

Shape coverage: `shrink-wrap.ll`.

Measured statically on the IR corpus of the hot-path review
(2026-09-13), the same `llc` with and without
`-enable-shrink-wrap=false`:

| | framed functions | shrink-wrapped | `.text` |
|---|---:|---:|---:|
| Doom | 928 | 67 (7.2%) | −12 B |
| kernel `GENERIC.DEBUG` | 5,881 | 230 (3.9%) | +188 B |
| SQLite (PIC) | 1,642 | 101 (6.2%) | +184 B |

The hooks refuse little: the pre-fix compiler with the force flag
shrink-wraps two more Doom functions and the same number elsewhere.
SQLite's `getCellInfo` (cached path 14 instructions → 4) is among the
shrink-wrapped.  Dhrystone's hot functions have no early-exit shape;
kernel error paths and lock fast paths are where it can show.

Open:

1. **Reach limit — regalloc pins the save point for argument values.**
   An argument live across calls gets its `COPY` into a callee-saved
   register placed in the *entry* block, which ShrinkWrap counts as a
   frame use, so such functions never shrink-wrap.  The standard
   counter is `TargetRegisterInfo::getCSRFirstUseCost` (AArch64 returns
   5), which makes greedy RA pre-split cold CSR first-uses — but
   forcing `-regalloc-csr-first-time-cost` did *not* split the argument
   shape on Penumbra (the region-split candidate search found nothing
   under CSRCost).  Needs its own investigation; it is the likely cause
   of the low kernel count above.
2. **`R_PointOnSide`** still saves 4 registers on its axis-aligned
   early exits, which riscv32 shrink-wraps with 2 — 52 cycles per exit
   on penumbra1, 26 on penumbra2_5.  Not root-caused; the six returns
   sharing one return block, or `r5` used as scratch before the first
   early exit, are the suspects.  Doom's BSP leaf visits also need the
   tail-call entry.
3. **Known regression, accepted — DDB backtraces.**  The kernel
   unwinder (`db_trace.c`) finds a frame by scanning the first 16
   instructions of the function for `SUB r14,#imm` and the `STW r13`
   save, stopping at the first instruction outside that pattern.  A
   shrink-wrapped function starts with its early-exit test, so the
   scan finds no frame and treats the function as a leaf: the walk
   stops at the first shrink-wrapped caller, and a trapping
   shrink-wrapped frame past its prologue unwinds with a stale LR and
   the wrong SP.

   Other ports: x86 and AArch64 build the kernel with frame pointers
   and walk the chain, which shrink-wrapping cannot break above the
   trapping frame.  MIPS and SH-3 scan forward from the symbol like
   Penumbra and break the same way; MIPS stops at the first branch and
   says so ("return breaks stackframe-size heuristics with gcc -O2").
   Alpha's `decode_prologue` is the one written for moved code: it
   scans every instruction from the symbol to the PC, adds up the stack
   allocations wherever they sit and ignores stack releases, since an
   epilogue can be laid out before a PC still inside the frame.

   The fix, when backtraces matter enough: scan Alpha-style.  Caller
   frames are then always right — a return address lies inside the
   saved region, because a call needs LR saved.  The trapping frame
   stays a guess (the trap may sit before the saves or past the
   restores); check that the LR slot holds a kernel text address
   following a `BL`/`JALR`, and otherwise fall back to the live LR and
   the unadjusted SP.  A frame pointer in R10 would cost a save and an
   allocatable register in every framed function, the cost
   shrink-wrapping exists to remove.

## Compiler: tail-call optimization — measured opportunity

`PenumbraCallLowering::lowerCall` hardcodes `Info.IsTailCall = false`
(`// TODO: tail calls`), so every call — including a pure forwarding
wrapper `return bar(args)` — emits a full `BL`/`JALR`, a frame, and a
`jmp lr` return hop.  Every major GISel target lowers tail calls;
Penumbra is missing the wiring, not the capability.

### Static opportunity (measured 2026-06-22)

Method: static scan of `llvm-objdump -d` over the real built binaries.
A *tail-position call* is a `bl`/`jalr` whose only successors before the
function's terminal `jmp lr` are frame teardown — a `ldw` of a
callee-saved reg or `lr` from `[sp+N]`, plus the `sp` adjust — so the
result in r1/r2 passes through untouched.  Restricting teardown-loads to
callee-saved targets (excluding a `ldw r1,[sp]`, which would mean a
reloaded local rather than the call result) moved every count by <2%, so
the heuristic is tight.

| binary | total calls | tail-position | % | free-frame wrappers |
|--------|------------:|--------------:|--:|--------------------:|
| kernel (MINIMAL) | 37,267 | 2,027 (1,869 `bl` + 158 `jalr`) | 5.4% | 531 (28% of direct) |
| libc.so.12       | 20,113 |   937 (875 + 62)                | 4.6% | 476 (54% of direct) |
| bin/sh           |  2,986 |   105                           | 3.5% |  23 |
| bin/cat          |     62 |     2                           |   —  |   2 |

So ~4–5% of all call sites already compile to a tail-position call.  The
"free-frame wrapper" column is the subset whose stack frame exists
*solely* to save `lr` across the call; libc's forwarders (`__foo`→`foo`
weak aliases) make it over half of libc's tail sites.

### Per-site savings — two tiers

- **Plain tail call**: −1 static instruction (the trailing `jmp lr`
  disappears; `bl`→`b`, `jalr`→`jmp`), and −1 dynamic taken
  control-transfer — the callee returns straight to our caller instead
  of bouncing back through our frame.
- **Free-frame wrapper**: the whole prologue/epilogue collapses to a
  single `b target`, removing a **write-through `stw lr`** + its reload
  *per dynamic execution*.  Given gen1 store-stalls are ~12% of cycles
  (see the "Dhrystone hot-path code shape" entry in workloads.md),
  that is real on a hot wrapper, not cosmetic.

### Expected leverage — footprint + call-overhead, not spill

Workload-shaped, unlike the broad spill win the Localizer captured:

- **Dhrystone / compute kernels: ≈0.**  Their hot loops carry no tail
  calls — the same structural reason shrink-wrapping measured flat.  Do
  not expect DMIPS to move.
- **Wrapper-heavy paths: modest but real.**  Kernel syscall dispatch,
  VFS/VOP indirect dispatch (the 158 `jalr`-tail sites are `VOP_*`-
  shaped), libc forwarders, and process startup (ties into the "fork()
  is unreasonably slow" entry).  Estimate: low single-digit % on
  call-overhead-bound benches (pbench syscall rows), not a
  Localizer-sized number.
- **Recursive tree walks.**  Doom's `R_RenderBSPNode` pays its full
  7-register frame on every leaf visit, which only masks the child
  index and calls `R_Subsector`.  With tail calls and shrink-wrapping
  a leaf is `test; bne; mask; b R_Subsector` — about 90 cycles less
  per subsector on penumbra1, 50 on penumbra2_5.  SQLite's
  `sqlite3BtreeNext` keeps a frame only to save `lr` around a
  tail-position `bl`.
- **Code size: ~−2K kernel instructions at the −1/site floor,
  ~−15–18 KB once free-wrapper frame removal counts** — helps the 1 KB
  direct-mapped gen1 I$ only where the wrapper text is hot.

A hard percentage needs a **dynamic** call-site histogram: run a
wrapper-heavy workload (kernel pbench syscalls / fork+exec) under the ISS
with call tracing and weight these static sites by execution frequency.
Dhrystone provably cannot resolve this — measure on the syscall/startup
benches.

### Implementation — a small first cut exists

The audit's "medium GISel project" framing is the *fully general*
version; a useful first cut is small:

- **First cut**: tail-call only when there are **no outgoing stack args**
  (≤4 register-arg-words) and the calling conventions match.  Reuse the
  existing `PenumbraOutgoingValueHandler`; emit a `TAILBL`/`TAILJMP`
  terminator pseudo instead of `BL`/`JALR`; let PEI sink the epilogue
  before it.  Penumbra's caller-pop ABI means no callee-cleanup
  mismatch — a real simplifier.  The tail branch stays in the same
  privilege level (SSP/USP banking untouched), R12/curlwp is reserved,
  and `lr` is restored before the branch so the callee returns to our
  caller.  This already catches most free wrappers, which are typically
  ≤4 args.
- **The medium tail**: outgoing stack args overlapping the incoming arg
  area (ordering-sensitive copy), `sret`, varargs, and the
  eligibility-predicate corners.  A minority of the opportunity.

RISC-V's `lowerTailCall` + `PseudoTAIL`/indirect-tail pseudo is the
matching template; AArch64's `isEligibleForTailCallOptimization` is the
eligibility-check reference.

## Compiler: IRTranslator runs without branch probabilities — RESOLVED

`PenumbraPassConfig::addIRTranslator` built the translator at its
default `CodeGenOptLevel::None`, so it never requested
`BranchProbabilityInfo`.  Every conditional edge was recorded 50/50,
and block placement ignored `__predict_false`, `__builtin_expect`,
cold calls and loop-exit heuristics; the same probabilities order
switch clusters.  It now passes `getOptLevel()`, as RISC-V and AArch64
do.  Shape coverage: `branch-probability-layout.ll`.

Measured statically on the same IR (kernel `GENERIC.DEBUG`, Doom,
PIC `libsqlite3`):

| | kernel | Doom | SQLite |
|---|---:|---:|---:|
| `.text` | +1.5% | +0.19% | −0.77% |

| `kern_assert` call reached by | before | after |
|---|---:|---:|
| fall-through | 6,730 | 732 |
| taken branch | 139 | 6,133 |

`kern_assert` returns, so each block moved out of line pays a `b` back
— most of the kernel's growth.  Hot functions stay within a few
instructions of their old size.  On hardware the pbench kernel rows
all improved except `clock_gettime`, which retired slightly more
instructions per call although nothing on its path grew — see "pack
the hottest syscall-path text onto shared pages".

Turning it on exposed a latent PIC bug: `lowerAnchoredOperand` read an
offset from jump-table-index operands, which have none, and emitted
whatever the operand storage held.  It had always held zero before —
all 1,895 PIC jump-table bases in the installed shared libraries check
out.

## Compiler: no branch-cost model — branch-avoidance may be over-eager

Penumbra sets none of the branch/select cost knobs
(`convertSelectOfConstantsToMath`, `predictableSelectIsExpensive`, TTI
`getCFInstrCost`, …), and GlobalISel would not ask the one that matters.
Upstream's `match_selects` (in `select_combines`, part of
`all_combines`) rewrites `select(cmp, C1, C2)` into arithmetic on the
compare result — `tryFoldSelectOfConstants` covers `C` against `C ± 1`,
a power of two or all-ones against zero, and all-ones against anything
— with no cost check.  SelectionDAG gates the same fold on
`TLI.convertSelectOfConstantsToMath(VT)`, which defaults to false and
which RISC-V, X86 and LoongArch opt into; GlobalISel never consults it.
That is a GlobalISel-vs-SelectionDAG gap worth a `doc/llvm-*.md`
write-up.  Penumbra's own branchless icmp-to-value path through the
carry flag is a separate, clear win over the old `SELECT_CC` form and is
not in question.

What the fold and the branch diamond produce, identical under every
tuning:

| Source | Emitted |
|---|---|
| `x == 2 ? 3 : 4` | `sub; cmp; mov; adc; shl; sar; add` — 7, branchless |
| `x < 5 ? 8 : 0` | `mov; lli 1; lli 0; cmp; blt; mov` to build the 0/1, then `shl 3` — 7, a branch and the arithmetic |
| `x == 7 ? 100 : 37` (not folded) | `mov; lli; lli; cmp; beq; mov` — 6 |

Beyond the fold's cost-blindness:

- **Signed conditions pay both costs.**  A signed compare cannot come
  out of the carry flag, so selection builds its 0/1 with a branch
  diamond and the fold's shift or sign extension runs on top.  That is
  a loss on every core, whatever the tuning.
- **The branch form is longer than it needs to be.**  The select
  diamond loads both constants before the compare and then copies
  conditionally.  `LLI` and `MOV` leave the flags alone, so
  `cmp; lli C1; beq; lli C2` is 4 instructions.

Against that 4-instruction form the measured costs
([Branch costs](#branch-costs)) favour the branch on both cores for
`x == 2 ? 3 : 4`:

| Core | Branch form | Branchless (7 instructions) |
|---|---|---|
| penumbra1 | 6–8 cycles, deterministic | 14 cycles |
| penumbra2_5 | 4 cycles, +4 when mispredicted | 7 cycles |

On penumbra2_5 the branch loses only above a 75% misprediction rate, and
the BTB predicts each site's last outcome, so even random data sits near
50%.  The branch form is also the shorter code, so the I-cache effects
the cycle figures leave out point the same way, and the decision does
not wait on a hardware A/B.

In order:

1. Make `tryFoldSelectOfConstants` consult
   `convertSelectOfConstantsToMath`, and write the upstream doc.
   Penumbra's override weighs the tuned core's costs the way
   `isIntDivCheap()` does: penumbra1 decides on instruction counts
   alone, since a `Bcc` costs as much as an ALU operation; penumbra2_5
   adds the misprediction penalty at one named, assumed misprediction
   rate.  That needs branch costs in the scheduling models, which
   [instruction-timing.md](../internals/instruction-timing.md) already
   records.  With today's costs the override answers false on both
   cores, which also removes the branch-plus-arithmetic shapes; the cost
   check pays once a core (gen3) moves the numbers.
2. Select a select of two constants as `cmp; lli; bcc; lli`, so the
   check compares against the branch form actually emitted.
3. Separately, InstCombine already delivers `sext(icmp)` for
   `x > y ? -1 : 0`, outside the fold's reach; selection builds a 0/1
   diamond and then `shl 31; sar 31`, where loading -1 directly saves
   two instructions.

Dhrystone's `Proc_6`, `*ref = (x==2) ? Ident_3 : Ident_4`, is the
`C`-against-`C + 1` shape.  Reach check 2026-06-09: that chain is the
*only* such site in Dhrystone's hot path (see the Dhrystone hot-path
entry in workloads.md), so Dhrystone cannot show the difference either way;
confirm on kernel hot paths.

## Compiler: inline small constant-size memcpy/memset/memmove

`G_MEMCPY`/`G_MEMSET`/`G_MEMMOVE` are `.libcall()`'d unconditionally in
`PenumbraLegalizerInfo.cpp`, so even `memcpy(d, s, 16)` and
`memset(d, 0, 12)` emit a `bl memcpy`/`bl memset` rather than a few
inline word stores. Other GISel targets inline the small constant-size
cases; Penumbra is missing the wiring, not the capability.

The expansion itself is generic: `CombinerHelper::tryCombineMemCpyFamily`
(and `tryEmitMemcpyInline` for `llvm.memcpy.inline`) turns a constant-size
op into a load/store sequence bounded by `TargetLowering::MaxStoresPerMem*`
(base defaults 8, or 4 at `-Os`), shaped by `findOptimalMemOpLowering` ->
`getOptimalMemOpType` + `allowsMisalignedMemoryAccesses`. It is **not** a
`Combine.td` rule, so sitting in `all_combines` does not enable it — each
target hand-wires a dispatch in its pre-legalizer combiner's
`tryCombineAll` override (see `AArch64PreLegalizerCombiner.cpp` and
`MipsPreLegalizerCombiner.cpp`). The legalizer `.libcall()` then remains
the fallback for the variable-size / over-threshold cases. AArch64 also
forces an inline length of 32 bytes at `-O0` and lowers `memset(...,0,...)`
to bzero.

Penumbra currently uses the plain generated `tryCombineAll` (no
`CombineAllMethodName` override, no mem-opcode dispatch) and overrides none
of the mem-op TLI hooks, so `tryCombineMemCpyFamily` is never reached and
the default threshold of 8 sits unused.

To enable it:
- Switch `PenumbraPreLegalizerCombiner` to the `CombineAllMethodName =
  "tryCombineAllImpl"` pattern and hand-write `tryCombineAll` to dispatch
  `G_MEMCPY`/`G_MEMSET`/`G_MEMMOVE`/`G_MEMCPY_INLINE` to the helper before
  falling through to `tryCombineAllImpl`.
- Override `getOptimalMemOpType` to return `s32` — the base default returns
  an invalid `LLT`, so `getMemOps` won't pick word stores otherwise.
- Keep the default `MaxStoresPerMem*` (8 / 4) unless profiling says
  otherwise.

Caveat: Penumbra lowers unaligned loads/stores to byte ops, so an
under-aligned copy would expand into more than the threshold's worth of
byte stores and bounce back to the libcall. The realistic win is therefore
word-aligned constant-size struct/array copies — which is the common case.
Decide via `allowsMisalignedMemoryAccesses` whether to inline unaligned at
all (probably not).

## Compiler: per-generation tuning — models and their consumers

Every Penumbra generation implements the same ISA and accepts the same
binaries, but their micro-architectures pull optimization tradeoffs in
opposite directions.  The backend needs to know which one it is
building for and skew cost-driven choices accordingly, without ever
changing what code is *legal*, so one object file still runs anywhere.

### The invariant that constrains the whole design

Generation knobs are **tuning only**.  They may change *which of
several correct lowerings* the backend picks (branch vs. branchless,
inline vs. libcall, schedule, unroll factor) but must never change
instruction legality or the emitted instruction set.  Legalization
stays identical for every generation; the knobs are consumed only in
cost models, combiner gating, and scheduling.  Concretely: never
branch on a subtarget feature inside `PenumbraLegalizerInfo`'s
legality rules.

This is why the selection is `-mtune`-shaped rather than `-mcpu`-shaped.
A feature belongs in the *tune* list of a `ProcessorModel` (the fourth
argument), never the architecture list: `MCSubtargetInfo` takes
architecture features from the `-mcpu` name and tuning features from
the `-mtune` name, and reads the scheduling model from the tune name
alone.  Putting a Penumbra property in the architecture list would
claim it changes what a binary may contain.

### Infrastructure — landed

`Penumbra.td` defines `penumbra1`, `penumbra2` and `penumbra2_5` as
processors over one shared architecture.  Each carries a
`PenumbraTuning` record pairing its tune features with its scheduling
model — `Penumbra1Model`, `Penumbra2Model` and `Penumbra2_5Model` —
and all feature lists are empty.  `generic` is
not a tuning of its own: it is defined from the
**most-used variant's** `PenumbraTuning`, so it tunes exactly like
that core rather than averaging across generations, and moving the
default is a one-reference change in `Penumbra.td`.
`PenumbraSubtarget` holds the generated tune-feature fields and
defines what an unspecified name resolves to (`resolveCPU` /
`resolveTuneCPU`), which is `generic`.
`PenumbraTargetMachine::getSubtargetImpl(const Function &)`
builds a subtarget per distinct (cpu, tune, features) combination.

That last piece is a prerequisite, not a refinement: `-mtune` reaches
the backend *only* as a per-function `"tune-cpu"` attribute, so a
single module-wide subtarget could not observe it at all.  `llc` has
no `-mtune` switch, which is why `test/CodeGen/Penumbra/mtune.ll`
drives tuning through IR attributes rather than a command line.

Clang accepts both flags for the Penumbra triple (`getCPUName` in
`CommonArgs.cpp`, `AddPenumbraTargetArgs` for `-mtune`) and diagnoses
an unknown name against the list in `clang/lib/Basic/Targets/
Penumbra.cpp`, which mirrors the `ProcessorModel` definitions.

The models differ in the MUL and DIV latencies they state and in what
an ALU read waits for, so a constant divide and a wide constant are
the places the names produce different code so far.  A core earns a name when builds
target it explicitly or when `generic` follows it; its tuning then
fills in as properties with consumers land.

### The largest divergence: dependence distance

The property that most separates the generations is how far a
consumer must sit from its producer, and it splits them three ways
rather than two:

| Shape | Machines | Scheduling consequence |
|---|---|---|
| No overlap | gen1 (microcoded, single-issue) | Separating a use from its producer buys nothing and costs live range |
| Overlap, full interlock | gen2 (no GPR forwarding, no write-through) | Every producer/consumer pair wants distance |
| Overlap, forwarded | gen2.5 | Only loads want distance, and only one slot |

The gen2 figures are in
[penumbra2/overview.md](../internals/penumbra2/overview.md): load-use
stall 4 vs. 1 and CPI on tight cache-hit loops ~2.5-3.0 vs. ~1.2-1.5
(branch costs are under [Branch costs](#branch-costs)).  That CPI gap
is this property.

Expressing it is **not** a feature bit.  Dependence distance is
operand latency plus `ReadAdvance` bypass entries in a
`SchedMachineModel`, which the MachineScheduler consumes; feature bits
gate discrete choices.  A model attaches to a processor, so a
generation whose dependence behaviour differs needs its own processor
entry — that is the argument for splitting a name, and the only one
that requires it.

The models are `CompleteModel = 0`, state `WriteRes` latencies for ALU
operations, MUL and DIV, and carry no `ReadAdvance` entries.  Nothing
schedules against them yet: MachineScheduler runs only for a subtarget
that returns true from `enableMachineScheduler()`, which defaults to
false and Penumbra does not override, so the model's `IssueWidth`,
`MicroOpBufferSize` and `LoadLatency` have never influenced code
generation.  What does read the model today is Penumbra's own cost
code (`PenumbraSubtarget::getALUCycles` and friends, feeding
`getArithmeticInstrCost`, `isIntDivCheap()` and the constant-multiply
combine); upstream, `SelectOptimize` reads `MispredictPenalty` (see
[Branch costs](#branch-costs)).  The other consumers of scheduling data
— MachineCombiner, EarlyIfConversion, MachineLICM's high-latency
hoisting — need target patterns, a target that adds the pass, or a
hook override, and none apply.  Writing a real dependence model
therefore means enabling MachineScheduler as well, annotating every
instruction's reads, and measuring what the pass costs in compile time
and register pressure.  `generic` takes the most-used variant's side
once a second model exists, so a generic build schedules for a
forwarding core; an interlocked or single-issue core that wants its own
schedule is built with its explicit name.

**gen2's dependency cost is modelled for ALU reads only.**
`Penumbra2Model` states the wait as `ReadAdvance<ReadALU, -3>` on
every ALU instruction's register read, and the price of a wide constant
reads it.  Loads, stores and branches carry no read annotation, and
the other cost decisions — the constant-multiply combine's shift/add
chains among them — still price gen2 as if it forwarded.  On gen2 a consumer waits for
its producer to leave WB: 3 stall cycles at distance 1, fewer further
back ([hazard-model.md](../internals/penumbra2/hazard-model.md), Example
A; a chain of eight dependent `ADD`s measures 29 cycles against 8 on
gen2.5).  A shift/add chain is the worst case, since every op reads
the one before it.

LLVM can describe this.  A `ReadAdvance<Read, N, [producers]>` says
when a consumer's operand becomes usable relative to its producer; a
negative `N` is a consumer that must wait (TargetSchedule.td names
this use), and the producer list scopes it — e.g. to loads.
MachineScheduler builds a dependency DAG whose edges carry
`computeOperandLatency` (producer latency minus the consumer's
`ReadAdvance`) and places instructions on a cycle timeline under
`IssueWidth` and in-order issue.  On that timeline a constant `-3` *is*
distance-dependent: a consumer is not ready until three cycles after
its producer, so independent instructions scheduled between them absorb
the stall, which is exactly gen2's behaviour.  Only per-instruction
queries that see one edge at a time — MachineTraceMetrics' critical-path
depth, which also assumes an out-of-order core by taking the longest
path rather than summing — cannot see distance.
`TargetSubtargetInfo::adjustSchedDependency` allows per-edge tweaks if
the constant proves too coarse.  So a gen2 model is
`ReadAdvance<ReadGPR, -3>` on every GPR read plus MachineScheduler
enabled for that tuning; gen2.5 would add only
`ReadAdvance<ReadGPR, -1, [WriteLoad]>` for its load-use interlock,
which is little to gain for the cost of enabling the pass.

### Branch costs

Measured on the RTL testbenches
([instruction-timing.md](../internals/instruction-timing.md)):

| Control transfer | gen1 | gen2.5 |
|---|---:|---:|
| `Bcc` not taken | 2 | 1 |
| `Bcc` taken / `B`, BTB-predicted on gen2.5 | 2 | 2 |
| mispredicted, or indirect `JMP` / return | 2 | 5 |
| `BL` | 3 | — |

gen1 has no prediction: a control transfer is one µop like an ALU
operation.  On gen2.5 the fetch-time BTB (32-entry direct-mapped,
trained by EX: taken allocates, not-taken clears) predicts each direct
branch site's last resolved direction; a correct taken prediction
discards the sequential fetch launched in IF1 as the branch enters
IF2, and a misprediction, indirect jump or return discards the four
slots behind the branch in IF1, IF2, the fetch buffer and ID
([instruction-timing.md](../internals/instruction-timing.md#control-transfers-on-penumbra25)).
gen2.5's `MispredictPenalty` is therefore **4**.  gen2 has no BTB, so
every taken branch there costs the redirect; branch behaviour is a
second reason gen2 and gen2.5 cannot share a model.

**A correct prediction still costs a slot.**  The BTB was designed to
make a correctly predicted taken branch free, but it steers IF1 as the
branch enters IF2, by which time IF1 has launched the sequential fetch.
Reaching zero would take launching the target fetch in the same cycle
as the branch — worth weighing for gen3's front end, where every taken
branch in a hot loop pays it.

No scheduling-model field expresses "a predicted taken branch still
costs a cycle", and setting `MispredictPenalty` would change almost
nothing for Penumbra: `SelectOptimize` uses it to choose between a
`select` and a branch, but Penumbra has no conditional-move instruction,
so a `select` becomes a branch diamond either way; SimplifyCFG adds
`TTI::getBranchMispredictPenalty()` (default 0, overridden only by X86)
to its speculation budget, and only for branches marked unpredictable.
The decisions these costs should drive are the branch-versus-branchless
combines — see the "no branch-cost model" entry.

### Other consumers waiting on a feature

- **Multiply cost — landed, as model latencies rather than a
  feature.**  A cycle count is a number, which a feature bit cannot
  carry, and the compiler should know only how long an operation
  holds the core, not how the hardware gets there.  Each scheduling
  model states MUL and DIV latencies; `PenumbraSubtarget::getMulCycles`
  / `getDivCycles` read them, and `getArithmeticInstrCost` and
  `isIntDivCheap()` consume them.  See
  [the constant strength-reduction entry](compiler-arith.md#compiler-constant-multiply-divide-and-remainder-strength-reduction).
- **Branch-vs-branchless** — the "no branch-cost model" entry above.
  Gate the select-of-constants fold on
  `convertSelectOfConstantsToMath`, answered from the branch costs of
  the tuned generation; `getCFInstrCost` / `getCmpSelInstrCost` can
  read the same costs.
- **Code density** — the "inline small constant-size memcpy" entry.
  Gate the inline-expansion thresholds (`MaxStoresPerMem*`) on I-cache
  pressure, so a small-cache generation keeps the compact `bl memcpy`
  while a larger-cache one inlines.  The same property could later
  skew unrolling and the 16-bit jump-table choice.

### Validation

Lock each knob with lit tests that run one `.ll` under both tune
targets and CHECK the divergence.  The infrastructure test
(`mtune.ll`) pins both the code every name emits identically and a
constant divide that splits by multiply cost, including that `generic`
decides as penumbra2_5.  Cost *numbers* are a measure-on-hardware
follow-up: benchmark
on the ULX3S, not the ISS.

### Open decisions

- Feature granularity: start from the properties that have concrete
  consumers and grow on demand.
- Default-generation policy lives in `generic`'s definition in
  `Penumbra.td` (it follows the most-used variant);
  `PenumbraSubtarget::DefaultCPU` only names `generic`.

## Compiler: 16-bit jump-table entries when offsets fit

`PenumbraAsmPrinter::emitJumpTableEntry` always emits
`EK_LabelDifference32` entries (`.word target - JT_base`, 4 B
each). For switch tables whose target span fits in ±32 KB the
entries could be 16-bit (`.half`), halving table size in `.text`.

Fix: pre-walk the JT in `PenumbraTargetObjectFile` (or in the
AsmPrinter) computing max signed displacement; switch the entry
encoding to `EK_Inline` 16-bit when the range fits. The runtime
BRJT expansion would need a matching sign-extend before the ADD-
back step (`LDH +SEXT offset,[entry_addr]` → `ADD offset, base`
→ `JMP`). Low-priority cosmetic win — only matters for code-size-
sensitive builds and large switches; for now we always pay the
4 B/entry tax.

## Compiler: named byval args overlap on stack — RESOLVED

When a fixed (non-variadic) function received byval struct args
that spilled past R1-R4, adjacent stack slots overlapped: CC_Penumbra's
`CCAssignToStack<4,4>` reserved only a pointer-sized slot while the
framework's byval-mem path wrote `Flags.getByValSize()` bytes (8 for
`_Complex float`, 16 for `_Complex double`) at that offset.

Resolved by the aggregate-ABI rework above: the `PenumbraABIInfo`
classes route these types so the byval-on-stack path is never taken —
a 5–8 byte aggregate (`_Complex float`) goes Direct([2 x i32]) and a
> 8 byte one (`_Complex double`) goes indirect non-byval with a
clang-materialized copy.  The once-proposed `CCIfByVal<CCPassByVal<4,4>>`
CC change is moot for clang-emitted code.  `complex-7.c` passes and its
exclusion is removed.  The variadic-byval stack-overflow bug
(920625-1.c) was a separate issue, fixed earlier by
`normalizeVarArgByVal()` in `PenumbraCallLowering.cpp`.
