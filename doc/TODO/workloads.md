# Workloads

Profiles and code reviews of real workloads (Dhrystone, Doom,
SQLite, audio mixing, CoreMark-Pro) whose levers span the compiler,
libc, the kernel and the workload source.

## Compiler/benchmark: Dhrystone hot-path code shape — where the cycles go

Measured 2026-06-09 (PC-weighted ISS trace histogram over 300 iterations
+ disassembly review; method: `+trace=` through a FIFO into a per-PC
`awk` histogram, symbolized against the ELF).  Numbers are for the
`benchmark/` bare-metal Dhrystone at -O2, gen1 geometry (1 KB
direct-mapped L1-I, 16 B lines).

### The headline: I-footprint, not instruction count

- **928 dynamic instructions per iteration** (ISS, CPI=1 by
  construction; the ISS-ideal bound is 13.2 DMIPS @ 25 MHz, so
  HW-DMIPS / 13.2 is the effective stall multiplier).
- Per-iteration hot text: **81 distinct 16 B lines (1296 B) vs the
  64-line (1 KB) direct-mapped I$**.  30 of 64 sets hold more than one
  hot line; ~70 of the 81 lines share a set with another line touched
  in the same iteration.  A cyclic sweep through that working set
  conflict/capacity-misses nearly every line, every iteration —
  expect L1-I read misses/iteration in the ~70–80 range on hardware.
  (Confirmed: 77/iteration measured on the ULX3S pre-Localizer;
  44/iteration after — see the pass/gate audit entry.)
- Instruction-count micro-optimizations are invisible under this:
  the select-CMPi fold (commit `8a23e2222632`) measured flat on HW
  Dhrystone for exactly this reason.

### Dynamic instruction shares (per iteration)

| code | insns/iter | share |
|------|-----------:|------:|
| memcpy (2× 48 B struct assign) | 292 | 26% |
| strcpy | 189 | 17% |
| strcmp | 165 | 15% |
| main loop body (Proc_1–5, Func_3 inlined) | 131 | 12% |
| Proc_8 | 64 | 6% |
| Proc_6/7, Func_1/2 | ~95 | 9% |

The string routines are 58% of dynamic instructions from only ~10
I$ lines — they are the cache-*friendly* part (tight pointer-bump
loops).  The other ~40% sweeps the ~70-line Proc/loop text.

### Findings, ranked by leverage

1. **Spill/reload density is the top compiler lever.**  21% of hot
   static text is `stw`/`ldw` against `[r14 + N]` (callee-save
   prologues + locals reloaded across calls), another 14% is `mov`
   shuffles (two-address tax).  The loop body alone is 39% loads.
   Examples: `Proc_8` saves 6 callee regs for straight-line address
   arithmetic; `Proc_1` saves 7.  This matters doubly because spill
   *stores* are write-through all the way out on gen1 — and gen2
   keeps write-through L1 at least initially, so spill cost carries
   forward.  Candidate directions: shrink-wrapping, register-pressure
   aware address-arithmetic scheduling/CSE, revisiting CSR allocation
   order.
2. **Word-wise string routines** (bare-metal `benchmark/common` and
   NetBSD libc both): rolled byte/word loops cost ~150–200
   insns/iteration here for ~+10 lines of shared footprint.  Related:
   the existing "Libc: memcpy misses same-offset misaligned shortcut"
   entry.
3. **I$ geometry is a gen2 matter.**  gen1 2-way associativity was
   tried and **blew timing** (the fetch/TLB path is already the fmax
   limiter) — treat gen1 I$ improvements as infeasible; gen2's BRAM
   VIPT 4-way design (Decision 11) is the fix.
4. **Branchless verbosity is real but small in Dhrystone**: exactly
   one hot site (Proc_6's 8-insn `sext(eq)+add` select-of-constants
   chain, see the branch-cost entry in compiler-pipeline.md).
   Kernel hot paths may differ — measure there before investing.
5. **Signed sub-word loads — the Dhrystone instance is now gone
   (re-measured 2026-06-22).** The 2026-06-09 profile saw ~6
   `shl 24; sar 24` sext-of-char chains in the loop body; the eq/ne
   char-compare cases since fold to plain `LDB` and the rest to `LDBS`,
   so the Dhrystone binary now carries **zero** adjacent sext chains
   (1 `ldb+shl24+sar24` site total, not on the hot path). The remaining
   gap is the *signed-relational* shape (`while (*p > 0)`), not eq/ne —
   see the "signed sub-word loads in relational compares" entry, now
   re-scoped and measured small.
