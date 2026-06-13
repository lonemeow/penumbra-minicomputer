# Penumbra -- TODO

Outstanding work and roadmap items, plus durable findings from
completed investigations.

## Kernel: block-device reads still go single-block

`pmci` handles CMD18/CMD25 multi-block natively (a per-block
FIFO-burst loop inside a single command envelope), but the block
device doesn't reach it:
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

## Kernel: IRQ-driven SPI completion deferred

The SPI FIFO data-phase plan originally included
`intr_establish_xname()` + `cv_wait` on XFER_DONE.  Implemented and
benchmarked: **3× slower**
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
- CMD18 multi-block — landed.  Each `pmci_burst` now covers up to
  N × 660 µs of wire time on the FS-mediated path (16 KB FS blocks →
  32-sector CMD18), so re-measuring IRQ-driven completion against the
  new envelope is worth a fresh attempt

## Hardware: FPU

Add a floating-point unit to the ALU. Currently using soft-float.

## Hardware: Penumbra/2 retire pulse counts faulting/trapping slots

BREAK is now a real EX trap (along with SYSCALL): it sets `fault_pending` in
EX, commits at WB, and vectors to `VEC_BREAK` through the same entry path as
every other synchronous exception — there is no hardware halt. The testbench
still detects program end by watching `o_retire_valid` + `o_retire_op_class`
for `OPC_BREAK`, which works because a trapping BREAK still leaves WB as a
valid slot the cycle it takes its trap.

Residual: `o_retire_valid` is currently `memwb_valid`, so it pulses for
faulting and trapping slots too (the alignment-fault load, the BREAK/SYSCALL
trap). As an insns-retired perfctr source that over-counts — a faulting
instruction is re-executed after the handler, not retired. When a gen2 perfctr
lands, gate the retired count by `~fault_pending` (the program-end testbench
use is unaffected — it keys on the op_class, not the count).

## Hardware: Penumbra/2 SPR access path — complete

WRSYS drives a real sysreg write at the EX drain-commit
(`make test-prog CORE=penumbra2 PROG=test_syswrite`). Wiring it surfaced a shared decode bug:
WRSYS *and* WRSPR encode their value register in the Rd field (as the assembler
emits and gen1 reads), but gen2 decode read it from Rs (i.e. R0). Both were
corrected to read the Rd field.

The SPR access path is fully wired, in backend-ordered pieces:

- **1a (done):** RDSPR/WRSPR EPC/ESR + RDSPR SR. EPC/ESR are SPR-file-backed
  (not regfile entries) and commit at WB like any register write — the value
  rides ALU_PASS to wb_value and the WB SPR strobe (previously tied off in the
  spine) drives the SPR-file write, gated to EPC/ESR. RDSPR reads them as
  operand B; the scoreboard downstream-writer predicate now counts `spr_we`.
  RDSPR SR composes {committed S/I, flag-bypassed NZCV} in EX. WRSPR is **not**
  a drain-commit — value SPRs are plain stores, unlike WRSYS. Tests:
  `isa/test_wrspr_epc`, `isa/test_rdspr_sr` (pass on ISS/gen1/gen2).
- **1b (done):** WRSPR/RDSPR USP — routes to the regfile R14 bank (entry 14,
  the `cross_bank` any-mode access the regmap already raises). RDSPR USP worked
  after 1a (ordinary operand-B regfile read); 1b ORed the USP SPR strobe into
  the regfile write enable. Test: `isa/test_wrspr_usp`.
- **1c (done):** WRSPR/RDSPR SCRn — `penumbra2_scratch_file` (4×32, one WB
  write port, one combinational ID read port) backs the four scratch SPRs,
  which fall outside the 16-entry regfile so they need storage of their own.
  The spine gates the write to the SCRn range and muxes the readback onto the
  operand-B SPR path; the ID stage routes SCRn reads through that path (USP
  stays on the regfile read). Test: `isa/test_scratch_sprs`.

With all value-SPR writes wired, gen2 advertises the `wrspr` capability
(`RUNNER_PROVIDES_penumbra2`), which pulled the previously-skipped `wrspr`-
tagged conformance tests into the gen2 suite. Two then failed — `test_priv`
(user-mode privilege trap) and `test_usp` (user-mode USP banking) — because
`penumbra2_core` hardwired `core_supervisor = 1'b1`, a bring-up stub deferred
"until the SR.S write path lands." With ESR/ERET restore real, that landed: the
spine exports the committed `SR.S` (`o_sr_s`, mirroring `o_sr_i`) and the core
sources its single `core_supervisor` site from it, so decode-time privilege
checks, both MMU user bits, regfile USP/SSP banking, and the fetch-side user
bit all see real privilege. Using the committed value is safe for every stage
because privilege changes only via drain-commit ops (exception entry / ERET),
which flush younger instructions. Full gen2 conformance suite green (72/72, 10
skipped on absent peripherals: bus/irq/timer/machid/perfctr/uart).

The privilege threading places the committed `SR.S` flop on the fetch/MMU path
(the gen2 fmax-sensitive cone). First `make timing` of the full
`machine_penumbra2` probe (`BOARD=ulx3s CORE=penumbra2 VARIANT=probe`) with the
change in: **30.18 MHz, PASS at the 25 MHz target** on ECP5-85F sg6 — within
the typical 29–30 MHz band, above the 27 MHz floor, so no fmax regression from
the SR.S threading. This is also the establishing operating point for the full
gen2 machine probe (the bare-core probe was ~55 MHz; the cache/MMU/arbiter
layers are the limiter, not the compute cone).

## Hardware: WRSPR-SR dropped ISA-wide (RESOLVED)

WRSPR SR is reserved (illegal) on every generation: the kernel changes
`SR.S`/`SR.I` via exception entry / ERET / EI / DI and NZCV via flag-writing
ALU ops, never a direct SR write — and a direct `SR.S` write is the only SPR
write that would need WRSYS-style context-synchronization (`SR.S` gates
fetch-translation privilege). Dropping it removes that complexity entirely;
the value SPRs need no context-sync. RDSPR SR stays (NetBSD spl/status reads
it).

gen2 made it illegal first (piece 1a); gen1 (`cpu_core` traps `WRSPR SR` to
`VEC_ILLEGAL` at dispatch; `datapath` no longer asserts `spr_sr_load` for SPR
3) and the ISS (`WRSPR SR` raises `VEC_ILLEGAL`) now match, and the ISA docs
mark the encoding reserved. `isa/test_spr_sr` (its only purpose was `WRSPR
SR`) is removed; `isa/test_rdspr_sr` covers `RDSPR SR` + EI/DI and
`isa/test_wrspr_sr_illegal` checks the trap, both on every generation. The
`wrspr` capability tag now means exactly "value-SPR writes work" everywhere —
which is what lets piece 1c advertise `wrspr` on gen2 once the SCRn scratch
file lands.

The gen2 *rationale* docs (`penumbra2/{design-decisions,hazard-model,
control-decode}.md`) were reframed to match: `WRSPR SR` is no longer listed as
a live drain-commit / flag producer (ERET, EI/DI, WRSYS remain). The drain-
commit and S/I-serialization analysis that `WRSPR SR` used to motivate is
preserved as the *justification for reserving the encoding* — a direct `SR.S`
write would feed the MMU and IF1 IRQ logic out of pipeline exactly as ERET's
does, so it would need the same serialization and fetch re-sync, and reserving
it removes that case rather than building it.

## Hardware: Penumbra/2 perfctr stall counters not modeled

`machine_penumbra2` implements the two architecturally-portable SYSDEV_CPU
performance counters — `CYCLES` (free-running) and `INSNS_RETIRED` (counts the
core's `o_insn_retired`, which includes the drain-commit ops that retire from
EX, not WB). The four gen1 stall counters — `STALL_FUNIT` / `STALL_IFETCH` /
`STALL_LOAD` / `STALL_STORE` (SYSDEV_CPU regs 7–10) — read 0 on gen2.

