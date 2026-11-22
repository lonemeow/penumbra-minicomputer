# Compiler: passes, frames, calls and tuning

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

## Compiler: codegen pass/gate audit — what we leave at the default

Audited 2026-06-09 after the shrink-wrapping discovery: Penumbra
overrides *no* behavior gates (only accessor plumbing in
`PenumbraSubtarget.h`), so several generic optimizations that mature
targets opt into are silently off.

Swept again 2026-11-16 across the pass configuration and every hook
scalar code reaches in the subtarget, instruction, register, frame,
lowering and cost-model interfaces.  An item's estimated cycles come
from the `-O2` IR corpus (the `test-compiler` programs, Doom, the
bare-metal benchmarks and the kernel) compiled for `penumbra2_5`,
with each basic block run once through the timing reference's issue
and latency figures and every block weighted alike.

Status per item:

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
  placement.  Fixed in `40c15ff5805d`.
- **Shrink-wrapping — DONE.**  `enableShrinkWrapping` defaulted to
  false.  See "shrink-wrapping — what still limits it".
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

  **The pipelined-generation condition is met.**  `penumbra2_5` and
  `penumbra3` have their own models, and every model states the
  timing reference's issue and latency for ALU operations, loads,
  stores, multiplies and divides (`cdb7898`, `7cc0d6c`): a load's
  reader waits a cycle at distance 1 on `penumbra2_5`, and two on
  `penumbra3`.  `RDSYS` has no class.  The scheduler after register
  allocation runs on those models; this one stays off.

  **Measured 2026-10-04 on `penumbra2_5`, and not the scheduler to
  enable.**  With the load and store figures in the model, this
  scheduler adds instructions and register saves to Doom and hides no
  more load-use waits than the one after register allocation does
  alone ([ruled out](#ruled-out)).
- **Cross-block copy coalescing** (`enableJoinGlobalCopies`): off
  only because it defaults to `enableMachineScheduler()`, so nearly
  every other target has it.  Alone (`-join-globalcopies`) it
  estimates 0.09% fewer cycles in Doom, 0.03% fewer in the kernel and
  none in `test-compiler`, whose spills and reloads grow 0.13%.  A
  one-line override, for a gain below what a hardware A/B resolves.
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
  guard; they are side-effecting now.  Follow-up: "Compiler: constants
  are not rematerializable".
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
- **Stack-slot recognition** (`isLoadFromStackSlot`,
  `isStoreToStackSlot` and their `PostFE` forms): not implemented, so
  the spiller and stack-slot coloring cannot tell a spill from any
  other store, and keep pairs they could drop.  The visible upper
  bound is small: a reload directly after the spill of the same
  register to the same slot occurs 382 times in the kernel and twice
  in Doom.
- **Hot/cold splitting** (`-hot-cold-split`): an IR pass that moves
  the code behind `panic`, `KASSERT` and unlikely branches out of its
  function, so hot code packs denser into the 4 KiB L1-I.  That
  layout swung the kernel's syscall benchmarks by up to 6% either way
  in the gen2.5 A/B of the scheduler after register allocation.  Not
  measured; clang's `CCC_OVERRIDE_OPTIONS` adds the flag to every
  compile of an image build, so an A/B needs no code.
- **MachineOutliner / RISC-V-style save-restore millicode**: nothing
  implemented (`getOutliningCandidateInfo` etc.).  Code-size tools —
  attractive under the 1 KB I$ regime (21% of hot text is spill code;
  identical prologue/epilogue sequences outline well), but each
  outlined call costs BL+JMP at runtime, and gen2.5 predicts no
  return, so the JMP costs 5.  Bigger project; consider
  RISC-V's `-msave-restore` shape (dedicated prologue/epilogue
  routines) rather than the general outliner first.
- **PostRA scheduler** (no Penumbra scheduling model sets
  `PostRAScheduler`): pointless for gen1 (single-cycle, no pipeline);
  belongs in a pipelined generation's own scheduling model (hide
  load-use and divmul latency).  Measured 2026-10-04 on
  `penumbra2_5`: with the load and store figures in the model it
  removes half of Doom's and Dhrystone's load-use pairs and changes
  no instruction count
  ([the costs the compiler is not told](#compiler-the-costs-the-compiler-is-not-told)).
- **MachineCombiner** (`getMachineCombinerPatterns` + sched model):
  reassociation for ILP — little to gain on a single-issue core with
  full forwarding; revisit with gen4's dual issue.
- Not applicable: EarlyIfConversion (no predicated execution),
  MachinePipeliner (needs deep sched model, ILP machine),
  `enableSubRegLiveness` (no subregisters), GISel LoadStoreOpt
  (no wider load/store ops to merge into), SelectOptimize (turns a
  `select` into a branch for a target whose `select` is a
  conditional move; Penumbra's is a branch already, and the pass's
  triggers need branch weights).
- **TTI capability/cost audit** (opened by the SDAG action-table
  removal): with no SelectionDAG path, IR-level passes read machine
  capabilities from `PenumbraTTIImpl`, and un-overridden defaults can
  answer wrongly in both directions.  `hasDivRemOp` — **DONE**
  (reports the fused DIV_P/DIVU_P; DivRemPairs keeps `x/y; x%y`
  pairs for the GISel divrem fusion instead of decomposing the rem
  into mul+sub — one ~33-cycle divmul op per pair instead of two).
  `getArithmeticInstrCost` — **DONE** (`c1ea725`).

## Compiler: shrink-wrapping — what still limits it

LLVM's ShrinkWrap pass (runs post-RA, just before PrologEpilogInserter)
computes a save point / restore point pair in `MachineFrameInfo` so PEI
places the *whole* prologue (CSR saves + stack adjust) at the earliest
block dominating all frame uses instead of the entry block — early-exit
paths then run zero frame code.  It is gated per-target on
`TargetFrameLowering::enableShrinkWrapping`; every major target
(AArch64, ARM, LoongArch, Mips, PowerPC, RISC-V, X86) opts in, and
`PenumbraFrameLowering` does (`830c55e13181`).

The prologue and epilogue hooks refuse a block where the SP
adjustment would overwrite a live SR or R11.  SR is block-local by
construction today, which that liveness check relies on: code that
ever splits a block while flags are live must add the SR live-in
itself, as X86's select lowering does for EFLAGS.

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

### Static opportunity (measured 2026-09-22)

Method: static scan of `llvm-objdump -d` over the real built binaries.
A *tail-position call* is a `bl`/`jalr` whose only successors before the
function's terminal `jmp lr` are frame teardown — a `ldw` of a
callee-saved reg or `lr` from `[sp+N]`, plus the `sp` adjust — so the
result in r1/r2 passes through untouched.  Restricting teardown-loads to
callee-saved targets (excluding a `ldw r1,[sp]`, which would mean a
reloaded local rather than the call result) moved every count by <2%, so
the heuristic is tight.

| binary | total calls | tail-position | % | free-frame wrappers | no stack args |
|--------|------------:|--------------:|--:|--------------------:|--------------:|
| kernel (GENERIC.DEBUG) | 46,163 | 2,307 (2,107 `bl` + 200 `jalr`) | 5.0% | 687 | 1,897 |
| libc.so.12 | 20,366 | 888 (844 + 44) | 4.4% | 411 | 688 |
| doom | 6,458 | 350 (329 + 21) | 5.4% | 122 | 327 |

So ~4–5% of all call sites already compile to a tail-position call.  The
"free-frame wrapper" column is the subset whose stack frame exists
*solely* to save `lr` across the call; the last column is the subset a
first cut could take, having no outgoing stack arguments.

The `jalr` tail sites are device-driver method dispatch (`scsipi_*`,
`usbd_*`, `com_*`, `umass_*`) rather than VFS — `VOP_*` accounts for
28 of the kernel's 200, and those are not tail-callable at all (see
[Implementation](#implementation--a-small-first-cut-exists)).

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

### Measured leverage — small, and concentrated in the kernel

Static sites weighted by `+pcprof` execution counts: a Doom timedemo,
and a shell/VFS workload for the kernel and libc.  Saving is one
instruction per plain tail-position execution, and five plus a
write-through `stw lr` per *pure forwarder* — a function whose only
call is the tail one and whose frame saves nothing but `lr`.

| image | instructions | tail-position executions | instructions saved |
|-------|-------------:|-------------------------:|-------------------:|
| doom | 383,495,873 | 83,901 | 0.022% |
| kernel | 4,827,652,868 | 14,442,516 | 0.649% |
| libc.so.12 | 513,100,394 | 111,417 | 0.025% |

- **Dhrystone / compute kernels: ≈0.**  Their hot loops carry no tail
  calls — the same structural reason shrink-wrapping measured flat.  Do
  not expect DMIPS to move.
- **Doom cannot repay this at any implementation quality.**  Its
  entire call-and-return budget is 1.27% of its instructions
  (2,445,538 calls, 2,410,205 returns), and tail-position calls are
  3.43% of those calls.
- **The kernel is where the win is, and it concentrates.**
  `turnstile_exit` alone is 3.0M of the 4.6M pure-forwarder executions
  and `kpreempt_disabled` another 1.1M — two functions are 90% of the
  free-frame win.  Both are NetBSD base, so the compiler is the only
  lever that reaches them.
- **libc is structurally a dead end.**  Its hot syscall stubs are
  already frameless (`lli r11,N; syscall; bcs __cerror; jmp lr`), and
  the wrappers that do carry a frame are the 64-bit-offset ones
  (`pread`, `pwrite`, `truncate`), which pass stack arguments and so
  fall outside the first cut.  What remains is jemalloc bookkeeping at
  ~1k executions each.
- **Code size: ~−2K kernel instructions at the −1/site floor,
  ~−15–18 KB once free-wrapper frame removal counts** — helps the 1 KB
  direct-mapped gen1 I$ only where the wrapper text is hot.

Recursive tree walks are *not* a source of this: `R_RenderBSPNode`'s
7-register frame cannot be removed by tail calls or shrink-wrapping,
because tail-recursion elimination has already turned its right-child
recursion into a loop, hoisting the `viewx`/`viewy` pointers into
callee-saved registers that are live across three calls inside it.
The frame the recursion used to pay is now the loop's.

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

The no-stack-args predicate is a correctness condition, not only an
ABI convenience.  `VOP_*` builds its argument struct in its own frame
and passes `sp`, so tearing the frame down before branching would hand
the callee freed stack; a caller that passes the address of anything
in its own frame is disqualified the same way.  Excluding them costs
1.5M of the kernel's 14.4M dynamic tail-position executions.

RISC-V's `lowerTailCall` + `PseudoTAIL`/indirect-tail pseudo is the
matching template; AArch64's `isEligibleForTailCallOptimization` is the
eligibility-check reference.

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
   [instruction-timing.md](../system/instruction-timing.md) already
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

## Compiler: expand small constant-size memcmp and bcmp inline

A `memcmp` or `bcmp` with a small constant length is a library call.
ExpandMemCmp turns one into loads and compares only where
`TTI::enableMemCmpExpansion` names the load sizes to use, and
Penumbra's cost model does not override it.  The `-O2` IR corpus has
52 such calls of 16 bytes or less in the kernel and 163 in the
`test-compiler` programs (2026-11-16).

The expansion loads at whatever alignment the operands have, and
Penumbra lowers an under-aligned load to byte loads and shifts, so it
pays for short compares, or ones whose operands are known aligned.

Open:

- Override `enableMemCmpExpansion` for 4-, 2- and 1-byte loads, with
  `MaxLoadsPerMemcmp` as the budget.
- Count which of the kernel's 52 calls have aligned operands, and
  how often the paths that make them run.

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

`Penumbra.td` defines `penumbra1`, `penumbra2`, `penumbra2_5` and
`penumbra3` as processors over one shared architecture.  Each carries
a `PenumbraTuning` record pairing its tune features with its
scheduling model — `Penumbra1Model`, `Penumbra2Model`,
`Penumbra2_5Model` and `Penumbra3Model` — and every architecture list
is empty.  `generic` is
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
`ReadAdvance<ReadGPR, -1, [WriteLoad]>` for its load-use interlock.
Measured 2026-10-04, that one fact is worth half of Doom's load-use
pairs once a scheduler runs after register allocation, and the
scheduler before it costs more than it gains
([the costs the compiler is not told](#compiler-the-costs-the-compiler-is-not-told)).

**gen3 doubles the load's wait.**  A load's value forwards from the
register behind the last memory stage, so its reader waits two cycles
at distance 1 and one at distance 2
([memory completion](../internals/penumbra3/memory-completion.md#load-hit--a-registered-load-use-hazard)).
Counted by the ISS's load-use model (`+loadusemodel=ex,ex+wait:1`), a
single-issue core with every access a hit, those waits come to 131
cycles per 1000 instructions on `pbench kernel` and 147 on Doom
`demo3`, against 59 and 71 at gen2.5's one cycle
([penumbra4/load-use.md](../internals/penumbra4/load-use.md)).  In a
model that is gen2.5's line with the wait doubled,
`ReadAdvance<ReadGPR, -2, [WriteLoad]>`.  The count is the most a
schedule could hide, not what one would.  A schedule made for
`penumbra2_5` hides 30% of Doom's gen3 waits, by a count of adjacent
load-use pairs; what one made for gen3's own wait hides is not
measured
([the costs the compiler is not told](#compiler-the-costs-the-compiler-is-not-told)).

### Branch costs

Measured on the RTL testbenches
([instruction-timing.md](../system/instruction-timing.md)):

| Control transfer | gen1 | gen2.5 |
|---|---:|---:|
| `Bcc` not taken | 2 | 1 |
| `Bcc` taken / `B`, BTB-predicted on gen2.5 | 2 | 2 |
| mispredicted, or indirect `JMP` / return | 2 | 5 |
| `BL` | 3 | 2 predicted, 5 otherwise |

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

The compiler can hide that slot on gen2.5: a load, store, `MUL` or
`DIV` among the two instructions directly before a predicted taken
branch absorbs the discarded fetch
([instruction-timing.md](../system/instruction-timing.md#taken-branches-beside-a-stall)).
In the `-O2` IR corpus (2026-11-16), 505 of Doom's 2,301 backward
branches and 7,624 of the kernel's 17,638 have a memory operation
earlier in their block but none in those two slots.  That is an
upper bound: every ALU operation sets the flags, so the one a `Bcc`
reads stays directly before it, and a store ahead of its base's
increment moves past it only with its offset rewritten.  A
preference in the scheduler after register allocation would express
it, since no model field does.  gen3 hides nothing this way, so the
work pays only while gen2.5 is the board in use.

No scheduling-model field expresses "a predicted taken branch still
costs a cycle", and setting `MispredictPenalty` changes nothing: the
reference's figures (4 for `penumbra2` and `penumbra2_5`, 3 for
`penumbra3`) leave every file of the `-O2` IR corpus byte-identical
(2026-11-16).  Its readers are EarlyIfConversion and SelectOptimize,
which run only where a target adds them to its pipeline, and Penumbra
adds neither; SimplifyCFG adds `TTI::getBranchMispredictPenalty()`
(default 0, overridden only by X86) to its speculation budget, and
only for branches marked unpredictable.
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
- **Code density — landed, as the `small-icache` feature.**  A
  generation whose L1-I holds 4 KiB or less expands a constant-size
  `memcpy`, `memmove` or `memset` into at most four stores, where a
  larger cache takes eight.  The same property could later skew
  unrolling and the 16-bit jump-table choice.

### Validation

Lock each knob with lit tests that run one `.ll` under both tune
targets and CHECK the divergence.  The infrastructure test
(`mtune.ll`) pins both the code every name emits identically and a
constant divide that splits by multiply cost, including that `generic`
decides as penumbra3.  Cost *numbers* are a measure-on-hardware
follow-up: benchmark
on the ULX3S, not the ISS.

### Open decisions

- Feature granularity: start from the properties that have concrete
  consumers and grow on demand.
- Default-generation policy lives in `generic`'s definition in
  `Penumbra.td` (it follows the most-used variant);
  `PenumbraSubtarget::DefaultCPU` only names `generic`.

## Compiler: the costs the compiler is not told

The scheduling models state the reference's figures for ALU
operations, loads, stores, multiplies and divides, and on the
pipelined generations the scheduler after register allocation reads
them ([scheduling-models.md](../llvm/scheduling-models.md)); the load
and store figures landed in `7cc0d6c`.  Every other cost is LLVM's
default, and the defaults describe a machine where a call and a
register save each cost about what an ALU operation does.  On the
pipelined generations they do not
([instruction-timing.md](../system/instruction-timing.md)):

| Cost | What the compiler assumes | `penumbra2_5` |
|---|---|---|
| Call and return | the inliner's call penalty of 25, five instructions' worth | 7 cycles, and 8 more for each register the callee saves and restores |
| Jump-table dispatch | preferred from 4 cases | 11 cycles: five instructions, one a load whose reader waits, one a jump that is never predicted |
| First use of a callee-saved register | free: `getCSRFirstUseCost` returns 0 | a store and a load on every call, 8 cycles |

The traffic those costs fall on, counted from execution profiles of
code built by the compiler at `3f0f7412a3dd`:

| | Kernel, `pbench kernel` | Doom, `demo3` |
|---|---:|---:|
| Saves and restores of callee-saved registers, per 1000 instructions | 128 | 24 |
| Other loads and stores through SP, per 1000 instructions | 34 | 24 |
| Callee-saved registers saved per call | 2.74 | 3.44 |
| Calls whose callee saves all seven | 16% | 14% |
| Loads whose value the next instruction reads, per 1000 instructions | 101 | 127 |

One kernel instruction in eight saves or restores a register.

### How it was measured

Measured 2026-10-04 on the ISS.  Each cost was given to the compiler
as a command-line setting over a whole build of five workloads: the
`test-compiler` corpus at `-O2` (1,623 tests, which pass under every
setting reported here), Dhrystone, CoreMark, Doom running
`-timedemo demo3`, and the `GENERIC.DEBUG` kernel running
`pbench kernel`.

- **Instructions for fixed work are the measure:** a Dhrystone run, a
  CoreMark iteration, Doom's own code from the first frame rendered
  to the end of the demo, and kernel instructions per operation for
  four `pbench` rows, each run alone.  A row's figure is every
  kernel-text instruction in its profile over its operations, so it
  carries whatever else the kernel did meanwhile: under 3% of the
  total for three rows, and a fifth for `clock_gettime`, most of it
  the SD driver.  `clock_gettime`'s figure is therefore taken from a
  trace of the loop itself.
- **Load-use pairs and stack traffic** are execution counts from
  `+pcprof` laid over the disassembly, per 1000 instructions of the
  image's own text.  A pair is a load whose value the next
  instruction reads.  It is a different count from the cycles waited
  that [penumbra4/load-use.md](../internals/penumbra4/load-use.md)
  takes from a timing model, and the two do not compare.
- **Nothing here is a cycle count, and nothing has run on hardware.**
  The cache and TLB models' counts follow code layout more than they
  follow any setting: the Doom build with the first-use cost has 0.2%
  more text and takes 17% more misses in a 4 KiB L1-I.  They are left
  out.

The baseline is `3f0f7412a3dd` with `isLoadFromStackSlot` and
`isStoreToStackSlot` implemented.  Without them no pass can recognize
a spill or a reload the allocator wrote.  Alone they take 0.27% off
CoreMark and move nothing else.  The baseline also has the load and
store figures in `penumbra2_5`'s model; the first column adds the
scheduler that reads them.

### What each cost moves

Each column is one setting against the baseline; the last is all
four together.

| | Load and store timing, scheduled after allocation | First-use cost 16384 | Call penalty 150 | Jump tables from 8 cases | All four |
|---|---:|---:|---:|---:|---:|
| Dhrystone, instructions | 0 | 0 | 0 | −1.33% | −1.33% |
| CoreMark, instructions per iteration | +0.01% | 0 | −7.37% | −3.05% | −3.04% |
| Doom, instructions in its own code | 0.00% | −0.06% | +0.01% | −0.02% | +0.06% |
| Kernel instructions per `getpid` | +0.02% | −1.82% | +4.12% | +0.02% | +2.35% |
| per `clock_gettime` | 0.00% | −0.47% | +2.13% | 0.00% | +1.90% |
| per `pipe_pingpong` | −0.07% | −1.10% | +3.54% | +0.80% | +2.85% |
| per `fork_exit` | +0.13% | −1.46% | +1.48% | −0.08% | +0.27% |
| Corpus `.text` | −0.12% | +0.11% | +0.01% | −0.40% | −0.40% |
| Doom `.text` | 0.00% | +0.20% | −3.18% | −0.17% | −3.16% |
| Kernel `.text` | −0.01% | +0.41% | −5.03% | −0.25% | −4.94% |
| Load-use pairs: Dhrystone | −53% | 0 | 0 | −5% | −59% |
| CoreMark | −27% | 0 | −12% | −7% | −35% |
| Doom | −54% | +0.1% | +0.5% | 0 | −53% |
| kernel | −18% | +1% | −3% | 0 | −26% |
| Registers saved per call: kernel, from 2.74 | 2.74 | 2.23 | 2.64 | 2.78 | 2.15 |
| Doom, from 3.44 | 3.45 | 3.28 | 3.27 | 3.44 | 3.28 |

### The cost of a call

The inliner counts 5 for an instruction and 25 for a call.  By the
reference a call and its return are 7 cycles and each register saved
is 8, so at the kernel's 2.74 saves a call is about 30 cycles, or 30
ALU operations: 150 in the inliner's unit.
`TTI::getInlineCallPenalty` is the hook; the measurement used
`-inline-call-penalty`.

- **It makes every kernel path measured longer:** 1.5% to 4.1% more
  instructions at 150.  The penalty is both the credit for removing a
  call site and the price of every call the inlined body keeps, and
  the kernel's small inline helpers keep calls.  `KPREEMPT_ENABLE`
  holds two assertions and a call to `kpreempt`; at 150 it is no
  longer inlined into `mi_userret`, and every return to user mode
  takes 10 instructions more.  The kernel measured is
  `GENERIC.DEBUG`, whose assertions are such calls; `GENERIC` was not
  measured.
- **Text shrinks for the same reason:** 5.0% in the kernel, 3.2% in
  Doom.
- **Neither neighbour is better.**  At 75 the kernel's rows move
  between −0.4% and +1.6%.  At 300 they are 2.4% to 6.3% longer,
  CoreMark gains nothing more and Doom makes 1.7% more calls.
- **CoreMark's 7.4% is one decision:** `core_state_transition` is
  inlined into `core_bench_state`, at an inliner cost of 205 against
  a threshold of 225, and its saves go with it (22 saves and restores
  per 1000 instructions become 7).

The same arithmetic gives `penumbra1` about 19 ALU operations for a
call, so this is a figure for each model to state, not a constant.

### The first use of a callee-saved register

`getCSRFirstUseCost` is compared with the cost of spilling the value
instead: the summed frequency of the blocks that use it, on a scale
where the function's entry is 16384.  At 16384 a callee-saved
register is taken only for a value whose uses together run at least
once per call.
AArch64 and RISC-V return 5, three ten-thousandths of an entry; at 5
the corpus changes by 8 bytes.

- **At 16384 the kernel saves 2.23 registers per call instead of
  2.74.**  Saves and restores fall from 128 to 105 per 1000
  instructions and other stack traffic rises from 34 to 44: the
  allocator spills or splits instead, and comes out 14 per 1000
  ahead.
- **The four kernel rows take 0.5% to 1.8% fewer instructions,** for
  0.4% more kernel text.  Doom moves by 0.06%.
- **The value is not settled.**  A save and its restore are two
  memory operations per call, which on this scale is 32768.  There
  the saves fall further (kernel 2.06 per call, Doom 3.10);
  instructions for fixed work were not measured.

"shrink-wrapping — what still limits it" names this hook for its
argument-copy limit; whether a value this size splits that shape was
not checked.

### Jump tables, and where the settings collide

By the reference's figures a dispatch through a table costs 11 cycles
whatever the case count; a compare and a branch not taken cost 2, so
a tree three compares deep is 6 to 9 cycles when its branches are
predicted.  `setMinimumJumpTableEntries` is the hook; the measurement
used `-min-jump-table-entries`.  From 8 cases, Dhrystone takes 1.3%
fewer instructions and CoreMark 3.1%, Doom does not move, and the
kernel rows are flat to 0.8% worse.  From 16 the two benchmarks are
the same and the kernel rows move a little either way.  At 8,
CoreMark's seven-state switch, the one "jump tables stay
base-relative in a static link" counts, is no longer a table.

The inliner prices a switch by whether it will be a table, so this
threshold moves inlining too.  The inliner's own remarks for
`core_state_transition`, against its threshold of 225:

| Setting | Inliner cost | Inlined |
|---|---:|---|
| Baseline | 330 | no |
| Call penalty 150 | 205 | yes |
| Jump tables from 8 cases | 360 | no |
| Both | 235 | no |

Its switch costs 30 more as a compare tree, which puts it 10 over.
That is why all four settings together give CoreMark 3.0% where the
call penalty alone gives 7.4%.  On the kernel rows the four together
land between the call penalty's loss and the first-use cost's gain.

### Open

1. **Give each model the load and store figures and turn on the
   scheduler after allocation** for the pipelined generations.
   gen3's model needs the longer wait.  gen1 has nothing to
   schedule.  The two stack-slot hooks go in with it.
2. **The call penalty is not ready.**  At the value the reference
   gives it shrinks text and lengthens every kernel path measured.
   Open: whether the price of a call the body keeps and the credit
   for the call removed can be told apart, and what `GENERIC` does
   without the assertions.  Only then a value from the model: call,
   return, store and load cycles, and a stated number of registers
   saved per call.
3. **Settle the first-use cost** by measuring 32768 on instructions
   for fixed work, then state it from the model as well.
4. **Hold the jump-table threshold back** until its effect on
   inlining is a decision, not a side effect.
5. **The scheduler before allocation, told that only five registers
   are free.**  `getRegPressureSetLimit` is the override.  Cross-block
   copy coalescing alone was not measured either.
6. **Spill weights count a store and a reload alike,** where the
   store costs 6 cycles and the reload 2.  Not measured.
7. **Measure on hardware.**  Every figure here is an instruction or
   event count from the ISS.

## Compiler: interprocedural register allocation miscompiles six tests

With `-enable-ipra` a caller keeps values in caller-saved registers
across a call whose callee is known not to write them.  Six of the
1,623 `test-compiler` programs then fail at `-O2`
(`make test-compiler OPT="-O2 -mllvm -enable-ipra"`, 2026-10-04):

- `gcc-c-torture/execute/20090113-3.c`
- `gcc-c-torture/execute/20170401-1.c`
- `gcc-c-torture/execute/pr37573.c`
- `gcc-c-torture/execute/pr43784.c` (times out)
- `gcc-c-torture/execute/pr45070.c`
- `penumbra-codegen/narrowed-constant-halves.c`

Not root-caused.  The pass trusts each function's list of registers
written, collected from its instructions' operands, so the first
suspect is an instruction or an expansion that writes a register it
does not declare.  The pass is off by default and nothing built
today is affected, but any other pass that trusts operand lists
would be misled the same way.

Open:

- Reduce one failure to the instruction whose write is missing.
- Then measure the pass: with five registers free of a save, values
  kept in them across calls to small functions are where it could
  pay ([the costs the compiler is not
  told](#compiler-the-costs-the-compiler-is-not-told)).  A static
  estimate (2026-11-16, on the corpus and method of
  [the pass/gate audit](#compiler-codegen-passgate-audit--what-we-leave-at-the-default))
  finds little: 2.75% fewer callee-saved registers and 0.24% fewer
  cycles in `test-compiler`, and nothing in Doom or the kernel,
  because a callee counts only when its body is in the same file.

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

## Compiler: jump tables stay base-relative in a static link

`getJumpTableEncoding` returns `EK_LabelDifference32` for every
relocation model, and `emitJumpTableEntry` emits `target - table`
whatever the encoding says.  That is what a PIE needs, since the
entries then take no dynamic relocation.  A static link pays for it
with an `ADD` on every dispatch:

```
shl  r6, 2
add  r6, r11            ; the table
ldw  r6, [r6 + 0]
add  r6, r11            ; the entry is an offset from the table
jmp  r6
```

With absolute entries the second `add` goes.  CoreMark's state
machine dispatches 4,600 times an iteration, so it is 1.2 % of its
instructions ([workloads.md](workloads.md), "hot-loop code review —
CoreMark").  The kernel is a static link too.

Open:

- Emit absolute entries, `EK_BlockAddress`, when the relocation model
  is static, and drop the add-back from the `G_BRJT` selection there.
- This and "16-bit jump-table entries when offsets fit" pull opposite
  ways: a 16-bit entry is necessarily an offset.  Decide by relocation
  model and size together.

## Compiler: register copies are a tenth of the instructions

A `mov` between registers is 8.9% of Doom's instructions, 9.9% of the
kernel's and 8.7% of the `test-compiler` programs' in the `-O2` IR
corpus compiled for `penumbra2_5` (2026-11-16).  Where they come from
is not known.  The candidates are the two-operand ALU forms, whose
destination is also a source, so a value still live past the
operation is copied first; argument and return registers; and copies
PHI elimination leaves, of which
[a loop-carried value that is also live out](#compiler-a-loop-carried-value-that-is-also-live-out-is-copied-twice)
is one measured shape.

Open:

- Attribute the copies in Doom's and the kernel's hot code to their
  sources, weighted by how often each runs.
- Then pick the lever for each source: allocation hints, commuting a
  two-operand form, or splitting an edge.  The ISS pairing model's
  `movop` fusion, studied for gen4, would remove the first kind's
  cost in hardware.

## Compiler: a loop-carried value that is also live out is copied twice

A pointer stepped once an iteration, whose value before the step is
used on a loop exit, takes three instructions where the step is one.
CoreMark's `core_state_transition` walks its input this way:

```
ldb  lr, [r4 + 0]
cmp  lr, 0
beq  exit               ; stores r4
mov  r6, r4
add  r6, 1              ; str + 1, beside the old str
cmp  r3, 1
bne  head               ; else exit, storing r4
...
head:
mov  r4, r6
```

The exit on the second branch stores the pointer before the step and
the loop continues with it after, so both are live across that
branch.  Two instructions a character, 2.2 % of CoreMark's
instructions ([workloads.md](workloads.md), "hot-loop code review —
CoreMark").

Not root-caused.  The edge back to the loop head is critical: its
source has two successors and its target two predecessors.  PHI
elimination puts the head's copy at the end of the source block,
ahead of the branch, so the copy runs on the exit path too and the
old value has to survive in a second register.  Splitting that edge
would let the step sit on it alone as `add r4, 1`, at the price of
a block that needs a place where it costs no jump.

Open:

- Confirm the cause on the MIR before and after PHI elimination.
- Count how often the shape occurs: a static scan for `mov a, b;
  add a, C` whose `a` is copied back to `b` at a loop head.

## Compiler: backend byval handling is only safe because clang avoids it

Clang's `PenumbraABIInfo` passes a 5–8 byte aggregate as
`Direct([2 x i32])` and a larger one indirect *non-byval*, with the copy
made in IR, so clang-emitted code never reaches the backend's byval
paths.  Those paths still carry the two defects the aggregate-ABI work
found (`test/compiler/penumbra-abi/`), and IR from any other producer
that uses `byval` hits them silently:

- a register-slot byval aggregate gets no copy, so pass-by-value
  becomes pass-by-reference;
- `CC_Penumbra`'s `CCAssignToStack<4,4>` reserves a pointer-sized slot
  for a stack byval argument while the framework writes
  `getByValSize()` bytes there, overlapping the arguments after it
  (8 bytes for `_Complex float`, 16 for `_Complex double`).

Variadic byval is separate and handled (`normalizeVarArgByVal()` in
`PenumbraCallLowering.cpp`).  Either implement byval properly —
`CCIfByVal<CCPassByVal<4,4>>` plus the register-slot copy — or reject
`byval` with a fatal error, so the miscompile cannot happen quietly.
Not re-verified since the rework.

## Ruled out

The first three were measured 2026-10-04 the way
[the costs the compiler is not told](#compiler-the-costs-the-compiler-is-not-told)
describes, against the same baseline; the rest 2026-11-16 on the
corpus and estimate of
[the pass/gate audit](#compiler-codegen-passgate-audit--what-we-leave-at-the-default).

- **The machine scheduler before register allocation,** on top of
  the one after it.  In its bidirectional mode, with the cross-block
  copy coalescing its hook turns on, it removes no more of Doom's
  load-use pairs (53.4% against 53.7%) and adds 0.49% to Doom's own
  instructions, and 1.4% to the loads and 2.4% to the stores of the
  whole run.  `R_DrawColumn` saves `r5`–`r8` where it saved `r5` and
  `r6`: the scheduler holds register pressure under the 12
  allocatable registers, and only five of them, `r1`–`r4` and `r11`,
  cost nothing to use.

  Register-pressure tracking was on in that run: the generic
  scheduler tracks pressure in any region longer than half the
  allocatable registers, six instructions here.  Forcing it on
  everywhere (2026-11-16) changes nothing measurable, and lowering the
  pressure limit to five registers keeps the `R_DrawColumn` saves:
  that setup block needs seven registers in source order, so it
  exceeds any such limit either way.  On the audit's estimate, every
  variant tried (each direction, forced tracking, the lower limit)
  costs `penumbra2_5` 0.8–1.8% in Doom, 0.1–0.4% in the kernel and
  2.3% in `test-compiler`.  For `penumbra3` it saves 1.0–1.2% in Doom
  and 0.3–0.5% in the kernel, and costs 0.5–0.9% in `test-compiler`;
  the saving is an upper bound, since it prices a spill store at the
  store buffer's one cycle.  Worth an A/B once gen3 runs NetBSD.

- **Turning every select into a branch before selection.**  With no
  conditional move a `select` ends as a branch either way, and
  `isSelectSupported()` returning false has CodeGenPrepare make it
  one early.  CoreMark then takes 9.7% more instructions and Doom's
  own code 4.0% more.  With selection looking through the `freeze`
  CodeGenPrepare puts on each condition it is 8.4% and 2.3%, with
  1.9% more text in the `test-compiler` corpus, against 0.8% fewer
  instructions on Dhrystone.  Not root-caused past the `freeze`.
  The change was never committed.
- **Runtime and partial loop unrolling by default**
  (`-unroll-runtime -unroll-allow-partial`).  Text grows 52% in the
  `test-compiler` corpus, 12.5% in Doom and 5.5% in the kernel.
  Doom's own code runs 0.67% more instructions and CoreMark 0.71%
  fewer.  A preference for particular loops through
  `getUnrollingPreferences` is a different proposal, and nothing
  measured here argues for one.  gen3's 8 KiB L1s and 4-cycle fills
  from the L2 make the text growth cheaper there, but the instruction
  counts above still point to a wash; only a gen3 A/B would tell.

- **Spill-copy elimination** (`enableSpillageCopyElimination`).  It
  changes 9 of the corpus's 1,734 files, and no estimated cycle.

- **A cost on a callee-saved register's first use**
  (`getCSRFirstUseCost`).  The allocator takes the five registers
  free of a save first anyway, so the cost only weighs a first
  callee-saved register against a split or a spill.  At 2 and 5 it
  moves no estimate past 0.03%; at 20 the kernel spills 1.5% more to
  use 2.7% fewer callee-saved registers, for 0.22% more cycles.