6. Minor: `Proc_1`–`Proc_5`/`Func_3` are fully inlined but their
   out-of-line bodies stay linked (extern linkage) — ~500 B of dead
   text between hot functions.  Harmless to the cache (never fetched)
   but skews naive footprint reading; `--gc-sections` +
   `-ffunction-sections` would drop them.

## Compiler: hot-path code review — kernel, SQLite and Doom

Reviewed 2026-09-13: the `GENERIC.DEBUG` kernel, `libsqlite3` (PIC) and
a Doom rebuild, all from the same compiler at the default tuning.
Doom's hot functions came out the same under `-mcpu=penumbra1` apart
from constant multiplies.  All counts are static disassembly scans by
throwaway scripts.  Nothing is weighted by execution frequency yet;
for Doom, the renderer counters of a `PERFCTR=1` run supply the
weights.

Compiler causes, widest reach first, each with its own entry:

| Cause | Shows up in | Entry |
|---|---|---|
| No branch probabilities reach block placement | every `__predict_false`, KASSERT and cold path | `40c15ff5805d` |
| Countdown loop exits test a materialized overflow flag — RESOLVED | Doom column and span drawers: 6 of 18 instructions per pixel | `9a9bcc05c70a` |
| s64 relational compares — RESOLVED | SQLite b-tree bisection and record compare | `8c53d2bb4b84` |
| Compare immediates, TEST, inverted booleans | 848 / 612 / 274 / 140 sites | "compare and branch selection misses immediate and TEST forms" |
| Byte and halfword sign-bit tests | SQLite varint bytes, BSP child index, `sy_narg` | "signed sub-word loads in relational compares miss LDBS/LDHS" |
| Constants never rematerialized | 220 kernel loops; ~1,250 SQLite constant reloads | "constants are not rematerializable" |
| `&global + C` built in the entry block; addresses hoisted into callee-saved registers | 1,282 SQLite sites; Doom BSP walk frame | `dc921849a615` (resolved), "early MachineLICM hoists global addresses into callee-saved registers" |
| `p - K` and integer MMIO addresses miss the offset fold | memmove backward loops; 187 driver sites | "pointer-minus-constant and integer MMIO addresses miss their folds" |
| 64-bit counter increments — RESOLVED | every syscall and interrupt | `fb237ee8d98a` |
| A store between producer and compare | `KPREEMPT_ENABLE` on every `userret` | "compare-elim does not look through COPY" |

Most of the frame cost belongs to the shrink-wrapping and tail-call
entries.  Shrink-wrapping is enabled, but its reach is limited — see
item 1 of its open list.

### Tree walks

Doom's BSP walk is the first tree-walking code studied here.  Its data
access is already close to minimal — the node loads and `nodes[]`
indexing leave nothing to take.  The cost per node is the call:

- The recursion saves 7 callee-saved registers per level, leaves
  included (tail-call and shrink-wrapping entries).
- Early MachineLICM moves the `viewx`/`viewy` address loads into
  callee-saved registers: two more saves per call (early MachineLICM
  entry).
- The `NF_SUBSECTOR` test on the i16 child index is 6 instructions for
  a 2-instruction bit test (sub-word entry).
- The near/far child order and early exits are laid out without
  probabilities.