gen1's stall taxonomy is its microcoded sequencer's: one stall point with four
mutually-exclusive causes, so `cycles − Σstall` is exactly productive work. gen2
is pipelined and stalls differently — ID scoreboard RAW stalls, IF-side miss
stalls, MEM data-access busy, the divmul EX stall, and drain-commit drain
cycles — and these are neither single-point nor cleanly mutually exclusive (the
gen2 stall-propagation policy lets several stages stall in the same cycle).
Defining a gen2 stall breakdown — which events, where sampled, and whether to
keep them non-overlapping or accept overlap and report it — is the open work;
until then those registers read 0. The `perfctr` capability covers only the two
portable counters (what `isa/test_cpu_perfctr` checks) on every generation.

## Hardware: build/test restructure to the BOARD×CORE matrix

`doc/internals/build-system.md` defines the target structure: the
four-axis build matrix, the `hw/rtl/machine/` integration layer, the
`isa/` conformance split for test programs, and the
`make fpga BOARD=<board> CORE=<generation>` porcelain. The
test-program side of the migration is done: programs live in `isa/` /
`penumbra1/` / `penumbra2/` with `; RUNNER:` / `; REQUIRES:` tags,
`hw/tools/run-prog-tests.py` drives `make test CORE=<core>` /
`make test-prog`, and `tb_penumbra2_prog` is the generic gen2 runner.
The FPGA side is done too: sources compose from `SRC_CORE_<gen>` /
`SRC_FABRIC` / `SRC_BOARD_<board>`, board tops live under
`hw/rtl/fpga/<board>/` (`ulx3s_top` became `ulx3s_penumbra1_top`),
the Makefile registry (`FPGA_TOPS`, per-top `FPGA_SRC_<top>`,
`FPGA_ROM_TOPS`/`FPGA_UCODE_TOPS` for hex embedding) hard-errors on
unknown BOARD/CORE/VARIANT combinations, and `TOP=` remains the
low-level escape hatch. The gen2 bare-core timing probe
(`ulx3s_penumbra2_probe_top`, `make fpga/timing BOARD=ulx3s
CORE=penumbra2 VARIANT=probe`) is in: core + `unified_mem`, IRQs on
buttons, commit/retire XOR-folded onto the LEDs so synthesis keeps
the design — the synthesize-after-every-change workflow for gen2,
in place *before* the BRAM L1 lands (Decision 11's 4-way-vs-leaner
choice is gated on the IF2 tag-compare/way-mux path at synthesis).
First bare-core build: ~55 MHz achieved on ECP5-85F sg6, critical
path in ID decode toward the fault-vector register.

The gen2 machine assembly is done: `machine_penumbra2` under
`hw/rtl/machine/` honors the program-end contract, the runners build
`machine_penumbra2_sim` (machine + `unified_bus_mem`), the probe
variant wraps the machine — the IF2 tag-compare/way-mux path and the
L1↔L2 layer are in front of nextpnr; the first `make timing` run of
that configuration is pending — and the ISA-shaped gen2 programs
(smoke, branch, loadstore, fault, eret, syscall_trap) moved into
`isa/`. `test_intr` stays generation-pinned: its RUNNER tag selects
the IRQ-driving testbench, which is implementation-pinned by
definition. Remaining:

1. Opportunistic: extract `machine_penumbra1` from `machine_sim` /
   `ulx3s_penumbra1_top` so both wrappers share one integration
   (the sim-vs-FPGA congruence argument in the build-system doc).

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

## Compiler: share the hi-word compare in i64 three-way compares