- `R_PointOnSide` keeps its frame on its early exits, and `FixedMul`
  and `R_PointToAngle` are calls into other files ("Doom: renderer
  source-level costs").

A tree walk needs cheaper calls more than better expression code: fewer
saved registers, and recomputation preferred over a callee-saved
register for anything one or two instructions long.

### Smaller findings

- **Pointer copy in scan loops.**  The `solidsegs` scans in
  `R_CheckBBox`, `R_ClipSolidWallSegment` and `R_ClipPassWallSegment`
  use the pointer after the loop, so the tied `ADD` needs a copy every
  iteration: `mov; ldw; add; cmp; blt`, one instruction more than
  necessary.  43 small loops in Doom.
- **MachineCSE leaves same-block duplicates.**  `R_MapPlane`
  recomputes `y << 2` for each parallel array.  On a MIR repro,
  `-run-pass=machine-cse` merges duplicate address pseudos but not
  `SHLi`, `NOT` or `LLIS`, even with `-aggressive-machine-cse`.  The
  rejecting check is not identified yet (needs an assertions build).
  17 Doom sites.
- **Cancelling adds.**  `sqlite3VdbeExec`'s `jump_to_p2` does
  `llis r7,-20; add` on the way in and `add r7,20` at the shared tail
  (34 branches in); riscv32 does the same.  98 cancelling or mergeable
  pairs in `libsqlite3` — a post-RA constant add merge would catch
  them.
- **Copy churn.**  75 `mov a,b; mov b,a` pairs at join points and 30
  duplicate `LLI`s (`sqlite3VdbeSerialGet`: `lli r1,4; sth; lli r1,4`)
  in `libsqlite3`.

Checked and not levers:
- SQLite's big-endian `get4byte` decode and its ×40 `aMem[]` scaling
  are already minimal.
- `pcache1` divides by a run-time `nHash`.
- Doom's `(frac >> 16) & 127` column index is already minimal as
  compiled.  Keeping the fraction pre-shifted in the source removes
  the `AND` — see "Doom: FastDoom techniques that transfer".
- SQLite's VDBE dispatch — 13 instructions per opcode, including an
  `STW` of `nVmStep` for lack of registers — is PIC jump-table cost
  plus register pressure.  The bounds check could only go by padding
  the table to 256 entries.

Source-level findings from the same review are in "Kernel: source-level
costs on the trap, syscall and driver paths" and "Doom: renderer
source-level costs".

## Doom: where a frame goes

Measured 2026-09-18 on hardware at 37.5 MHz: a `PERFCTR=1` build
playing the 150-tic `BENCH.lmp` timedemo of Freedoom's E1M1.  By feel
a 386DX/40 runs Doom 4–5× faster.

| | lumps read into a 16 MB zone | WAD mapped, 4 MB zone |
|---|---:|---:|
| fps | 2.52 | 3.03 |
| instructions / frame | 5.02M | 4.03M |
| cycles / frame | 14.8M | 12.3M |
| CPI | 2.95 | 3.05 |

The mapped run's stalls: load 21%, store 14%, ifetch 14%, flush 11%,
hazard 4%, funit 3%; hit rates L1I 97.2%, L1D 82.4%, L2 86.8%.

- **The instruction set is not the gap.**  Host gcc 11 at `-m32
  -march=i386 -O2` (compile-only, with an empty `gnu/stubs-32.h` and a
  `sys/endian.h` shim) against our clang `-O2` over the eight renderer
  files: 6,151 instructions against 9,026, the usual CISC/RISC ratio.
  The pixel loops are within 1.2× — `R_DrawColumn` 10 against 12 per
  pixel, `R_DrawSpan` 15 against 17, the difference being x86's
  base+index addressing — and with every access hitting, Penumbra is
  ahead per clock: the column loop measures 22 cycles a pixel on
  penumbra2_5 in RTL simulation, against ~35 for the i386 C loop by its
  timing tables.  Outliers:
  `R_RenderSegLoop` 201 against 405, 139 of them `lli`/`lui` pairs for
  globals (`LUI` ORs, so there is no `%hi`/`%lo` fold into the load);
  `R_DrawVisSprite` 84 against 242, not yet examined.
- **The pixel loops are ~16% of the instructions.**  46.5k pixels a
  frame at 12 and 17 instructions come to 0.65M of 4.0M.  A column
  call draws 7.2 pixels on average (29,058 in 4,014 calls), so
  per-call setup outweighs the pixels — see "Doom: renderer
  source-level costs".
- **The BSP walk is not a lever.**  LLVM already turns the back-side
  recursion into a loop, and the front-side descent's 7-register frame
  is at most ~3.8k memory operations a frame, ~0.1%.  The traversal
  proper — `R_AddLine`, `R_CheckBBox`, `R_FindPlane`'s linear scan of
  the visplanes, the node visits — is an estimated ~0.2M instructions.
  The `bsp` phase is big because every wall runs inside it: only one
  `R_StoreWallRange` call in 16 is bracketed as `wall`.
- **Part of the frame is not Doom's.**  The per-frame work counts are
  identical in every run, yet instructions fell from 5,025k to 4,719k
  after a change that touched no frame code (unbuffered stdout), and
  to 4,029k once the lumps left the zone.  That second drop sits in
  the sprite (−570k) and plane (−171k) phases, whose data moved; a
  time-uniform cost would have come mostly out of `bsp`.  Skipping
  `Z_ChangeTag2` accounts for ~60k.  TLB refills are the suspect — see
  "Kernel: count TLB refills".  Until that is understood, one layout's
  runs vary by ~6%: judge small changes over several runs.

### Instruction profile (2026-09-22)

Measured on the ISS with a scratch patch — a PC histogram, per-vector
exception counts and a TLB-miss histogram by faulting page; none of it
is in the tree — playing the same timedemo from the first
`R_RenderPlayerView` to the report line: 149 frames, 522M
instructions, 3.50M a frame.  The ISS models no cache or memory
latency, so these are architectural instruction counts and not cycles;
the drawer loops and the refill handler carry very different stall
weights on hardware.  The suspected refill cost above is now counted,
and it is the second-largest item in the frame.

| per frame | insns | share |
|---|---:|---:|
| `R_DrawColumn` (4,016 calls, 29,059 pixels) | 595k | 17.0% |
| TLB refill handler (9,425 refills) | 544k | 15.5% |
| `R_RenderSegLoop` (2,289 column iterations) | 450k | 12.8% |
| `R_DrawSpan` | 312k | 8.9% |
| `R_DrawMaskedColumn` | 233k | 6.7% |
| libc `memcpy` — the framebuffer blit | 175k | 5.0% |
| `R_MakeSpans`, `FixedDiv`, `R_GetColumn` | 103k, 100k, 70k | 7.7% |
| rest of the kernel (idle, SD paging, clock) | 207k | 5.9% |

Doom 73%, kernel 21%, libc 5.6%; four functions are 45% of the whole.
The run is cold — the mapped WAD pages in from SD during it, which is
where `pmci_burst` (32k a frame) and most of the idle time come from.

- **Reaching globals is 16% of everything executed.**  `lli`+`lui`
  address pairs are 11.1% (98% of the executed `lui`s address
  `.data`/`.bss`) and loads of globals another 4.7%.
  `R_RenderSegLoop` is 34% address materialization and 11% global
  loads; `R_DrawColumn` spends 61 instructions of setup, ten of them
  address-plus-load triples, on an average 7.2-pixel column.  What
  removes both halves is the source change — pass the drawer state
  instead of storing it into `dc_*` before each call, the shape
  `R_PointToAngle2` already has.  Globals over parameters was right on
  a machine with eight registers; here a global costs three
  instructions to address before the load, and the call inside the
  column loop forces every one of them to be reloaded after it.
- **The compiler's share of that is smaller than it looks.**  Of the
  11.1%, 6.2 points are the *same* address recomputed again within one
  call (`R_RenderSegLoop` 4.0, `R_DrawMaskedColumn` 1.5): layout-
  neutral, and worth a live register against two instructions an
  iteration — which still loses when the loop calls out and the base
  has to be callee-saved.  The other 3.5 points need distinct globals
  within ±32 KB of each other, a placement bet the split L1 can make
  expensive, since instruction fetch never seeds L1D.  See
  "G_GLOBAL_VALUE materialization defeats CSE and rematerialization".
- **The blit is 5%.**  libc `memcpy`'s word loop is 7 instructions per
  4 bytes and unrolled zero times: 112k instructions a frame for the
  64 KB screen copy alone — see "Libc: cache-line-align the
  memcpy/memset hot loops".
- **The rest of the codegen ranking does not reach Doom.**  64-bit
  compares: the renderer is 32-bit fixed point.  Tail calls: the BSP
  walk is 0.3% of a frame.  Dead flag definitions: PIC anchors, and
  Doom is not PIC.  Inlined small memory calls: Doom's copies are
  64 KB, not 32 bytes.

The profiler is `+pcprof`, with `sw/tools/pcprof-report.py` for the
symbols; how to drive a NetBSD workload under it is in the root
CLAUDE.md.

### Judging a change: Doom's own instructions, never the total

Measured 2026-09-22.  Inlining `FixedMul` (`31b62fe`) grew `.text` by
304 bytes and left every data access identical, yet the run's TLB
refills went from 1.28M to 1.84M and the total instruction count rose
5.5% — while the code that changed got *cheaper*, Doom's own
instructions falling 1.3%.

Relinking the same object files at a different base settles what that
was: Doom's own work came out within 0.1%, refills fell 26% (1.84M →
1.36M) and the total fell 5.3%.  Identical instructions, different
addresses.  So a few hundred bytes of text movement swings the frame
by more than any of the source or codegen changes measured here, and
the present 3.7 fps is one arbitrary draw from that distribution.
Two consequences:

- Report a change as Doom's own instruction count *plus* the refill
  count, never as one total.  A total compares two layouts as much as
  two programs.
- A deliberate layout — function ordering, or padding chosen against
  the TLB's sets — is worth measuring in its own right, on the same
  footing as the drawer work.  The kernel-side version was tried and
  declined — see "Ruled out" in [kernel.md](kernel.md) — on evidence
  that may carry over: the layout noise swamped the effect.

`r_phase` gaps:

- Every mark is a `sysctl` call.  A sampled wall call carries two, and
  the scaled `wall` estimate (7.8M cycles) exceeds the whole bsp+wall
  bucket (6.9M), so a mark pair costs at least ~15k cycles.  Calibrate
  it with a back-to-back pair and subtract before trusting the
  bsp/wall split.
- Only cycles and instructions are kept per phase, although `read_cpu`
  fetches all eight counters.  Per-phase stall buckets would say which
  phases are load- or store-bound.
- The whole-run `perf_report` line subtracts a single snapshot pair of
  32-bit counters, so it is meaningless past 2^32 cycles (~115 s at
  37.5 MHz).  `BENCH.lmp` runs ~60 s.

## Doom: renderer source-level costs

Found in the hot-path code review (2026-09-13).

- **Span and low-detail drawers reload their globals per pixel.**
  `R_DrawSpan`, `R_DrawSpanLow` and `R_DrawColumnLow` read their
  colormap and source pointers through globals the byte store may
  alias, so both are reloaded every pixel — 8 cycles on penumbra1, 4
  on penumbra2_5.  `b82fb4b0df29` hoisted them into locals in
  `R_DrawColumn` only.  With the countdown-exit fix as well, the span
  loop goes from 23 instructions to 15.