The `G_SCMP`/`G_UCMP` lowering (`.lower()` →
`LegalizerHelper::lowerThreewayCompare()`) expands the i64 case
verbosely: 16 BBs — two s64 ICMPs each become a three-block hi/lo/eq
diamond, materialized as four `mov` selects.  A peephole that shares
the hi-word compare across the two ICMPs would shrink it.  Not on any
hot path (qsort's comparator is the i32 case); low priority.

## Compiler: aggregate-ABI rework — small structs in registers

`doc/system/abi.md` ("Argument Passing" / "Return Values") specifies
the slot-based aggregate convention: aggregates ≤ 4 bytes travel by
value in one slot, 5–8 bytes in two slots (register/stack straddle
allowed), > 8 bytes by reference to a caller-owned copy; returns
≤ 8 bytes come back in R1/R1:R2, larger ones via hidden sret pointer
with R1–R4 undefined at return.  The implementation is still
`DefaultABIInfo` (clang/lib/CodeGen/Targets/Penumbra.cpp) — every
aggregate indirect, every aggregate return sret — with three defects
the `test/compiler/penumbra-abi/` tests pin down:

- **Missing byval copy for register-slot aggregates.**
  `PenumbraCallLowering` passes the byval *pointer* in the register
  slot without materializing the copy, so C pass-by-value is
  silently pass-by-reference: a callee that writes its parameter
  mutates the caller's object, and a `.rodata`-sourced argument is
  exposed to callee writes.  (The variadic path was already fixed —
  `normalizeVarArgByVal` — the fixed-arg path was not.)
- **Stack-positioned byval corrupts trailing arguments.**  The
  caller memcpys the aggregate's bytes into the outgoing area but
  advances the slot offset by only one slot, so the next argument
  overwrites the aggregate's tail.  Silent data corruption for any
  by-value struct that lands in stack slots.
- **Outgoing stack stores expand byte-by-byte.**  Stack-slot stores
  are created with align-1 memory operands, so every stack argument
  (scalars included — e.g. a stack-passed `long long`) lowers to
  the unaligned byte-store expansion: eight `stb` + shifts instead
  of two `stw`.

Plan: replace `DefaultABIInfo` with a `PenumbraABIInfo` (RISC-V
ILP32 pattern): ≤ 4 bytes → Direct(i32), 5–8 → Direct([2 x i32]),
> 8 → indirect **non-byval** (clang materializes the copy in IR, so
the backend byval paths go dead and `normalizeVarArgByVal` can
retire); same classes for returns (`RetCC_Penumbra` already assigns
i32 to [R1, R2]).  Fix the stack-store alignment in
`PenumbraCallLowering`, make any remaining byval IR a hard error,
re-triage the `struct-ret-1.c` exclusion, and add lit shape tests
for the new convention.

Tracked tests: `test/compiler/penumbra-abi/` —
`abi-aggregate-{mutate,const,value}.c` fail until this lands;
`abi-aggregate-{return,varargs,boundary}.c` must stay green
across it.

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
`doc/llvm-divdf3-rounding-bug.md`.

Found along the way, still open: the bare 64×64→128 mulhi idiom
(`wideMultiply` reimplemented as a function returning only the high
half) is recognized by the IR optimizer into an i128 multiply that
crashes the legalizer — see "Compiler: G_ZEXT s128 from the mulhi
idiom fails to legalize" below.

## Compiler: G_ZEXT s128 from the mulhi idiom fails to legalize — RESOLVED

A function that computes the high 64 bits of a 64×64 product from
32-bit partial products and returns only that half (the classic
mulhi shape) gets idiom-recognized by AggressiveInstCombine's
ungated `foldMulHigh` into `zext i64 → i128; mul i128; lshr 64;
trunc`, which ICEd the legalizer.  C23 `_BitInt(128)` reached the
same gap directly.  (`compiler-rt`'s `wideMultiply` escapes
recognition because it returns both halves through out-params; the
32→64 flavor of the same transform is a win for us — a hand-written
`mulhi32` becomes a single MULU.)

Fixed in three parts, no actual i128 support needed (GISel narrows
iteratively s128 → s64 → s32 through the same paths s64 uses):
extension narrowing widened from `typeIs(s64)` to
`scalarWiderThan(32)`; extending loads with memory wider than
register width (`G_ZEXTLOAD s128 ← s64`, formed when a load feeds
the zext) lowered to plain load + extension; and the missing
aligned-pow-2 case added to the generic
`LegalizerHelper::lowerLoad` (it previously assumed lower() on an
extload always meant an unaligned split).  Test:
`test/CodeGen/Penumbra/i128-mulhi.ll`.

**Upstream-worthy:** the `lowerLoad` gap is generic — riscv32
GlobalISel ICEs on the identical `G_ZEXTLOAD s128 ← s64` from the
same IR (verified against an in-tree RISCV llc build), via the
sibling `// FIXME: Need to split the load.` hole in
`narrowScalar`'s extload case.  Full upstream-facing analysis,
reproducers, target survey, and proposed two-part patch:
`doc/llvm-gisel-wide-extload-legalization.md`.

## Compiler: PIC GOT anchor was position-dependent under code motion — RESOLVED

Dynamic NetBSD died at boot: init took a SIGBUS storm
(`Process (pid 1) got sig 10`, no progress) the instant it started.
Bisected to userland (old kernel + new userland reproduced; new
kernel + old userland booted), then to libc's `dl_iterate_phdr`
parsing the auxv — its switch cases stored auxv values through the
wrong GOT slots.

Root cause was the PIC GOT materialization
`LLI %got_pcrel_lo16(sym-8); LUI %got_pcrel_hi16(sym-4); ADD Rd, PC`,
where the `ADD`'s own PC is the relocation anchor and the **-8/-4
addends hard-coded the assumption that the ADD sits 8 bytes after the
LLI**.  Nothing enforced that adjacency.  The new GlobalISel
Localizer pass (`4c016efc4222`) clones `G_GLOBAL_VALUE` into each
using block, so switch cases storing through different globals ended
in identical `ADD/LDW/STW` tails — which BranchFolder tail-merged,
fusing the anchor `ADD`s of separate cases and leaving two of three
`LLI/LUI` pairs measuring against a PC 8 bytes off.  Each computed a
neighboring GOT slot; the dereferenced "address" was an unrelated
rodata pointer, and the store faulted.  Pre-Localizer, GISel CSE
materialized each global once, so duplicate triples never existed for
tail-merging to find — the latent bug had no trigger.

Fixed by making the sequence position-independent the way ARM32
(`PICADD` + `.LPC` labels) and RISC-V (`%pcrel_lo(.Lpcrel_hi)`) do:
the addend now names the anchor via label arithmetic
(`%got_pcrel_lo16(sym - .LPC0_0)` with `.LPC0_0:` on the ADD) instead
of a fixed constant.  Selection emits paired pseudos
(`PICLLI`/`PICLUI`/`PICADDi` carriers + `PICADDPC`/`PICMOVPC` anchors,
`isNotDuplicable`) sharing a per-function pclabel id; the AsmPrinter
places the label and lowers the label-difference operand, which the
assembler folds into the relocation addend (`A = P - Q`).  The linker
formula `S + A - P` is unchanged — no lld or relocation-table change.
Covers the GOT-global, TLS-GD, block-address, and jump-table PIC
paths.  Spec: `doc/system/abi.md` "PC-Anchored Relocation Pairs".
Tests: `test/CodeGen/Penumbra/pic-anchor-tail-merge.ll` (the
switch/tail-merge shape) and `test/MC/Penumbra/got-pcrel-label-anchor.s`
(addend folding across adjacent, branched, and same-section-local
layouts).  Validated by booting dynamic NetBSD to the single-user
shell (init/`/bin/sh`/`sysctl` all run, zero faults).

**Test-coverage gap — addressed:** `make test-compiler` runs
`penumbra-unknown-none` bare-metal, non-PIC, so it never exercised GOT
materialization — which is why a PIC-only miscompile reached a libc
this fundamental before anything caught it.  `make test-compiler-pic`
now runs a curated `-fPIC` set (`test/compiler/penumbra-pic/`) through
the same harness, linked static at a fixed address so lld resolves the
GOT at link time (no runtime relocator needed for the flat ISS image)
while the GOT-indirect codegen is still exercised.  The tests are
self-checking and were confirmed to fail when the anchor pairing is
deliberately broken (each catches a value landing in the wrong
GOT-loaded global / jump-table target).  Still open: a true dynamic
PIE leg (with `__tls_get_addr` and runtime relocation) would also cover
TLS-GD, which the static-link approach cannot reach.

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

~118 ms ≈ 2.95 M cycles for an init-sized address space (~2000 valid
pages, sparse over ~10 valid L1 slots).  That works out to ~1500
cycles per visited *valid* PTE — far more than the algorithm
describes (load+branch on invalid entries; PV-list walk + stat
decrement on valid ones).  Likely culprits: the per-L2 cached SDRAM
walk over 1024 entries each (256 cache lines × first-touch SDRAM
latency), and `pv_remove`'s SLIST traversal hitting cold lines.
Scratch-window cache (item 2 in "Quickest wins" below) would help
directly since adjacent L2 walks share PA.  Worth a closer look in
its own right after the bigger gap is closed.

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

## RESOLVED: "uncached MMIO STW 2.4× slower than LDW" was codegen, not hardware

The hardware write path is symmetric — RTL-sim microbenches put a
single uncached STW and LDW at the same ~4 cyc/op.  The observed 2.4×
came from `-fno-strict-aliasing` (kernel-wide, `Makefile.kern.inc`):
with TBAA off the compiler can't prove a volatile MMIO store doesn't
alias the in-memory `bus_space_handle_t sc_ioh`, so it reloaded the
handle before every write — 3 instructions per "STW" vs 1 per LDW,
3:1 ≈ the observed 2.4×.  Do not re-investigate as a hardware
write-path asymmetry.

`pmci_burst` now uses the canonical `bus_space_{write,read,set}_multi_1`
primitives (handle passed by value → base pinned in a register, immune
to the reload; committed as `9ddfcf4aa941`).  Perf-neutral by design —
per-byte cost is dominated by the MMIO store latency, and the idiomatic
m68k flat-MMIO ports reach the same rolled-loop conclusion — the value
is idiom and immunity to the reload.

## Hardware: SD read throughput is bottlenecked by pmci_burst byte-PIO

SD reads run at ~190 KB/s through FFS ("loading a binary takes
seconds").  Root-caused by instrumentation on ULX3S @ 25 MHz CPU /
12.5 MHz SCLK (`time dd if=/netbsd of=/dev/null bs=32k` → 4.2 MB in
~22 s).  Per 512-byte block, split into card-wait vs our processing:

```
token poll (waiting on the card):   ~700 cyc   ~28 µs    ~2%
pmci_burst (our byte-PIO):        ~31,600 cyc  ~1.26 ms   ~98%
```

`pmci_burst` is ~45× the card wait and ~95% of the kernel `sys` time.
The ~1.26 ms is: fill 512 idle bytes (~287 µs CPU PIO) + clock 512
while busy-polling XFER_DONE (~328 µs) + drain 512 (~287 µs CPU PIO)
+ per-block CONTROL save/restore + FIFO flush.  All single-byte MMIO,
all sequential, bus idle during fill/drain.

**Falsified — do NOT re-investigate these:**
- the SD card (token-wait is negligible, ~28 µs/block);
- the SPI clock (wire is ~26% of the burst; the validated 6.25→12.5
  MHz bump gave only ~6%);
- transaction granularity (reads ARE coalesced to 64 KB CMD18 =
  128 blocks/cmd = MAXPHYS; FFS/buffer-cache cluster correctly);
- IRQ/cache/TLB noise (the earlier guesses here — `sys` is dominated
  by the byte-PIO, not interrupts).

**Levers, all in our control:**
1. Word-wide FIFO MMIO (4 B/access) — cuts the ~574 µs fill+drain
   PIO ~4×.  Needs HW FIFO-width + a CAP-tiered driver (byte path
   stays the boot/discrete floor; word path gated on a CAP bit).
2. Auto-idle TX for reads — let the engine clock 0xFF itself instead
   of the CPU filling 512 idle bytes; drops the ~287 µs fill.
3. Cross-block streaming — drain block N while the engine clocks
   N+1.  The 512-deep FIFO (= one block) forces today's stop-start.

Realistic: ~2-3× on the pmci term, ~1.5-2× end-to-end (the kernel
I/O stack + per-command framing is the other ~half of the run).  A
real SPI read-path + HW project, not a quick fix; gen2-adjacent.

## Hardware: gen1 CPU fmax — critical-path findings

gen1 targets 25 MHz on the ULX3S (ECP5-85F sg6). It is the single-cycle
microcoded core, so one combinational cone spans the whole instruction:
micro-ROM read → control decode → register-address resolution →
register-file read → ALU → flag update. There is no slack cycle to hide
any of it in — that cone is the structural fmax floor for this
generation.

### RDSPR-USP read-address override removed — DONE (`82d758cb4689`)

The register read-address path carried a per-instruction mux that forced
the A address to R14 at runtime, solely so RDSPR USP could read the
banked user stack pointer. Its select came from the late micro-ROM
`a_src` field, so every instruction's read address waited on a decode
that mattered for one rare op. Moving the R14 selection into the `rdspr`
micro-word (`reg_a=R14`) makes the address a static literal: the
register file already maps R14 to the banked SP and `cross_bank` still
picks USP vs SSP, and the A-mux ignores `reg_a` for the other SPR
sources, so one literal address is correct for every SPR variant. A
`datapath.sv` assertion guards the microcode-supplied-address contract.

Measured on ULX3S, single synth run each:

```
  gen1 CPU fmax:  25.37 MHz → 27.11 MHz   (constraint 25 MHz)
  critical path:  39.42 ns  → 36.89 ns
```

### The bottleneck relocated — read the path shape, not the number

The durable lesson. Before, the critical path was the compute cone,
rooted at the micro-ROM and ending at the `flag_z` register data input.
After, it is a different net entirely:

```
  PC reg → MMU → TLB pinned-match (~11 ns) → TLB main → D$ cacheable
         → I$ tag compare (~4 ns) → read-miss → bus arbiter
         → sequencer next_upc → flag write-enable (flag_z clock-enable)
```

i.e. the instruction-fetch / address-translation / hit-miss control
path. The compute cone dropped clean below a path that had been sitting
just under it the whole time. Two consequences:

- It proves the change was real signal, not placement noise. Noise
  jitters the number on the *same* path; it does not relocate the
  startpoint or move the limiter to another subsystem. A changed path
  shape is the honest test for "did this optimization do work."
- Further compute-cone work (the 32-bit `flag_z` zero-detect reduction,
  the register file, the ALU) now buys **zero** fmax — none of it is on
  the limiting path. The next gen1 lever would be the TLB pinned-match
  and I-cache tag compare, which is exactly what the gen2 pipelined
  fetch restructures, so it is not worth chasing in gen1.

Caveat on the timing tool: the nextpnr per-module rollup attributes
fused post-flatten LUTs by net-name prefix, not by logical dataflow. The
two register-file read ports (A/B) are independent parallel reads of
replicated DPRAM banks, but because they feed a common ALU sink the
rollup can make them *look* serial. Trust the hop trace and the
start/end points, not the module labels.

### RTL-cleanup-for-fmax techniques (ref: openiphub UberDDR3 post)

[Pushing UberDDR3 frequency through RTL
cleanup](https://www.openiphub.com/post/pushing-uberddr3-frequency-through-rtl-cleanup-post-18)
walks a DDR3 controller from 82 → 132 MHz. The transferable techniques,
and how they land here:

- **Register control decisions one cycle early; reuse the registered
  decision instead of recomputing it combinationally.** Their biggest
  wins. For a single-cycle microcoded core this *is* adding a decode
  stage — there is no earlier cycle to register into — which is the
  gen2 premise, not a gen1 tweak.
- **Don't gate the common path with rare-command logic.** Exactly what
  the RDSPR-USP override removal did: a rare op sat on every
  instruction's read address.
- **Flatten deep control trees; right-size register/counter widths;
  separate combinational from sequential.** Cheap, local, low-risk — the
  right tools for buying placement-noise margin rather than raising the
  ceiling.

The headline: gen1's ceiling is structural (one cycle does fetch +
decode + read + execute + writeback), so the durable fmax lever is the
gen2 pipeline. Within gen1, prefer small depth-shaving wins that buy
safety margin over the 25 MHz target rather than campaigns to raise it.

## Hardware: Penumbra/2 machine assembly — done; findings + residue

The gen2 machine is assembled (`hw/rtl/machine/machine_penumbra2.sv`):
both VIPT L1s, the transactional I/D arbiter, the fill sequencer, and
the shared L2 sit behind the core's fetch/dmem front ports; IF2 and
MEM carry their miss-stall handshakes (level-held request, busy-drop
completion, a one-entry IF2 skid for completion-under-ID-stall, and a
redirect-target launch held out of an in-flight I-transaction); the
identity-mapping assertions are gone and the non-identity tests
(`test_tlb_remap`, the COW set) pass; SYSDEV_L1_DCACHE/ICACHE are
wired through the WRSYS commit and RDSYS sideband. The conformance
suite runs end-to-end on `machine_penumbra2_sim`.

Two durable findings from the integration, both instances of one
invariant — *a presented transaction never vanishes mid-flight*:

- **The request/busy mesh must be structurally acyclic.** Every
  memory layer's busy combinationally follows its consumer's request
  (the sync-bus contract), so no request — and nothing a request is
  gated on, like the front-end flush — may combinationally depend on
  any busy, or Verilator (rightly) reports the mesh as a
  combinational cycle. This is why the vector-fetch redirect is a
  registered pulse (captured handler word, one cycle after the read
  completes, `o_active` covering the redirect cycle) and why
  `o_fetch_re` is a pure function of FSM state.
- **Pass-through transactions need an owner, like fills have.** A
  flushed fetch whose pass-through read the arbiter had already
  granted-and-locked would withdraw the back-side request
  mid-transaction and wedge the arbiter's completion condition
  forever (line fills were immune — the cache FSM owns them; the
  window needs a D-access parking the fetch first, which is why plain
  branch tests never hit it). The L1's S_PT state captures an
  accepted-but-busy pass-through read and holds it to completion,
  serving a withdrawn consumer into the void. Front-side consumers
  may therefore walk away from *reads*; writes have no kill source.

Remaining in the gen2 machine, roughly in order:

- First `make timing BOARD=ulx3s CORE=penumbra2 VARIANT=probe` run of
  the machine-shaped probe (Decision 11's 4-way-vs-2-way L1 choice is
  gated on the IF2 tag-compare/way-mux path it exposes; 2-way is a
  parameter fallback).
- A bus-fault return path through L2/sequencer/arbiter/L1 — gen1
  wires no-device-at-address into the core; the gen2 path carries no
  fault signal yet (blocks `test_bus_fault*` and, later, bus
  autoconfig RAM probing).
- The remaining capability gaps vs gen1's runner: wrspr (the SPR
  write port milestone above), timer/uart devices on the external
  bus, machid/busctl via a machine sysreg expansion port.

The shared L2 stays untouched; its read-pipeline initiation interval
is the fill-penalty floor, characterised by
`test_back_to_back_read_throughput` in `hw/sim/tb_l2_cache.cpp`.
Deferred fill-speed directions (L2 initiation-interval decouple, wide
datapath, write buffer) are gated on gen2 bottleneck measurements.

L2-side residue of the valid-bit storage design (the L1 half — flop
valids, single-cycle flush — is implemented in `cache_bram_vipt.sv`):
the L2 is the opposite corner — large valid array, no runtime-flush
caller (PIPT) — so its reset clear can stay a sequenced multi-cycle
walk; that, plus uniform hold-core-until-`init_done` bring-up, is
what a multi-cycle reset sequencer is for (reset axis only,
orthogonal to flush; scoped with the L2 rework that de-hacks the
post-reset-walk-while-live).

## Hardware: L2 phase 2 — write-back / write-allocate

The headline remaining cache optimization, and the highest-impact
item left in this section.  Today every L1-D store under WT-WnA
still pays a full SDRAM round-trip — Dhrystone shows 1.22M writes
per run, each ~20-30 cycles, ~25-37M cycles of write traffic on a
45M-cycle run.  Phase 2 absorbs writes into L2 entirely until
eviction.

Upper-bound win (from current measurements): the 841K Dhrystone
write hits become zero-bus-traffic dirty marks (~25M cycles saved
on Dhrystone alone, before counting kernel workload gains).  WA
additionally captures the 380K cold writes at the cost of one
read-fill per first-touch line — net positive when intra-line
write locality is high (stack frames, page zeroing).  pbench data
shows the workloads WT-WnA didn't help (`fork_exit`, large
`memset`) are precisely the ones that have heavy cold-write
traffic, which WB+WA targets.

Implementation outline (full design in
`doc/internals/l2-cache.md` § Write Policy / Phasing):

- Per-line dirty bit (4096 bits at 64 KB / 16 B lines)
- Stage-1 write hit sets dirty (no bus traffic)
- Eviction of dirty line writes back before installing new line
- `FLUSH_LINE` / `FLUSH_ALL` sysreg ops for software-managed
  coherence (DMA-out preparation, etc.)
- WA path: write miss allocates the line, then dirty-marks it
- Optional phase 3: 1-deep writeback buffer to overlap eviction
  drain with the new line fill

Estimated complexity: ~250-300 lines of RTL + dirty bit storage,
plus FLUSH ops and the eviction state machine.  Test surface adds
write-hit-no-bus-traffic, dirty-eviction-writes-back, and
flush-then-evict-is-clean assertions.

Forward-looking note on perfctrs: the reserved slots at
`SYSDEV_*CACHE` regs 14-15 will likely become `LINE_FILLS` and
`WRITEBACKS` post-phase-2, directly answering "did WA's read-fill
cost pay back via intra-line locality."

gen2 note: the L1↔L2 interface is designed to not foreclose this — it
stays neutral about L2's write policy (the arbiter is L1-facing; WB/WA
are L2-internal and L2↔memory concerns). See the write-policy-neutrality
section of `doc/internals/penumbra2/memory-interface.md`.