- **The drawers take their arguments through globals.**  Every
  `R_DrawColumn` call reads ten `dc_*`/`ylookup`/`columnofs`/`centery`
  globals, each an address pair plus a load, for an average 7.2-pixel
  column: 61 instructions of setup against 12 per pixel, 7% of the
  whole frame ("Doom: where a frame goes").  `R_RenderSegLoop` pays
  the same tax per column and cannot keep anything in registers across
  its `R_GetColumn` and `colfunc` calls.  Parameters — the shape
  `R_PointToAngle2` already has, and what `R_PointToAngle` avoids —
  remove both the addressing and the reloads; no compiler change can
  remove the reloads, because the calls may alias the globals.
- **`RANGECHECK` is defined** (`doomdef.h`), adding 4–5
  compare-and-branch pairs to every drawer call.
- **Hot helpers are out of line in other files.**  `FixedMul` is now a
  `static inline` in `m_fixed.h` (`31b62fe`), worth 1.3% of Doom's own
  instructions — but only once the compiler fused a widening multiply
  whose operand came from memory (`2dbdf66`); on the compiler before
  it, the same change cost 0.7%, the call having hidden the gap.
  `SlopeDiv` and `R_PointToAngle` are still called, 152k and 5.2M
  instructions a run.
- **BSP leaves recurse to learn that they are leaves.**
  `R_RenderBSPNode` is entered for a subsector child only to test
  `NF_SUBSECTOR` and call `R_Subsector`, paying the full frame.
  Testing the flag in the parent before recursing removes a frame per
  leaf, whatever the shrink-wrapping and tail-call work delivers.
- **Every wall column piece pays a three-call lump lookup.**
  `R_GetColumn` → `W_CacheLumpNum` → `Z_ChangeTag2` runs ~100
  instructions with three call/return pairs and 12 stores, 11 of them
  register saves: `W_CacheLumpNum` saves six registers on its hit path
  for the sake of its miss path (`Z_Malloc`, `W_Read`).  A mapped lump
  skips `Z_ChangeTag2`, and its address never changes, so a column
  pointer table built alongside each texture's lookup would make
  `R_GetColumn` two or three loads.  Lumps read into the zone
  (misaligned ones) can be purged and still need the lookup.
- **Each wall column divides and calls.**  `R_RenderSegLoop` computes
  `dc_iscale` with a `DIV` (34 cycles on penumbra2_5) and the texture
  column with a `FixedMul` call, per column; masked columns divide
  too.

## Doom: R_Init reads every texture patch and sprite