## Hardware: L2 write buffer (latency-hiding alternative)

A smaller, orthogonal option to WB phase 2: a 1-4 word write
buffer at the L2's downstream port.  Hides write latency (CPU
stops stalling on the SDRAM round-trip) without reducing SDRAM
write traffic.

For the current single-master system (no DMA contending for SDRAM
bandwidth) latency hiding and traffic elimination are nearly
equivalent in CPU-visible cost.  The write buffer is materially
cheaper to implement (~30-50 lines vs ~250-300 for WB+WA), and
the snoop complexity normally associated with write buffers is
handled for free by WT-WnA's byte-en update — L2's cached copy is
fresh as soon as the buffer accepts the write, so subsequent
L1 read-misses to that line return correct data from L2's cache
without needing to consult the buffer.

When to pick this over phase 2:

- Lower complexity budget / faster-landing intermediate
- Single-master system stays single-master
- Want to confirm the CPU-side latency-hiding hypothesis cleanly
  before committing to the larger WB engineering effort

When phase 2 is required instead:

- Future DMA needs SDRAM bandwidth (writes from CPU starve other
  masters)
- Sustained write rate exceeds SDRAM drain rate (buffer fills up,
  CPU stalls again)
- Power/thermal sensitivity to SDRAM utilisation

They compose: WB phase 2 + write buffer (= phase 3 in the L2 doc)
is the maximally aggressive design.

gen2 note: `doc/internals/penumbra2/memory-interface.md` keeps writes
a distinct single-beat shape so a buffer drops in cleanly, and leans
its placement *after* L2 for exactly the byte-en-update reason above;
it lists the buffer as a deferred fill-speed direction gated on gen2
measurements.

## Hardware: read-miss fills pay 1-2 unnecessary cycles re-classifying

Both L1 (`cache_vipt.sv`) and L2 (`l2_cache.sv`) caches complete a
read miss by transitioning `S_FILL → S_IDLE` and then waiting for the
CPU's still-asserted request to be re-latched and re-classified as a
hit on the just-installed line.  That re-classification costs 1 extra
cycle at L1 (single-stage classify) and 2 extra cycles at L2
(stage 0 + stage 1) on every read miss.

Logically the cache could short-circuit this: at the cycle the last
word of the fill arrives, all words of the line are accessible
(those captured previously from BRAM, the last one from
`i_mem_rdata`), and the CPU's requested word offset is known from the
stalled address.  Driving `o_rdata` from the appropriate source and
`o_busy=0` on that cycle exits the CPU's STALL one or two cycles
earlier.

Two layered variants on the same idea:

1. **Skip re-classification.**  Serve the requested word on the
   last-word-captured cycle.  Saves 1-2 cycles per miss regardless
   of which word in the line was requested.
2. **Early restart.**  Serve the requested word the moment *it*
   arrives during `S_FILL` (not waiting for the rest of the line —
   the remaining words finish into BRAM in background).  Combined
   with critical-word-first burst ordering (already listed as
   Level 3 in `doc/internals/sdram-optimization.md`), the CPU sees
   just the bus round-trip latency rather than the full burst
   length.

Per-cycle savings are modest against the ~30-cycle SDRAM round-trip
on the single-issue microcoded CPU we run today.  For the planned
pipelined Penumbra/2 each saved cycle is one fewer dependent-chain
stall, and the higher target clock makes each cycle more expensive
in wall time, so the optimization is higher-leverage there than the
bare cycle counts suggest now.

**Update — Penumbra/2 design settled.** gen2's L1↔L2 interface takes
an *atomic full-line* fill (Decision 14 in
`doc/internals/penumbra2/design-decisions.md`; spec in
`doc/internals/penumbra2/memory-interface.md`), which splits the two
variants above by gen2-compatibility. **Variant 1 (skip
re-classification)** keeps the whole line present, so it is compatible
— gen2 may take it. **Variant 2 (early restart)** is deferred to the
non-blocking phase: serving a word before the line completes punctures
the transaction-atomicity invariant the gen2 arbiter and fill path
rely on (it forces per-word presence, hit-under-fill stalls, and
fill-vs-store / fill-vs-flush handling). So for gen2, early restart is
explicitly not the win the preceding paragraph assumed.

The L1-side win is more impactful than the raw cycle counts imply,
in a way that scales with L2 hit rate.  An L1 miss that hits in L2
costs roughly `4 words × ~3 cycles` ≈ 12 cycles of fill + 1 cycle
of re-classification — so the re-class is around 8% of the miss
cost on that path.  An L1 miss that *also* misses L2 (full SDRAM
round-trip) costs ~120+ cycles, and the same 1 cycle is less than
1% of it.  Hot kernel paths sit on the L1→L2-hit path
(syscall I-fetch is the canonical case where L2 lands ~2× wins),
so the bulk of L1 misses in real workloads are exactly where this
optimization saves the largest fraction.

Side effect on the perfctr work landed alongside this entry: both
L1's `state_was_fill` guard and L2's `fill_reserve_pending` flop
exist specifically because the post-fill re-serve generates a
phantom hit event today.  Once early-restart lands the re-serve
goes away on both caches and both predicates collapse to
`<valid> && <s1_re> && hit` with no suppression.

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

## Compiler: signed sub-word loads through PHIs

Mirror of the zext-load promote rule for the sign-extending case.

Confirmed hot 2026-06-09: Dhrystone's main loop carries ~6
`shl 24; sar 24` sext-of-char chains (`Ch_Index`/`Ch_1_Glob`
compares), including one site that sign-extends *both* operands of an
eq compare — for eq/ne any consistent extension works, so a
consistent-extension combine could drop both without LDBS selection.
See the Dhrystone hot-path entry below for the surrounding analysis.

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

## Compiler: selectAddSubCarry misses the immediate fold

`selectAddSubCarry` hand-emits the register `ADD`/`SUB` for the i64
carry chain, so an i64 add/sub by a small constant materializes it
with an LLI instead of `ADDi`/`SUBi`.  Routing those through an
`emitFoldableALU`-style helper (sharing the uimm16 fold logic with
`emitCompare`, the way `emitSelectCC` does for select immediates)
would close it.

## Compiler: redundant extension of comparison results

The branchless `icmp`-to-value path (`selectICmpToValue`) already
produces a clean 0/1 in a GPR, but the extension consumers re-process
it from scratch:

- `zext i1` -> `selectZExt` emits `ANDi #1`, a no-op on an already-0/1
  value.  The inverted predicates (`ult`, `ugt`, `ne`) compound it:
  `emitCarryToValue`'s own `ANDi #1` (masking the SBC's 0/0xFFFFFFFF down
  to 0/1) is then followed by the zext's `ANDi #1`, masking twice.
- `sext i1` -> `selectSExt` emits `SHLi #31` + `SARi #31` to turn 0/1
  into 0/-1, when the flag read could have produced the 0/-1 mask
  directly (a single SBC of zeros after the compare yields -(NOT C);
  pick the polarity to land 0/-1) and skip the two shifts.

The zext half is done: `selectZExt` emits a `COPY` (which the register
coalescer folds away) when the `G_ZEXT` source is a `G_ICMP`, dropping the
redundant mask.  Code in
`llvm/llvm/lib/Target/Penumbra/GISel/PenumbraInstructionSelector.cpp`.

Two pieces remain, both measured low-value 2026-06-06:

- **zext through a `G_PHI`.**  The `COPY` fold only fires when the icmp is
  the *direct* def, so `zext(phi(icmp, icmp))` still double-masks — 12
  cold kernel sites (all `and r,1; and r,1`).  Extending `selectZExt` to
  walk the PHI would have to prove every incoming value is 0/1, which is
  awkward at selection.  The idiomatic fix is instead a post-ISel
  `MachineFunctionPass` collapsing `ANDi x,C1; ANDi x,C2 -> ANDi x,C1&C2`
  (the redundancy only exists after selection, so a generic combiner
  cannot see it).  That is the same vehicle `RISCVOptWInstrs` (redundant
  `sext.w`) and `X86FixupSetCC` (zext-of-setcc) use for
  selection-introduced redundancy — but those target pervasive patterns;
  a pass for 12 cold instructions is not justified.  Reach for it if the
  pattern ever becomes frequent.

- **sext.**  `selectSExt` would have to *consume* the icmp (selection is
  bottom-up, and with no single-instruction negate, converting a 0/1
  afterward costs the same `SHLi;SARi` it would replace) and pick the
  carry polarity per predicate to land 0/-1 — a silent-miscompile
  surface.  Measured reach: 1 site in Dhrystone, 87 cold sites in the
  NetBSD kernel (0.23% of comparisons, ~0.02% of code).  Low leverage for
  the complexity; deferred.

Do **not** try to drive this from `setBooleanContents(ZeroOrOne)`.
Measured 2026-06-06: the redundant masks are emitted at *selection*,
after the combiners run, so boolean content cannot fold them.  Worse,
declaring `ZeroOrOne` flips `LegalizerHelper::lowerThreewayCompare` from
the SELECT-chain expansion to the arithmetic `zext(gt) - zext(lt)` form,
regressing the qsort i32 comparator (`scmp_i32` 10→15, `ucmp_i32` 10→13
instructions) while only shrinking the rarely-used i64 three-way compares
(−4 each); no other CodeGen test moves.  Only revisit `ZeroOrOne`
alongside a custom `G_SCMP`/`G_UCMP` lowering that keeps the i32
SELECT-chain.

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
- **Pre-RA MachineScheduler** (`enableMachineScheduler`, default
  false): with GlobalISel there is *no* scheduling at all without it —
  instruction order is IR order.  GenericScheduler in in-order mode
  (`MicroOpBufferSize=0`) prioritizes register pressure, which is the
  spill lever.  Flipping it also flips `enableJoinGlobalCopies`
  (better cross-block copy coalescing), which defaults to
  `enableMachineScheduler()`.  Next candidate after Localizer settles.
- **Tail calls**: `PenumbraCallLowering.cpp` hardcodes
  `Info.IsTailCall = false` (`// TODO: tail calls`).  Wrapper-heavy
  kernel code pays a full frame per hop.  Medium GISel project
  (lowerTailCall + branch-instead-of-BL emission).
- **`enableSpillageCopyElimination`** (MachineCopyPropagation
  extension, X86 enables): cheap flip + measure.
- **MachineOutliner / RISC-V-style save-restore millicode**: nothing
  implemented (`getOutliningCandidateInfo` etc.).  Code-size tools —
  attractive under the 1 KB I$ regime (21% of hot text is spill code;
  identical prologue/epilogue sequences outline well), but each
  outlined call costs BL+JMP at runtime.  Bigger project; consider
  RISC-V's `-msave-restore` shape (dedicated prologue/epilogue
  routines) rather than the general outliner first.
- **PostRA scheduler** (`Penumbra1Model` doesn't set
  `PostRAScheduler`): pointless for gen1 (single-cycle, no pipeline);
  belongs in `Penumbra2Model` with the gen2 subtarget split (hide
  load-use and divmul latency).
- **MachineCombiner** (`getMachineCombinerPatterns` + sched model):
  reassociation for ILP — little to gain at IssueWidth=1; revisit
  with gen2.
- Not applicable: EarlyIfConversion (no predicated execution),
  MachinePipeliner (needs deep sched model, ILP machine),
  `enableSubRegLiveness` (no subregisters), GISel LoadStoreOpt
  (no wider load/store ops to merge into).

## Compiler: enable shrink-wrapping (spill-density lever, part 1)

LLVM's ShrinkWrap pass (runs post-RA, just before PrologEpilogInserter)
computes a save point / restore point pair in `MachineFrameInfo` so PEI
places the *whole* prologue (CSR saves + stack adjust) at the earliest
block dominating all frame uses instead of the entry block — early-exit
paths then run zero frame code.  It is gated per-target on
`TargetFrameLowering::enableShrinkWrapping` (default false); Penumbra
does not override it, so the pass currently no-ops.  Every major target
(AArch64, ARM, LoongArch, Mips, PowerPC, RISC-V, X86) opts in.

Measured 2026-06-09 with the global force flag
(`-mllvm -enable-shrink-wrap=true`):

- **Mechanically works on Penumbra**: for an `if (!p) return -1;` +
  calls function whose CSR-live values are born after the branch, the
  prologue sinks below the early exit as expected.
- **Correct end-to-end**: `make test-compiler
  OPT="-O2 -mllvm -enable-shrink-wrap=true"` passes 1607/1607 on ISS.
- **Dhrystone is unaffected** (zero diff) — its hot functions have no
  early-exit shape; the payoff target is kernel code (error paths,
  lock fast paths).  Measure with pbench syscall benches, not DMIPS.

Enablement plan:
1. Override `enableShrinkWrapping` → true in `PenumbraFrameLowering`,
   **gated on the frame fitting the SUBi immediate form**: `adjustSP`'s
   large-frame path clobbers scratch R11 on the documented assumption
   that R11 is dead at function entry/exit
   (`PenumbraFrameLowering.cpp`), which a mid-function save point
   violates.  Either keep large frames un-shrink-wrapped (they are
   rare) or teach `adjustSP` to scavenge.
2. Lit test asserting the early-exit prologue sink.
3. Known reach limit — **regalloc pins the save point for argument
   values**: an argument live across calls gets its `COPY` into a
   callee-saved register placed in the *entry* block, which ShrinkWrap
   counts as a frame use, so such functions never shrink-wrap.  The
   standard counter is `TargetRegisterInfo::getCSRFirstUseCost`
   (AArch64 returns 5), which makes greedy RA pre-split cold CSR
   first-uses — but forcing `-regalloc-csr-first-time-cost` did *not*
   split the argument shape on Penumbra (the region-split candidate
   search found nothing under CSRCost).  Needs its own investigation;
   without it, shrink-wrapping only catches functions whose CSR
   pressure starts after the early exit.

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
   chain, see the branch-cost entry below).  Kernel hot paths may
   differ — measure there before investing.
5. **Signed sub-word loads confirmed hot**: ~6 `shl 24; sar 24`
   sext-of-char chains in the loop body (see that entry), including a
   double-sext feeding an eq compare where *no* extension is needed —
   a consistent-extension eq/ne combine would drop both.
6. Minor: `Proc_1`–`Proc_5`/`Func_3` are fully inlined but their
   out-of-line bodies stay linked (extern linkage) — ~500 B of dead
   text between hot functions.  Harmless to the cache (never fetched)
   but skews naive footprint reading; `--gc-sections` +
   `-ffunction-sections` would drop them.

## Compiler: no branch-cost model — branch-avoidance may be over-eager

Penumbra sets none of the branch/select cost knobs
(`predictableSelectIsExpensive`, TTI `getCFInstrCost`, etc.), so the
generic GISel combiners assume branchless code is always cheaper and
fire unconditionally.  The clearest case: `select(cmp, c1, c2)` with
small constants is rewritten (by `select_constant_cmp`/`match_selects`
in `all_combines`) into `sext(cmp) + c2`-style arithmetic, even though
Penumbra's own `selectSelect` would otherwise emit a CMP+Bcc branch.

The branchless form is not obviously a win here: it must materialize the
comparison into a 0/1 GPR and then extend it, whereas a branch lets the
CMP feed Bcc directly and never builds the value.  For Dhrystone's
`Proc_6`, `*ref = (x==2) ? Ident_3 : Ident_4` becomes ~8 instructions
(`sub; cmp; mov; adc; shl; sar; add` + the seed mov) versus ~4 for a
`lli; cmp; bne; lli` branch.  Whether that is faster depends on the
branch cost: penumbra1 (shallow, no mispredict penalty) likely favors
the branch; penumbra2 (no predictor, resolves in EX) makes taken
branches costly and narrows the gap.

This is a measure-on-hardware question, not a guess.  Action: benchmark
representative code (Dhrystone, kernel hot paths) with the select-of-
constants combine on vs off, then set the cost knobs (and/or gate the
combine) to match the measured branch cost.  Note this is orthogonal to
the branchless `icmp`-to-*value* path, which is a clear win over the old
`SELECT_CC` form regardless (fewer instructions, no branch, no BB
split); only the select-of-constants rewrite is in question.

Reach check 2026-06-09: the Proc_6 chain is the *only* such site in
Dhrystone's hot path (see the Dhrystone hot-path entry above), so
Dhrystone cannot resolve this question either way — measure on kernel
hot paths instead.

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

## Compiler: gen1/gen2 subtarget split — tuning knobs for divergent micro-arch

penumbra1 and penumbra2 are the **same ISA** running the **same binaries**,
but their micro-architectures pull optimization tradeoffs in opposite
directions: gen1 is single-issue/microcoded with no branch penalty but a
tiny L1 I-cache (code density matters), while gen2 is pipelined and resolves
branches in EX with no predictor (taken branches cost cycles, but the cache
is larger). The backend needs to know which it targets and skew cost-driven
choices accordingly — without ever changing what code is *legal*, so one
object file still runs on both.

**Today.** There is exactly one processor (`Penumbra1Model` +
`def : ProcessorModel<"penumbra1", Penumbra1Model, []>`, `Penumbra.td:41`),
no subtarget features, and `PenumbraTargetMachine.cpp:238,242` defaults an
empty `-mcpu` to `"penumbra1"`. The Subtarget passes CPU as both CPU and
TuneCPU (`PenumbraSubtarget.cpp:27`), so `-mtune=` is already plumbed. The
header of `Penumbra.td` already states the intended split: "This file
defines the ISA — not any specific implementation. CPU models … are defined
separately per implementation." Adding a second processor is the path the
target was set up for.

### The invariant that constrains the whole design

The gen knobs are **tuning only**. They may change *which of several correct
lowerings* the backend picks (branch vs. branchless, inline vs. libcall,
schedule, unroll factor) but must never change instruction legality or the
emitted instruction set. Legalization stays identical for both gens; gen
features are consumed only in cost models, combiner gating, and scheduling.
This is what guarantees a `.o` built for one gen still executes correctly on
the other — gen choice is a performance hint, not an ABI or ISA fork.
Concretely: never branch on a gen feature inside `PenumbraLegalizerInfo`'s
legality rules.

### Mechanism — three layers, all idiomatic LLVM

1. **Subtarget features named by micro-arch *property*, not CPU identity.**
   Declare in `Penumbra.td` (or a new `PenumbraFeatures.td`):
   - `FeatureBranchPenalty` — "taken branches cost pipeline cycles" (gen2).
   - `FeatureSmallICache` — "I-cache is small; favor code density over
     speculative code expansion" (gen1).
   Property-naming (the AArch64/RISC-V `Tune*`/`Feature*` idiom) keeps the
   codegen sites reading `ST.hasBranchPenalty()` — a capability question —
   instead of `CPU == penumbra2`, so a future gen3 sharing a property reuses
   the same path. Start with the two that have concrete consumers; grow on
   demand (`FeatureSlowTakenBranch`, etc.).

2. **A `SchedMachineModel` per processor.** Add `Penumbra2Model` beside
   `Penumbra1Model`. gen2 sets a non-zero taken-branch / `MispredictPenalty`
   cost and any latency differences; gen1 keeps today's numbers. The generic
   MachineScheduler and several cost heuristics read this automatically.

3. **ProcessorModel definitions wiring features + sched model:**
   ```
   def : ProcessorModel<"penumbra1", Penumbra1Model, [FeatureSmallICache]>;
   def : ProcessorModel<"penumbra2", Penumbra2Model, [FeatureBranchPenalty]>;
   ```
   TableGen then auto-generates `bool hasBranchPenalty()` /
   `hasSmallICache()` predicate accessors on `PenumbraSubtarget`.

### Where the knobs are read (the first two consumers already have entries)

- **Branch-vs-branchless** — the "Compiler: no branch-cost model" entry
  above is exactly this fork: the `select_constant_cmp` combine and the
  missing TTI cost hooks. Wire `getCFInstrCost` /
  `predictableSelectIsExpensive` / `getCmpSelInstrCost` in
  `PenumbraTargetTransformInfo.h` (it already holds the subtarget) to return
  branch-favoring costs when `!hasBranchPenalty()` and branchless-favoring
  costs when `hasBranchPenalty()`, and/or gate the combine on the feature.
- **Code density** — the "Compiler: inline small constant-size memcpy"
  entry: gate the inline-expansion thresholds (`MaxStoresPerMem*`, whether
  to inline at all) on `!hasSmallICache()`, so gen1's tiny L1 keeps the
  compact `bl memcpy` while gen2 inlines. The same flag could later skew
  loop unrolling and the 16-bit-jump-table choice.

### Selection and defaults

- `-mcpu=penumbra1|penumbra2` selects the processor; because the *arch*
  feature set is identical (shared ISA), this is purely a tune selection and
  is always safe. `-mtune=` already works via the TuneCPU plumbing.
- The empty-`-mcpu` default lives in one place
  (`PenumbraTargetMachine.cpp:238,242`). Keep `penumbra1` as the default
  while gen1 is the shipping silicon; flip to `penumbra2` there — and nowhere
  else — when gen2 becomes the primary target. Treat that line as the single
  source of truth for the default-gen policy.
- Clang: confirm `-mcpu` reaches cc1 `-target-cpu` for both Penumbra
  toolchains (bare-metal `PenumbraToolChain` and the NetBSD path); the build
  systems (`hw/rom`, `build.sh`) then pass `-mcpu=penumbra2` once gen2 is
  real.

### Known limitation (not blocking)

`PenumbraTargetMachine` builds a single `Subtarget` at construction
(`PenumbraTargetMachine.cpp:242`) rather than the cached per-function
`SubtargetMap` pattern, so gen selection is **whole-module** — per-function
`target-cpu` attributes won't re-key the subtarget. That is all we need; if
mixed-gen-in-one-module tuning is ever wanted, adopt the AArch64/RISC-V
`getSubtargetImpl(const Function&)` + `SubtargetMap` cache.

### Validation

Lock the knob behavior with lit tests that run the same `.ll` under
`-mcpu=penumbra1` and `-mcpu=penumbra2` and CHECK the divergence (branch vs.
branchless select-of-constants; `bl memcpy` vs. inline word stores). The
*infrastructure* is what this entry covers; the actual cost *numbers* are a
measure-on-hardware follow-up gated on gen2 silicon (benchmark on the
ULX3S, not the ISS).

### Open decisions

- Feature granularity: the two properties above now vs. a finer set up
  front. Recommend two; grow on demand.
- Whether gen2 also drops `FeatureSmallICache` — depends on the gen2 L1
  size, still open in the gen2 design.
- Whether to expose a user-facing `-mtune` story or keep it `-mcpu`-only.

## Compiler: fuse a widening multiply into a single MUL_P

A 32×32→64 widening multiply currently selects to **two** hardware
multiplies (commit `ba7ef55`): the custom legalization of `G_MUL s64`
emits `G_MUL(a,b)` for the low word and `G_S/UMULH(a,b)` for the high,
which select to `MUL`+`MUL_P` (signed) or `MUL`+`MULU_P` (unsigned).
But `MUL_P`/`MULU_P` already produce **both** halves in one instruction,
so the separate low `MUL` is redundant — it recomputes the low product
that `MUL_P` discards into a dead register.  Collapsing the pair to a
single `MUL_P`/`MULU_P` is the remaining optimization.