The long silent pause after R_Init's first 16 dots is
`R_GenerateLookup` running over all 963 Freedoom textures.  It
`W_CacheLumpNum`s every patch they use — 1,045 lumps, 10.1 MB — for
each patch's width and `columnofs` table, ~350 KB in all.  The 14 slow
dots after it are `R_InitSpriteLumps` reading all 853 sprite lumps
(2.5 MB) for three header fields each.  At SD speed ("Hardware: SD
read throughput is bottlenecked by pmci_burst byte-PIO") that is over
a minute; id's far smaller IWADs made vanilla's same eager scan cheap.

Reading only headers, or mapping the WAD as the port now does, leaves
the SD traffic as it is: the root filesystem's blocks are 64 KB and
patches average ~10 KB, so every block is read anyway.  Mapping did
remove the copies' memory pressure — faults waiting for memory went
from 53 to none and pagedaemon wakeups from 124 to none.

E1M1 uses 114 textures, 163 patches, 1.1 MB.  Ways to read only those
without stalling play:

- Build a level's lookups during level load, where `R_PrecacheLevel`
  already reads its patches.  Switch and animation frames missing from
  the sidedefs would still load on first sight.
- Cache the computed lookups and sprite dimensions in a file keyed to
  the WAD, so only the first start pays.

Deferring to first use is out: play would stall whenever something
new came into view.

The mapped default reads misaligned lumps into the zone instead
(`LumpIsMapped` in `w_wad.c`); that path has not yet run with an
unpadded WAD such as id's.

## Doom: FastDoom techniques that transfer

Studied 2026-09-18 from FastDoom (github.com/viti95/FastDoom), a DOS
fork tuned for 386 and 486; paths are under its `FASTDOOM/`, and much
of its asm is id's original.  x86-only tricks do not transfer: `lea`
chains, immediates patched by self-modifying code, `shld` span
extraction, Pentium and MMX variants, planar VGA.  Those that cut work
per pixel or per store do, in rough order of payoff here:

1. **Low and potato detail with wide stores** (`linearl.asm`,
   `linearp.asm`): each texel written once as an `STH` or `STW`.
   Halves or quarters the pixels, the stores and the per-column setup.
   Chocolate Doom has low detail; potato would be new.
2. **Column drawer.**  A fraction kept pre-shifted to 7.25 wraps with
   `>> 25` and no `& 127` — the disabled unrolled `R_DrawColumn` in
   `r_draw.c` already does this.  Unroll ×8 with constant-offset
   `STB [dest + k*320]` and enter through a computed jump; FastDoom's
   full 200-way unroll (`linearh.asm`) is 6.4 KB, past the I-cache.
3. **Spans storing four pixels per `STW`** — `planar2.asm` pairs two
   per 16-bit store.  One write-through store in place of four.
4. **Direct drawers** for columns drawn 1:1 — sky, player weapon
   (`linearh5.asm`) — with no fraction arithmetic.
5. **Flat-shaded visplanes** (`r_plane.c` `R_DrawPlanesFlatter*`,
   colour from `R_GetFlatColor`): FastDoom's largest measured gain,
   but a visual trade-off, so an option at most.
6. **Status bar copied only when it changes**: vanilla's
   `updatestate`, which Chocolate Doom dropped.

Its iterative BSP walk (after GBADoom) buys little here, since the
recursion is ~0.1% of a frame ("Doom: where a frame goes").  Drawing
straight into the uncached framebuffer would save the 64 KB copy each
frame — under write-through every store reaches the bus anyway — but
tears without page flipping and makes fuzz read uncached memory;
unmeasured.

## Compiler: hot-loop code review — software audio mixing

Reviewed 2026-09-21: the inner loops of `pbench`'s `audio/mix`
(`benchmark/netbsd-bench/audio/mix.c`), which mix tracker voices in
software.  Its two loop orders produce the same output and differ in
how much of each voice-sample is bookkeeping.  `mix_by_frame` visits
every voice for every output frame: about 25 instructions per
voice-sample, most of them reloading voice state and a run-time
indexed accumulator.  `mix_by_voice` holds a voice's state in
registers across the block: 16, where the ISA allows about 14.

Measured on hardware, a table lookup in place of the sample-by-volume
multiply gains nothing.  The multiplier's `funit` stall (10–12% of
cycles) is traded for a dependent load's, and load stalls rise from
about 13% to about 20%.  The levers are the loop order and the causes
below, not the multiply.

| Cause | Per voice-sample | Entry |
|---|---|---|
| `MUL` copies a multiplicand that stays live — resolved | 1 in `mix_by_voice` | `676cb9fed5c1` |
| A conditional subtract pays for its zero arm — resolved | 2 in `mix_by_voice` | `c5e1dfadebc7` |
| A run-time indexed local rebuilds its frame address | 2 in `mix_by_frame` | "pointer-minus-constant and integer MMIO addresses miss their folds" |

### Smaller findings