**Measured leverage (ULX3S @ 25 MHz, mandelbrot seahorse, µs/sample):**
56141 (software `__muldi3`) → 4244 (4-mul schoolbook, `beb685b`) →
2584 (2-mul pair, `ba7ef55`).  Fitting `cost = k·muls + N` to the last
two points gives `k≈830`, `N≈924`, so a 1-mul form projects to
**~1750 µs/sample — a further ~1.47×, ~32× over the software baseline.**
Diminishing: the non-multiply floor (`>>28` realign + escape loop) is
already ~36% of the sample, so the third multiply matters less than the
first two did.  Only multiply-bound fixed-point code (this demo) sees
the full benefit; the kernel and Dhrystone are not multiply-bound.

**Why it's deferred — no clean GISel mechanism exists.**  GISel has no
two-result multiply op (SelectionDAG's `ISD::SMUL_LOHI`/`UMUL_LOHI` have
no `G_*` equivalent), so a single instruction producing both halves can't
be expressed at the generic level.  Prior art doesn't transfer:
- **ARM** (`SMULL`/`UMULL`, our exact twin) forms it via SelectionDAG's
  `SMUL_LOHI` — SelectionDAG only.
- **x86** (`MUL`→`EDX:EAX`) selects each half to a full `MUL` reading the
  fixed result register, then leans on `MachineCSE` to fold the two
  identical instructions into one.  Needs **fixed** output registers;
  our `MUL_P` writes regalloc-chosen vregs, so two `MUL_P`s never CSE.
- **RISC-V RV32** does the same 2-instruction schoolbook we do now and is
  content with it — its `mul`/`mulh` are genuinely two instructions, so
  it has no single-instruction form to fuse toward.

**Implementation routes, both with real cost:**
1. Selection-time pairing in `selectDivMul`: when selecting `G_S/UMULH`,
   scan for a sibling `G_MUL` with identical operands, emit one `MUL_P`,
   route its low output to the `G_MUL` result and erase it.  Order-
   sensitive (either op may be selected first) — the fiddly part.
2. Build `MUL_P` directly in the custom legalization (skip the generic
   pair entirely).  Bypasses the fusion but emits a target instruction
   pre-RegBankSelect, which is non-idiomatic and needs manual reg-class
   constraining.

Not worth the complexity until something multiply-bound matters more than
the ~1.47× on a demo.  Correctness surface if revisited: the same
signed/unsigned/i64 widening + `smul`/`umul` overflow execution tests used
for `ba7ef55`.

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

## Kernel/compiler: dedicate R12 (TP) to curlwp

The ABI reserves R12 as the thread pointer, and the kernel uses no TLS,
so R12 sits unused in kernel code.  Pinning `curlwp` there — the way the
NetBSD RISC-V port pins it in `tp` via
`register struct lwp *riscv_curlwp __asm("tp")` — turns every `curlwp`
read from a load of `cpu_info_store.ci_curlwp` into a register read.  It
costs no register pressure: R12 is already reserved, so the allocatable
set does not change.

### Compiler support — DONE

Two pieces, both landed:

- `register T x __asm("r12")` works end to end (`ff46f87450d8`):
  `getRegisterByName` resolves the canonical name and the ABI aliases,
  the GlobalISel legalizer lowers `G_READ_REGISTER` /
  `G_WRITE_REGISTER` to a COPY to/from the physical register, and
  naming an allocatable or unknown register is a fatal error.  clang
  already lists `r12` in `GCCRegNames`.
- The across-call spill problem is fixed (`32937b8bf9d7`): R12 is now a
  *member* of `GPR_Allocatable` (listed last, still excluded from
  allocation by `getReservedRegs` — the RISC-V `tp` idiom), so the
  register coalescer substitutes R12 into cross-call `%v = COPY $r12`
  values and each use gets a fresh `mov rX, r12` instead of a
  callee-saved copy or stack spill.

### Kernel wiring (not yet done)

1. `cpu.h`: `register struct lwp *curlwp __asm("r12")`, with the
   `curlwp` macro reading it; `curcpu()` stays the fixed
   `&cpu_info_store`.
2. Initialise R12 = `&lwp0` early in `locore.S`, before the first C call.
3. `cpu_switchto` already saves/restores R12 via `pcb_context` and
   writes `ci_curlwp`; additionally set R12 = newlwp.  `cpu_lwp_fork`
   must seed the new `pcb_context` R12 slot with the lwp pointer.
4. Trap/syscall entry from userland: the trapframe already saves all 16
   GPRs (preserving the user TLS pointer); load `curlwp` into R12 after
   the save, before running C.  Exit restores the user value via the
   trapframe automatically.

### Expected benefit (single-issue, in-order; instruction counts)

The dominant pattern `curlwp->field` in straight-line code drops from
`lli`+`lui`+`ldw`(curlwp)+`ldw`(field) = 4 instructions to `mov`+`ldw`
= 2, and removes a D-cache access.  It lands on the
lock/scheduler/fault paths that read `curlwp` constantly.

## Hardware + kernel: local console (HDMI text-video + USB keyboard)

Design complete and committed as docs; implementation not started. The
goal is a standalone local console — character-cell video out over
GPDI/HDMI and a USB keyboard in — so the machine needs no host terminal.
NetBSD is the first consumer (boot may stay on UART initially); a
boot-ROM local console is a later, well-defined follow-on.

Two new autoconfig device classes plus one reserved
([`system/bus.md`](system/bus.md)):

- `CLASS_TEXTVIDEO` (6) — character-cell console. Contract
  [`system/devices/text-video.md`](system/devices/text-video.md),
  microarchitecture [`internals/text-video.md`](internals/text-video.md).
- `CLASS_USBHC` (8) — transaction-level USB host. Contract
  [`system/devices/usb-host.md`](system/devices/usb-host.md),
  microarchitecture
  [`internals/usb-host-controller.md`](internals/usb-host-controller.md).
- `CLASS_FRAMEBUFFER` (7) — reserved; protocol fixed once a device exists.

Decisions already settled (rationale lives in the docs, not here):

- Text cells are 16-bit `{glyph, attribute}` in 32-bit-strided slots;
  color is optional/discoverable (`CAP.COLOR`); the hardware cursor is
  mandatory; mode 0 (640×480 / 80×30) is the mandatory power-up mode for
  monitor compatibility, with extra modes optional via `MODE_SEL`.
- USB exposes a transaction-level minimum (token / data / handshake); the
  NetBSD HCD sits under the MI USB stack (`dev/ic/sl811hs.c` as the
  structural template), which owns enumeration + HID. `ukbd` → `wskbd`
  and a `pcdisplay`-style `wsdisplay` meet at `wscons`.

Implementation work, by layer:

### Text-video RTL — first cut: mode 0 only
- Pixel generator: dual-clock char/attr BRAM, 8×16 font ROM
  (`$readmemh`), scan-out pipeline, mandatory hardware cursor →
  parallel RGB.
- Output PHY: TMDS encode ×3 + ODDR 10:1 serialize, dedicated video PLL
  (fixed 25.175 MHz pixel / ~126 MHz serial).
- `autoconfig_dev` wrapper (`CLASS_TEXTVIDEO`); `ulx3s_penumbra1_top` wiring to the
  GPDI pins.
- Goal (later): PLL dynamic-reconfig FSM + mode 1 (800×600 / 100×37).

### USB host RTL — low + full speed
- 48 MHz SIE (NRZI, bit-stuffing, CRC5/16, SYNC/EOP), transaction FSM,
  1 ms frame timer, port/line detect + reset.
- US2 wiring: RX diff on `usb_fpga_dp/dn`, TX on `usb_fpga_bd_dp/dn`,
  pulls on `usb_fpga_pu_*`; dual-clock-BRAM + handshake CDC.
- `autoconfig_dev` wrapper (`CLASS_USBHC`).

### Kernel
- `CLASS_USBHC` host-controller driver (`usbd_bus_methods` /
  `usbd_pipe_methods`, software root hub) modeled on `dev/ic/sl811hs.c`;
  enable the MI USB stack + `uhidev` / `ukbd` in the kernel config.
- `wsdisplay` back-end for `CLASS_TEXTVIDEO` (`pcdisplay`-style character
  memory) + `wskbd`; bring up `wscons` as a local console alongside (or
  in place of) the `com` console.

### Boot ROM — later
- Per-class console backends (text-cell writes; framebuffer software
  glyphs once that class lands) and a USB boot-keyboard reader, behind a
  putc/getc abstraction the monitor selects at autoconfig.