- **A commutable `ADD` still copies into an argument register.**
  `pos += inc`, with both in argument registers and the sum returned,
  is `mov r11, r1; mov r1, r2; add r1, r11` where `add r1, r2` does
  it.  `ADD` is commutable, so this is not the `MUL` gap: commuting
  does not steer the tied operand to the one already in the result's
  physical register.  It is the same family as the subtracting
  decomposition's copy in "constant multiply, divide and remainder
  strength reduction".

  Two heuristics in `TwoAddressInstructionPass` cause it.  When the
  other operand dies at the instruction and the tied one does not, it
  commutes without asking where the result has to go, and a successful
  commute skips rescheduling the instruction below the tied operand's
  last use — which is how `mul64_minsize` in `mul-by-const.ll` gained
  a copy once `MUL` became commutable.  When both operands die,
  `isProfitableToCommute` prefers the operand already in the result's
  physical register, but it follows the result only through copies
  and two-address instructions; past anything else, a select's PHI
  included, it ties the operand defined more recently, which for
  arguments is the later register.

  Measured 2026-09-21 by a static scan of the kernel (`GENERIC.DEBUG`),
  `libc.so`, Doom's hot files and the `test-compiler` corpus, 1.5
  million instructions in all: `mov x, a; mov a, b; op a, x` with `x`
  dead, and `op c, a` followed by `mov a, c` with `c` dead, together
  cost at most 0.02–0.06% of instructions.  Not worth changing a pass
  every target shares.  Copies are 14–17% of all instructions, so
  they come from elsewhere.
- **The voice pointer is recomputed rather than stepped.**
  `mix_by_voice` forms `&v[k]` as `k * 24` by shift and add for each
  voice, spilling `k` to do it, where a pointer advanced by 24 needs
  neither.  Once per voice, not per sample.

Checked and not levers:
- Negative constants in a `CMP` are loaded into a register because
  `CMP Rd, #imm16` zero-extends its immediate and cannot hold them.
- The wrap test `(pos >> 16) >= len` costs a `MOV` and a `SHR` per
  sample.  Comparing `pos` with a precomputed `len << 16` removes
  both, but only the source can make that change: it needs
  `len < 65536`, which the compiler cannot know.

### 32.32 sample positions

A module's sample can be longer than a 16.16 position reaches — a MOD
sample runs to 131070 bytes, 65535 frames being the limit — so a
replayer's mixer needs a 32.32 position: the frame in the high word,
the fraction in the low.  `pos += inc` is then `ADD`/`ADC`, the sample
index is the high word as it stands, and the wrap test compares the
high word alone:

```c
int s = d[pos >> 32];                       /* uint64_t pos, inc */
pos += inc;
if ((uint32_t)(pos >> 32) >= end)
    pos -= (uint64_t)looplen << 32;
out[f] += s * vol;
```

At the ISA floor that is 14 instructions per voice-sample, one of them
a copy of the high word for the load address (there is no
register-plus-register addressing, and both the base and the position
stay live).  It compiles to 17:

- **A subtract with a zero low half keeps its low-half subtract.**
  `pos -= (uint64_t)k << 32` narrows to `G_USUBO lo, 0` feeding
  `G_USUBE hi, k`, and nothing folds the constant half, so every wrap
  runs `sub r4, 0; sbc lr, r9` where `sub lr, r9` does it.  Upstream's
  `match_subo_no_overflow` (a `G_USUBO` that cannot borrow becomes a
  `G_SUB` and a zero carry) and `adde_to_addo` (a `G_USUBE` whose carry
  is zero becomes a `G_USUBO`) cover it; neither is in
  `PenumbraPostLegalizerCombiner`.  1 per voice-sample.
- **The select of zero survives in the high half.**  The wrap is
  `pos - select(c, 0, k << 32)` at s64, which
  `penumbra_select_of_identity` leaves alone: it rewrites
  register-width values only (`c5e1dfadebc7`).  Split by the
  legalizer, the low select is a constant zero and folds, but the
  high one stays, so the common
  path runs `mov r9, zero` and the `SBC` of it.  2 per voice-sample.
  Running the rule after legalization would reach it, but also
  reshapes the legalizer's own selects, which cost `fshl_i64` two
  spills; the rule would need to recognise, before legalization, a
  64-bit select whose arms differ only in the high word.

### The 32.32 loops on hardware

`audio/mix` carries the loop above as `mix_wide`, and as
`mix_wide_hoisted`, which asks once per voice per block whether the
block can reach the loop end — frames left against `FRAMES` steps
rounded up to whole frames, in 32 bits — and when it cannot, mixes the
block with no wrap test.  Medians on penumbra2_5 at 37.5 MHz, in µs per
millisecond of 24 kHz audio, where 10 µs is 1% of the CPU:

| Voices | `mix_by_frame` | `mix_by_voice` | `mix_wide` | `mix_wide_hoisted` |
|---|---:|---:|---:|---:|
| 4 | 210 | 147 | 148 | 136 |
| 6 | 294 | 199 | 205 | 185 |
| 8 | 378 | 250 | 254 | 229 |

Per voice-sample, from the perfctr counts of the 8-voice runs less the
4-voice ones, which cancels the clearing and clamping done once per
block:

| Cycles per voice-sample | `mix_by_voice` | `mix_wide` | `mix_wide_hoisted` |
|---|---:|---:|---:|
| Instructions | 18.4 | 19.2 | 14.6 |
| `funit` stall | 10.0 | 10.0 | 10.0 |
| Store stall | 6.0 | 6.3 | 6.3 |
| Load stall | 4.4 | 4.3 | 4.5 |
| Flush, ifetch and hazard | 5.9 | 4.7 | 4.8 |
| Total | 44.7 | 44.5 | 40.2 |

The 32.32 position costs about a cycle per voice-sample over 16.16, its
extra instruction partly offset by fewer hazard stalls.  Hoisting the
wrap test takes a quarter of the instructions but a tenth of the
cycles: the multiply, store and load stalls, 21 of the remaining 40
cycles, do not move, and none of them is the compiler's.

- **The multiply holds EX for its whole iteration**, so no schedule
  hides it, and the table lookup above trades it for load stalls.
- **The accumulator is loaded and stored once per voice-sample**, and
  the store is write-through.  The first voice on each side can store
  its products rather than add them, which drops the `memset` and that
  voice's loads, and the last can clamp its sums into the output,
  which drops its stores.  Mixing two voices per pass halves the rest,
  but needs about 14 registers against the 12 there are, even with the
  position's high word kept as a pointer into the sample, which drops
  the base register and the `MOV`/`ADD` that form the load address.
  The spill reloads loop invariants, which hit L1, against saving a
  write-through store, so only a measurement can say whether it pays.

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
not an EEMBC-submitted score" wherever they are reported and in
benchmark output to stay clean of trademark misuse.

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
4. Extend the image overlay (or wherever the pbench bundling lives) to
   copy the 9 workload binaries plus their input data files (cjpeg,
   parser, zip) into `/usr/local/bin/coremark-pro/`.

**Prerequisites / risks:**
- libpthread is currently minimal stubs.  Need to verify single-
  context mode links cleanly without pulling in unimplemented
  thread primitives before committing to the build integration.
- FP-heavy workloads (`linear_alg`, `loops-all`, `nnet`, `radix2`)
  will be dominated by soft-float cost on the current CPU.  Numbers
  are still meaningful as a baseline but the suite gets a lot more
  interesting once a hardware FPU lands.
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

## ISA (data point): no rotate instruction costs SHA-256 ~30% of its ALU work

Every `ROTL(x,n)` lowers to the optimal-but-long `mov`+`shl`+`shr`+`or`
(4 ops; hand asm needs the same on a 2-address ISA).  SHA-256 executes
~480 rotates per 64-byte block (6 per round + 2 per schedule word) ≈
1900 instructions; MD5 64, SHA-1 ~160.  A Format-L `ROT Rd, #imm5`
through the existing barrel shifter would cut hash cores by roughly a
quarter to a third.  Recorded as a measurement, not a proposal — an ISA
change has its own bar (doc-first, conformance suite, all cores).

## Demos: mandelbrot's palette length forces a divide per cell

`pixel_color()` indexes `palette[iter % PALETTE_LEN]` with
`PALETTE_LEN` 30, so each cell pays two constant remainders
(foreground and background): two `DIVU` operations on penumbra1, and
on the default tuning a high-half multiply plus a shift/subtract
sequence each. Padding the table to 32 entries turns the index into
`and r1, 31`. Negligible in the terminal renderer — the escape loop
dwarfs it — but the cost scales with pixels rather than iterations, so
it grows in relative terms on a framebuffer, where shallow escape
counts are common.

## Benchmark: bare-metal startups use fixed low RAM

The harness startups (`benchmark/common/crt0.S`,
`benchmark/dhrystone/dhry_crt0.S`) copy their TLB-miss and interrupt
handlers to physical `0x1000` and pin that page, keep the timer's
accumulated ticks at `0x38` — reserved vector 14 — and pin page 0 so
the handlers reach it with the MMU on. `bench.c` and `tlbrange.c`
address both by constant, and `memtest` and `membench` fall back to
boot data at `0x40` when R1 is 0. None of it comes from the boot
data, and the two low pages are guaranteed to the ROM, not to what
it loads ([boot-protocol.md: ROM to Loaded
Program](../system/boot-protocol.md#rom-to-loaded-program)). It
works only because every machine so far has RAM there, and the tick
word breaks silently once vector 14 is assigned.

Open:

- Run the handler block in place from a page-aligned section of the
  image and pin that page; the handlers are PC-relative already.
- Move the tick word into the handler block beside the other handler
  state, and reach it by symbol from `bench.c`.
- Drop the page-0 pin and the `0x40` fallbacks.
- Take the memtest window from the `MEMORY` entry holding the image,
  not the first one.
