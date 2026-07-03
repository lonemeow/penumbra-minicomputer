# Penumbra -- TODO

Outstanding work and roadmap items, plus durable findings from
completed investigations.

## Hardware: gen2 SDRAM CDC request-handshake skew (RESOLVED)

gen2 Dhrystone hung silently before printing. The proximate symptom was
an uncached read returning a stale word — a vector-table fetch dispatched
to an unmapped ROM page and re-faulted into the same vector forever. Made
deterministic by a read-after-write checker in `sdram_sim`, which caught
a read of `0xd000` returning `memory[0x8114]` instead of the installed
value.

Root cause in `sdram_cdc`: request acceptance used a *registered*
`o_sys_req_ready`. The slot was latched on cycle T (when `!sys_full &&
i_sys_req_valid`), but ready pulsed on T+1, with a `just_accepted`
deadzone suppressing a re-latch. The speculative adapter drives its
request combinationally and treats `i_req_ready` as "my current request
was accepted." When the master changes what it drives across that
one-cycle gap — a speculative `addr+4` push one cycle, then a real
(mispredicted) push the next — the T+1 acknowledgment is for the T latch
(the spec), but the adapter attributes it to the real push. The spec is
physically stored yet tagged REAL, so its response (the open-row word at
the spec address) is delivered to the real read, and the real address is
never latched into any slot at all. gen1's sequential fills never change
the driven request across the gap, so they never trip it; gen2's
mispredicting uncached/jump traffic does.

Fix: make acceptance a single-cycle valid/ready transfer.
`o_sys_req_ready` is combinational (`!sys_full`), the slot latches on the
same edge, and the deadzone is gone. Latch and acknowledgment are one
atomic event, so the master's per-accept bookkeeping always names the
request actually stored. Speculation stays enabled (no latency-hiding
lost); removing the per-accept deadzone is in fact slightly faster.

Removing the deadzone exposed a second issue: `sys_full` counted slot
occupancy via the SD read pointer (`sd_rptr_bin_synced`, freed when the
SD side reads a slot), but the master's depth-2 response/tag tracking
drains only at response delivery (`sys_rptr_bin`, which lags) — so the
master could issue a third request into its depth-2 tracking. `sys_full`
now counts `sys_wptr_bin - sys_rptr_bin` (sys-local, matching the tag
lifetime); an `outstanding_sys <= 2` assertion guards it.

Regression net:
- A permanent, always-on read-after-write consistency checker in
  `sdram_sim.sv` (windowed to the low 2 MiB) `$fatal`s at the cycle and
  address of any stale read under every gen2 sim, so this class of bug
  reports itself instead of being chased.
- `tb_sdram_cdc` pins the handshake contract directly (single,
  back-to-back, distinct-address, depth-2-in-flight), driven to the
  combinational valid/ready protocol.
- `tb_sdram_sim` (new full-stack stress) drives gapless fills + gapped
  jumps with its own shadow check. It exercises the whole stack but did
  *not* deterministically reproduce this phase-sensitive case; the
  permanent checker is the actual regression net.
- `isa/test_trap_store` was added along the way (a memory store inside a
  fault handler — a conformance gap nothing else covered).

gen1 note: `sdram_cdc` ships in the gen1 ULX3S bitstream. The handshake
path is entirely on the 25 MHz sys clock (not the fmax-critical fetch/TLB
cone), and gen1 was confirmed to synthesize and run with the change.

## Hardware: SDRAM adapter may deadlock on a non-back-to-back read stream (suspected)

Found while probing the gen2 path toward 50 MHz. A single-outstanding
register slice inserted on the L2 <-> external-bus boundary (in
`machine_penumbra2`, to cut the combinational `L2-addr -> bus-decode ->
busy -> L2` round-trip that limits gen2.5) turns the L2 line fill into four
*gapped* single reads — `re` drops between words, +2 cycles each. With that
slice in place execution ran ~174 instructions and then wedged:
`test_l2_ifetch` hung at cycle 1448 ("no program end within 2000000
cycles"). Single bypassed reads (boot, MMU off) and the normal
back-to-back fill both work fine — only the gapped-mid-fill stream
deadlocked.

Suspected cause in `sdram_bus_adapter.sv`'s speculative-prefetch FSM. Its
own header says a mispredict (next read != the speculated `addr+4`) is
handled by abandoning the in-flight spec and discarding its response — so a
*deadlock* on a gap is a robustness bug, not an inherent limit. Likely
culprits: the "in-flight hit, stall in BEGIN" case never getting its
consume when `re` goes low mid-spec, or the 1-bit tag FIFO desyncing when a
spec response is orphaned by the gap. This is the same family as the
RESOLVED CDC handshake-skew bug above (speculation interacting badly with a
changed/interrupted request stream), so the spec state machine deserves a
hard look for any remaining "assumes the master streams back-to-back"
assumption — the master is not obligated to.

Not currently triggered: neither the gen2 L2 nor the gen3 bus master gaps
mid-fill -- both hold `re` and advance the address word-to-word. The gen3
CPU<->bus decouple (`penumbra3_bus_master`) is exactly the pipelined master
that keeps the back-to-back stream, and its Phase-0 probe P0.4
(`penumbra3_bus_master_test`) confirms registering the boundary that way
does not hit the deadlock -- so this does not block gen3.

The bug itself still stands and should be fixed. The bus contract does not
oblige a master to stream back-to-back, so a gapped read stream is a valid
sequence the adapter must serve rather than hang on. The fix lives in the
adapter's speculative-prefetch FSM: it must abandon an orphaned spec on a
gap exactly as it already does on an address mispredict. File:
`hw/rtl/io/sdram/sdram_bus_adapter.sv`.

## Hardware: gen3 (Penumbra/3) Phase-1 build status

Phase 0 (the four structural probes) passed; see
`doc/internals/penumbra3/phase0-probes.md`. Phase 1 builds the 7-stage
skeleton stage-by-stage: each module is forked or written, given a
`MODULE_TESTS` unit test, and committed to `main`. Run one test with
`make sim MOD=<name>_test TB=tb_<name> PROG=` (a packed-struct port needs a
`_test.sv` unpacking wrapper; flat-port modules use the bare name).

**Done** (front end through memory access, all unit-tested):

- `penumbra3_pkg` control bundle + enums (`7a964ee`); `dpath_payload_t`
  pipeline-register payload (`d8c2d5d`)
- `penumbra3_decode` -- word->bundle, parallel-then-select, mode-independent (`4e60d01`)
- `penumbra3_fetch_buffer` -- IF2->ID elastic FIFO (`00051be`)
- `penumbra3_if1_stage` (`67eeb41`), `penumbra3_if2_stage` decode-on-enqueue (`27166b0`)
- `penumbra3_id_stage` -- regmap + scoreboard + issue + priv/fault finalize (`10e088b`)
- `penumbra3_ex_stage` -- operand forwarding + ALU + flags + branch + divmul +
  drain-commit, with `penumbra3_alu` / `penumbra3_flag_bypass` leaves (`25d7b28`)
- `penumbra3_mem1_stage` / `penumbra3_mem2_stage` -- split launch/resolve: TLB
  verdict + cache hit, sub-word extract, fault merge, the `load_complete` hold
  buffer; `load_complete` parks the full descriptor and `dtranslate`/`tlb_store`
  gained an `i_hold` read clock-enable for the freeze; composed
  mem1->dtranslate->mem2 test (`96da15b`)
- `penumbra3_wb_stage` -- the commit point: physical-index write routing
  (regfile vs SPR-file/scratch, USP-to-regfile), the NZCV flag strobe, divmul
  dual-write sequencing through the single regfile port, and precise
  fault-commit gating; flat-driven unit test (`79ef076`)
- `penumbra3_regfile` / `penumbra3_spr_file` / `penumbra3_scratch_file` -- the
  storage tier WB commits into: GPRs + banked USP/SSP (R0 forced 0), SR/EPC/ESR
  (SR moves only via entry/ERET/EI/DI + flag commit -- no WRSPR SR leg), and
  SCR0..3; each flat-port unit-tested (`3e40ce4`)
- `penumbra3_spine` -- ID/EX/MEM1/MEM2/WB integration around the
  regfile/SPR/scratch files: bundle+payload handoffs, operand/flag forward
  sources into EX, the scoreboard clear, the regfile write-port mux (commit vs
  load completion), drain-commit (ERET/EI/DI, WRSYS) + fault commit/flush, and
  the freeze distribution (registered `load_pending` freezes the launch side +
  issue; the MEM2/WB register is held only by WB back-pressure -- the carve-out).
  Fetch-stream unit test: ALU forwarding, divmul dual-write, precise fault
  (`297b12d`)
- `penumbra3_vecfetch` -- exception vector-fetch FSM: borrows the fetch port on
  a fault commit, reads `vector_table[vec<<2]` (MMU-bypassed), redirects IF1 to
  the handler from a register (`19cb214`)
- `penumbra3_irq` -- interrupt recognition + EX-frontier injection (synthetic
  fault on the precise path; EI one-instruction shadow) (`e19a10a`). The spine
  gained `o_ex_valid`/`o_ex_stall` -- the clean-boundary frontier the unit gates
  on (`94d0b9a`)
- `penumbra3_core` -- the front end (IF1/IF2 + fetch buffer) onto the spine plus
  `vecfetch` + `irq`: the fetch-port mux (IF1 vs vecfetch), the redirect/flush
  composition (branch / vector-fetch / ERET / WRSYS-resync steer; a fault
  flushes but steers later), and the I-fetch + D-side port groups exposed for the
  machine layer. Integration unit test (sync-read fetch ROM): streaming +
  in-order commit, branch redirect, interrupt entry via the vector fetch
  (`c281f88`)

**Spine follow-ons** (do not block the memory-hierarchy fork): the
*forwarding-release* increment
(early scoreboard clear + the `i_fwd` broadcasts -- correctness-first leaves
them tied off today, so a load/divmul-use waits for the registered clear) and
the *load_pending-freeze / memory-path test* (the load path is wired but the
skeleton test drives no translate/cache/fill verdicts yet, so the load-miss
freeze is unexercised).

**Memory hierarchy** (the fork in progress):

- `penumbra3_translate` -- the side-neutral translate path (renamed from
  `dtranslate`; the MMU uses it twice), now exposing its registered set for
  sysreg read-back (`9fd2928`, `460f6d4`)
- `penumbra3_mmu` -- I+D translation unify: `translate` x2 over a shared
  coherent write stream + dual-port `tlb_pinned`, per-port bypass>pinned>main
  verdict, the device-0 sysreg interface (MMUCR / FADDR / FSTAT / TLB
  install+read-back), commit-time fault registers (`02f57e3`)

**Next, in order:** L1 caches (passive: index->tag/data + fill-install, no
embedded FSM -- miss orchestration is in `load_complete`) -> L2 (write-through)
-> fill sequencer -> arbiter (-> `bus_master`) -> `machine_penumbra3` +
`ulx3s_penumbra3_top`. **Open decision at the L1 step:** the store write-through
path is unbuilt -- `mem2_stage` writes the L1 copy (`o_dcache_we`) but issues no
write-through to L2/memory and does not block (stores retire via `mem2_commits`).
Phase-1's gating checkpoint is then the **full-top composition timing read**
(bare skeleton, no BTB) -- calibrates the probe->full margin before feature work.

**Locked hazard-model decisions** (context for resuming):

- Decode runs on the FIFO *enqueue* path and is a pure function of the
  instruction word -- everything mode-dependent (regmap, the privilege fault,
  the fault vector's `VEC_PRIV` arm) stays in ID against live `SR.S`.
- The bundle carries a single positive-sense `dst_we` (routed GPR/SPR by
  `dst_is_spr`): a forwarding machine's destination-valid bit must mean
  "actually writes", so a CMP must advertise no destination.
- The scoreboard tracks only the non-forwardable producers (LOAD / DIVMUL /
  RDSYS); ALU results never set a bit -- EX forwards them. The issue gate
  blocks a source only on `used & pending & ~forwardable`.
- Forward broadcasts to ID are producer *stages* (not source lanes), so every
  source matches every broadcast slot.
- The fetch FIFO *is* the IF2/ID register (decode folds into enqueue, no extra
  stage); the IF2 skid is retained for the drop-equals-valid I-cache
  completion when the FIFO is momentarily full.
- The resolved datapath rides a packed `dpath_payload_t` (value, dest routing,
  flags, PC, fault verdict) carried as a unit across the EX->MEM->WB registers
  alongside `ctrl_bundle_t`; `store_data` rides its own EX->MEM1 wire (consumed
  in MEM2, never reaches WB).
- The back-end stall is spine-distributed: MEM2's `load_complete` produces the
  registered `load_pending`, and the spine fans it back to freeze the *launch
  side* -- MEM1, the MEM1/MEM2 register, `dtranslate`/`tlb_store`/cache, and
  issue -- so the missing load and everything younger hold. It must NOT freeze
  the MEM2/WB output register: that drains older work to commit (the held load
  retires via the completion path, not MEM2/WB), and folding `load_pending` into
  its freeze re-presents an already-committed multi-cycle writeback (the divmul
  dual-register retire) and retires it twice. The MEM2/WB register's only
  back-pressure is the WB dual-write hold. Contract:
  [load-completion freeze scope](internals/penumbra3/memory-completion.md#what-the-pipe-gate-freezes).
  A frozen MEM2 slot keeps its own verdict, and no combinational cache/TLB
  verdict ever reaches issue. Candidate spine assertion: `load_pending` never
  gates the MEM2/WB register.

**Deferred (resolve at the named task):**

- *Issue-release broadcasts* -- the operand-value forward network is settled
  (EX sources EX/MEM1, MEM1/MEM2, MEM2/WB; youngest-wins; write-first regfile
  for the four-deep distance; the MEM1 leg excludes loads/sysreg reads; MEM2
  exposes `o_fwd_*` for the load-use leg). What remains is a pure spine task:
  drive ID's `i_fwd*` to release a scoreboard-pending load / sysreg read at the
  cycle its value becomes forwardable -- gated on the registered `load_pending`,
  never a live verdict. ID owns only the match today.
- *divmul aux-destination scoreboard bit* -- set path done (`4fa078b`): the
  scoreboard has a second set port, ID regmaps the aux (Rdh) and sets its bit at
  issue; the spine drives the matching clear at the aux writeback.
- *divmul writeback back-pressure reach* (`wb_local_stall`: the WB dual-write
  ripple WB->MEM2->MEM1->EX->ID). The divmul's two-cycle WB writeback makes WB
  occupy two cycles, and that hold ripples up the pure-stall chain. It is 1-bit
  and flop-shallow -- no cache/TLB verdict in it, so it is correct and within
  the "1-bit ready crosses backward" allowance -- but it is the longest-*reach*
  combinational stall and may bind at 50 MHz on routing distance alone.
  Floorplanning is NOT the remedy (placement cannot be the fix for a structural
  reach). Structural fix, deferrable because it is localized and does not touch
  the core shape: retire the divmul writeback through the **load-completion
  path** rather than the WB stage. divmul holds in EX until it is the oldest
  in-flight -- `drained` = `~mem1_valid & ~mem2_valid & ~memwb_valid &
  ~load_pending` (the drain-commit condition extended with `load_pending`) --
  then writes Rd/Rdh over two cycles through the completion write-port mux. The
  port is free **by the in-order single-outstanding invariant** (oldest => no
  older writer; younger blocked behind the held divmul), NOT by any cycle count
  -- so a slow uncached MMIO access ahead of the divmul simply makes it wait in
  EX, never write early or out of order. WB then never sees a divmul: the
  dual-write FSM and `wb_local_stall` both delete, and back-pressure reduces to
  the `load_pending` fan-out plus the one-hop EX->ID stall. Land as its own
  increment if HW timing shows the reach binds; until then the WB dual-write is
  correct (WB is the oldest by pipeline position, so it is already drain-gated
  -- only the reach is suboptimal).
- *Vector-fetch cacheability* -- `penumbra3_vecfetch` will keep the vector
  read uncached for now. It is the CPU's only fetch-by-PA (not VA), so
  threading a PA index into the VIPT caches is awkward; MMU-bypass is required,
  cache-bypass is not, so making it L2-cacheable is a worthwhile later win
  (kills a full-SDRAM round-trip per trap). Revisit after the skeleton boots.

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
lands, gate the retired count by `~fault_pending`.

The program-end testbench keys on `o_retire_op_class == OPC_BREAK`, and that
*was* spuriously trippable: an IF-faulted slot carries a garbage instruction
word whose decode can be `OPC_BREAK`. A fetch bus fault whose stale read
happened to decode as BREAK ended the run before the handler dispatched (found
via `isa/test_bus_fault_fetch`). Fixed: `penumbra2_id_stage` neutralises a
faulting slot's `o_op_class` to `OPC_ALU` (the same reason `o_is_trap` is gated
there), so an inert slot never reports a real op class at retire. The remaining
`o_retire_valid` over-count is the perfctr-only item above.

## Hardware: Penumbra/2 perfctr stall attribution is mis-timed — STALL_FLUSH over-counts (RESOLVED)

Resolved by the bubble-cause-tag scheme on both gen2 and gen2.5. Each pipeline
bubble carries a `BCAUSE_*` tag set where it is injected and propagated ID→EX→MEM
→WB; the perfctr charges a non-retiring cycle to the carried tag at the commit
point instead of to the live per-stage stall signals, so a short stall (whose
bubble outlives its signal) and a long stall's latency tail land in their true
back-end bucket (LOAD/STORE/FUNIT/HAZARD) rather than the front-end residual. The
front-end split was *not* deferred after all: the "larger fetch-path change" the
diagnosis feared (threading tags through the elastic buffer) proved unnecessary —
an empty fetch slot is classified at the starved cycle by `i_fetch_busy`
(memory-bound → IFETCH, else FLUSH), which is correct by the counters' own
definitions since the buffer absorbs fetch bubbles and the right question is the
front end's live state, not a carried token. RDSYS (the read path for the
counters themselves) is charged to the residual, never a precise back-end bucket,
so the instrument cannot perturb what it measures. Verified by
`test_cpu_stall_hit_load` (a cache-hit load burst now moves STALL_LOAD ≥ N and
does not leak to FLUSH — the old code scored ~0) plus a per-cycle one-hot
partition assertion that runs through the whole conformance suite on both cores.
The original diagnosis is kept below for context.

The CPU stall-attribution counters charge a non-retiring cycle by the stall
signals asserted *that same cycle* (head-of-line, evaluated at the retire
point). But the bubble that blocks retirement at WB was injected upstream,
roughly a pipeline-depth of cycles earlier, by a stall that may already have
cleared. So the attribution is mis-timed: a stall shorter than the pipeline
depth is *fully* mis-attributed (its bubbles reach WB after its signal
deasserts), and a long stall loses its tail. The orphaned cycles fall into
`STALL_FLUSH`, which is the residual catch-all — "non-retiring, and no stall
signal asserted." So `STALL_FLUSH` is **not** a front-end / branch-flush
counter; it is inflated by the latency tails of every short back-end stall.

Measured with an RTL-sim probe (a temporary per-cycle classifier in the gen2.5
core that buckets every non-retiring cycle by pipeline state, alongside a
replica of the perfctr's residual logic), on the representative regime —
cached-fetch + cached-data `memcpy` through the MMU, the same access pattern
real `memcpy`/`pmap_copy_page` hits:

- Of ~18.5k cycles the perfctr called `flush`, only ~5.2k were genuine
  front-end fill (pipe empty: no instruction anywhere in EX/MEM/WB). The other
  ~16k were back-end-stall latency tails — an instruction sitting in MEM with a
  bubble passing through WB and no stall asserted that cycle.
- Cross-checks pinning the cause: *no* flush cycle had a live data request
  (`o_dmem_re`/`o_dmem_we` both low), so it is not a current memory access;
  flush *persisted* with cached fetch, so it is not fetch-stall drain; and an
  A/B with the `ldw`→`stw` dependency broken (identical memory traffic, no
  load-use hazard) left the residual essentially unchanged, so it is **not**
  load-use hazard (only ~256 cyc were — those correctly land in `STALL_HAZARD`).
  It is memory-access latency tails surfacing late.
- A memory-free, hazard-free, BTB-predicted loop showed pipe-empty ≈ 0, ruling
  out any structural front-end-throughput bubble.

Implication for the gen2.5 roadmap: the priority ordering ("flush is #1, ~2.5×
the next lever") was read off a mislabeled bucket. The genuine,
prediction-addressable front-end flush is small (~5k on `memcpy`), which is
exactly why BTFN, the RAS, and the BTB each moved `STALL_FLUSH` only a little —
the predictors work as designed; the counter misled. The dominant real-workload
CPI lever is the **back end** — operand forwarding (which removes the load-use
stall *and* its latency tail, the bulk of the bogus "flush") and the memory
system — not further branch prediction.

Fix: tag each pipeline bubble with the cause that injected it, carry the tag
*with the bubble* to WB, and charge the non-retiring cycle to the carried cause
— so attribution no longer depends on how far upstream, or how many cycles
earlier, the stall was. Back-end causes (load / store / funit / hazard) ride
bubble tokens that flow ID→EX→MEM→WB, so they tag cleanly; this is the dominant
measured mis-attribution and the right first increment. Front-end causes
(ifetch / flush) are decoupled by the elastic fetch buffer, which drops bubble
tokens — an empty buffer *is* the front-end bubble, with no token to carry a
cause — so attributing those correctly needs the buffer to carry tagged
bubbles, a larger fetch-path change; defer it behind the back-end chain. The
change is perfctr-observability-only (no pipeline-behaviour risk) and
discrete-logic-friendly (a few flops + muxes per stage). The diagnosis and the
encoding sketch (a `BCAUSE_*` field) were worked out but not landed; the BTB
that prompted this investigation sits on the `gen2.5-btb` branch, separate from
this fix.

## Hardware: Penumbra/2 taken branch under a MEM stall lost its link write (RESOLVED)

A `BL`/`JMP` resolving in EX while an older memory op stalled MEM was discarded
instead of committing — its link write (`R13`) was lost. Surfaced as Dhrystone
on the gen2 RTL "crashing": `bench_main`'s prologue (a run of write-through
stores hitting cold lines) stalled MEM with `bl bench_init` right behind it; the
BL took its branch (front end redirected to `bench_init`) but never retired, so
`R13` kept `bench_main`'s caller link. `bench_init`'s closing `jmp r13` returned
straight to `_start`'s program-end `break`, skipping the benchmark — Dhrystone
printed nothing and BREAK'd at the *normal* end PC, which is why it read as a
crash rather than a hang.

Root cause: `penumbra2_id_stage`'s ID/EX register ranked `i_bubble` above
`i_stall_in`. `ex_branch_taken` rides `i_bubble` to kill a taken branch's
wrong-path *successor* — correct only once the branch advances out of EX. Under
MEM back-pressure (`ex_stall`) the branch is pinned in EX, so the ID/EX register
still holds the branch itself, and the bubble cleared it. Fix: gate that one
term — `i_bubble = (ex_branch_taken & ~ex_stall) | wb_fault_commit | … ` — in
`penumbra2_spine`. `wb_fault_commit` stays ungated (a fault must kill younger
slots regardless of back-pressure); `eret_commit`/`wrsys_resync` fire only with
the pipe drained, so they never coincide with a stall.

Diagnosed with the gen2 RTL trace (now implemented in
`tb_penumbra2_interactive`: retire stream, drain-commit lines, branch-resolve
markers, and an opt-in per-cycle EX/MEM/WB occupancy window via
`+pipe_lo=/+pipe_hi=`). The occupancy view was decisive — it showed the BL in EX
with the older store still occupying MEM, then gone the next cycle without MEM
advancing: a resolve-then-squash a retire-only trace cannot show.

Coverage gap this exposed: leaf testbenches drive their own stalls
(`tb_penumbra2_mem_stage` richly, `ex_stage`/`id_stage`/`if*` lightly), but the
*integration* (`tb_penumbra2_spine`) drove `i_dmem_busy = 0` always — zero
coverage of a stall *interacting across stage boundaries*, which is the only
place this bug is visible. Closed by the `run_mem`/`tick_mem` model (a
configurable multi-cycle MEM latency, borrowed from `tb_penumbra2_mem_stage`)
and Test 5 (a BL pinned in EX by a stalling load). Still worth adding on the
same harness: drain-commit under a MEM stall, a RAW consumer waiting on a
producer stalled in MEM, a fault committing while a younger branch is resolved
under stall (verifies the `wb_fault_commit` override — the opposite of this
fix), and a divmul immediately followed by a branch.

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
`machine_penumbra2_sim` (machine + `boot_rom` + the full SDRAM stack), the probe
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

The microarchitecture sub-variant axis is in: `CORE=penumbra<n>_<sub>`
(first instance `penumbra2_5`) builds its base generation's RTL with the
`CPU_VARIANT` core parameter set (entered as the `PENUMBRA_CPU_VARIANT`
define), across `make test` / `simulate-rtl` / `benchmark-rtl` / `fpga`.
`CORE_BASE`/`CORE_SUB` split the name; the variant inherits the base's
runner, capabilities, and program suite plus its own
`hw/sim/programs/penumbra2_5/`, and for fpga `TOP` names the per-variant
artifact while `TOP_MODULE` names the shared synth module (no duplicate
board top). The gen2.5 microarchitecture is built by **composition**:
`CORE=penumbra2_5` selects the gen2.5 fileset — gen2's leaf cells (ALU,
regfile, scoreboard, decoder, the MMU/TLB/cache stack, ...) shared
unchanged, plus the forked integration chain (machine shared; core /
spine / ID + EX assemblies forked into `hw/rtl/penumbra2_5/`) — so gen2
stays byte-frozen and never reads as a gen2.5 with its optimizations
switched off. The sim build resolves the forks via a variant `-I`
prepend, the fpga build via `SRC_CORE_penumbra2_5`; the cpuid name
("Penumbra/2.5") still rides the `PENUMBRA_CPU_VARIANT` define, and
cycle-exact tests gate on a `pinned-stalls` capability gen2 provides and
gen2.5 drops. Mechanism: `doc/internals/penumbra2/overview.md` (gen2.5
organization) and `doc/internals/build-system.md` (variant naming).
gen2.5 is no longer identity-only — see the feature status below.

### gen2.5 priority is ranked from the stall profile — real workload first

The build order below is **data-driven from the gen2 stall profile**,
re-measured on hardware after the perfctr stall-attribution fix (the
carried-cause counters — see the RESOLVED finding above). The pre-fix profile
that originally set this ranking was read off the mislabeled residual: it
reported `flush` as the dominant stall (~30% on Dhrystone, ~35% on NetBSD) and
put branch prediction first. That was an artifact — back-end latency tails
leaking into `flush` — and the corrected profile re-ranks the levers.

- **Dhrystone on gen2 (HW, 25 MHz):** CPI 3.13. Stalls (% of cycles):
  hazard 28.6, store 18.3, flush 11.6, load 6.0, funit 3.3, ifetch ~0.
  L1I/L1D ~99.9% hit, L2 nearly idle.
- **Dhrystone on gen2.5 (HW, 25 MHz):** CPI 3.05. hazard 29.4, store 18.8,
  flush 9.2, load 6.1, funit 3.4, ifetch ~0 — *same back end*; `flush` falls
  ~2.4pp from the predictors (and ~1.7M fewer wrong-path I-fetches), which is the
  entire gen2→gen2.5 gain (DMIPS 7.11 → 7.29, +2.5%).
- **NetBSD pbench profile (HW, 25 MHz, corrected counters) — the real-workload
  re-measurement, now in.** Kernel-path stall % of cycles, gen2.5 core at the
  BTFN+RAS milestone (both predictors later removed in
  [the fmax-closure round](#gen25-status-fmax-closure-round-375-mhz); the
  surviving predictor is the fetch-time BTB):

  | bench         | CPI  | hazard | ifetch | store | flush | load | funit |
  |---------------|------|--------|--------|-------|-------|------|-------|
  | getpid        | 4.12 |  22.2  |   8.1  |  22.5 |  13.1 |  7.6 |  2.1  |
  | clock_gettime | 4.65 |  18.8  |  16.0  |  20.2 |  11.7 |  5.6 |  6.2  |
  | pipe_pingpong | 4.63 |  22.1  |  19.3  |  12.1 |  17.3 |  7.0 |  0.6  |
  | fork_exit     | 4.94 |  21.0  |  19.7  |  11.4 |  14.3 | 12.1 |  1.2  |
  | fork_exec     | 4.41 |  22.6  |  16.0  |  12.0 |  13.5 | 11.6 |  1.6  |

  The pre-fix placeholder (flush 34.7, store 14.2, hazard 13.9, ifetch 5.3)
  inverts under the corrected counters. **hazard (~21%) is #1 here too** —
  forwarding leads on real code, not just Dhrystone. **`ifetch` is now
  first-class (16-20%) and on the representative fork/exec/pipe paths it is the
  #2 lever, ahead of flush and store** — genuine I-cache/L2 footprint (L1I
  88-94% hit vs Dhrystone's 99.9%), and untouched by any planned gen2.5 feature.
  store swings by workload (22% getpid, ~12% fork/pipe); flush (12-17%) is the
  real BTB target but no longer the headline.

Non-stall cycles equal insns retired (the core never overlaps a stall with
useful work), so a class's cyc/insn is its cycle-share x CPI and the ranking is
exact. On Dhrystone gen2: hazard 0.90, store 0.57, flush 0.36, load 0.19,
funit 0.10 cyc/insn.

What the corrected profile says:

- **hazard is #1 — by a wide margin (28.6%, ~0.90 cyc/insn).** It is RAW
  interlock with no operand forwarding: every dependent instruction waits for
  its producer to reach WB. **Forwarding is the top CPI lever**, not prediction.
- **store is #2 (18.3%, ~0.57).** Write-through round-trips — every store
  reaches L2 (L1-D/L2 write ~88% hit). The store buffer / write-back path is
  the lever here.
- **flush is #3 and genuinely front-end now (11.6%).** Prediction (gen2.5)
  already attacks it and delivers the measured +2.5%; real, but the smallest of
  the three big levers — exactly why BTFN/RAS/BTB moved CPI only modestly (the
  predictors work; the old counter had inflated their apparent target).
- **ifetch ~0, load 6.0, funit 3.3** are minor on this footprint; `ifetch` ~0
  confirms the I-cache (99.9% hit) is not the bottleneck. Re-measure on NetBSD
  before assuming the same — its larger footprint pays real `ifetch` and L2.

Decisions taken from this:

- **Store buffer is in gen2.5 scope** (beyond `overview.md`'s original
  forwarding+prediction list) — co-second cost, and the lever that reaches the
  CPI target on store-heavy code. The uncached/MMIO hazard is resolved by a
  **cacheable-only** buffer: an uncacheable store drains it to completion
  before issuing. That single rule gives MMIO program-order, DMA-kickoff
  visibility (the MMIO write that starts a transfer drains all prior cacheable
  stores first), and trivial load disambiguation (only cacheable loads consult
  the buffer; an MMIO read is a different PTE.C and never aliases a buffered
  entry). It attaches to the existing write-through path — distinct from, and
  lighter than, the gen3 write-back L2 reshape.

- **Build order: prediction -> forwarding -> store buffer.** This is a
  *risk* ordering, not a magnitude one — the corrected profile puts forwarding
  (hazard, #1) and the store buffer (#2) ahead of prediction (flush, #3) in
  payoff. Prediction went first anyway because its blast radius is perf-only
  (EX stays the branch authority, so a misprediction is an extra flush, never a
  wrong result) and it reuses machinery that already exists — the IF1
  `i_redirect` mux and the EX taken-branch flush — making it the safe way to
  prove the variant framework. It has landed (+2.5% DMIPS); **forwarding is now
  the live priority and the biggest remaining lever.** Forwarding second, once the variant framework is proven:
  it is the most result-checkable feature (it changes timing not results, so
  the optimized variant must produce bit-identical output to the baseline
  across the inherited suite — any divergence is a localized forwarding bug).
  Store buffer last (most structural — the load-disambiguation path).

- **Measure each feature's delta as it lands.** Features enter the gen2.5
  fork one at a time (prediction, then forwarding, then the store buffer);
  each one's isolated CPI delta is gen2 vs. the gen2.5 fork at that milestone,
  read off the hardware perfctrs. Composition has no shared file to gate, so
  there are no per-feature compile switches — the milestone is the isolation.

- **Test-signature asymmetry.** Prediction is validated by perfctr deltas
  (flush cycles must drop — its bugs are silent perf); forwarding by
  result-equivalence (its bugs are wrong answers). Running gen2's suite
  against the gen2.5 fork supplies both instruments.

**First cut — ID-stage BTFN + unconditional-direct fold (landed).** Backward-taken/
forward-not-taken on conditional branches, plus always-redirect on
unconditional direct `B`/`BL`. Prediction is at **ID**, not at fetch: the
decoder already produces `OPC_BRANCH` / `cond` / `imm`, so there is no
duplicated pre-decode, and the redirect (`id_pc + imm` -> IF1 PC) runs in a
fresh cycle on registered inputs, off the icache/TLB critical path — it is a
strictly shorter sibling of the existing EX->IF1 taken-branch redirect that
already closes timing, so it cannot become the new limiter. The new pieces are
the direction policy (`penumbra2_predict`), a 1-bit predicted-taken tag ID->EX,
and generalizing EX's redirect from "redirect-on-taken" to
"redirect-on-mispredict". These land in the forked gen2.5 integration files
(the ID/EX assemblies) plus the new `penumbra2_predict` leaf; gen2's own stages
keep resolving on taken, untouched. A correctly-predicted-taken branch costs
3->2 bubbles.

Fetch-time prediction (IF2) was rejected on timing: the target add would sit in
series with the icache hit / way-mux tail, the current fmax limiter. The
real-CPU way to redirect at fetch is a **BTB** — a small RAM, indexed by fetch
PC, returning the target in parallel with the icache (no decode, no add in the
fetch loop) — which is the documented evolution: it buys the last bubble
(3->1/0), keeps fetch-time prediction timing-safe, and (with a RAS) captures
the `JMP R13` returns the real-workload data weights heavily. Start simple at
ID, measure, then add the BTB.

The compiler already emits BTFN-shaped code, so a static predictor hits:
MachineBlockPlacement + the backend's branch-reversal place the likely path as
fall-through and loop back-edges as backward-taken branches — no forward-taken
hot paths, no branch-probability hints (`PenumbraInstrInfo.cpp`
analyze/insert/reverseBranch; `branch-opts.ll`; `Penumbra.td` even sets
`MispredictPenalty = 0`). Direct transfers (`B`/`Bcc`/`BL`) are
foldable/predictable; indirect ones (`JMP` — incl. `RET` = `JMP R13` —,
`JALR`, `BRIND`) need a RAS/BTB. Real NetBSD code has higher call/return
density than Dhrystone (printf/format-heavy call graphs), so the `JMP R13`
return slice of flush is larger there — **the return-address-stack follow-on
likely matters more on real workloads than Dhrystone implies, and should not be
deferred far behind the BTFN first cut.**

### gen2.5 status: BTFN landed (first feature)

> **Removed in the fmax-closure round**
> ([gen2.5 status: fmax-closure round](#gen25-status-fmax-closure-round-375-mhz)):
> the ID-stage BTFN predictor's redirect reached back into the fetch path and
> became the fmax limiter, and registering it bought zero cycles (it would fire
> the same cycle EX already resolves the branch). It was removed; the
> fetch-time BTB is the surviving direct-branch predictor. The measurements
> below are retained as the record of what ID-stage BTFN bought.

ID-stage BTFN branch prediction is implemented and committed — the
`penumbra2_predict` leaf, the integration-fork wiring (ID-stage redirect
+ predicted-taken tag; core/spine routing; EX resolve-on-mispredict), and
its unit (`tb_penumbra2_5_ex_stage`) and integration (`test_btfn.s`)
tests. First of the gen2.5 features; gen2 untouched.

**Measured (gen2 vs gen2.5, both at 25 MHz).** Dhrystone CPI 3.03 -> 2.92,
DMIPS 5.03 -> 5.22 (+3.8%), from ~12% fewer flush cycles (85.98M ->
75.76M). The NetBSD pbench kernel microbenchmarks (getpid, clock_gettime,
pipe_pingpong) are ~flat: their flush is return-dominated (`JMP R13`),
which static BTFN cannot predict. This confirms the `penmon`-profile read
above — real-workload flush is call/return-heavy.

**Next (chosen from that result):** a **return-address stack** to claim the
kernel returns BTFN leaves on the table — now landed; see the RAS status
below.

**fmax margin — floorplan deferred on purpose.** BTFN costs ~3.5 MHz:
gen2 synthesizes ~30 MHz, gen2.5 ~26-27.5 across seeds. Confirmed
structural by an A/B — forcing the predictor's two front-end outputs to 0
on the *same* gen2.5 build recovers ~29-31 across seeds, so it is the
BTFN logic, not the build setup. The limiter is unchanged shared logic
(the cache->MMU cone) that merely *places* worse once the BTFN cells +
the ID->fetch redirect bus crowd the memory cluster (utilization ~24%, so
placement, not fullness). gen2.5 still meets the 25 MHz constraint, with
thin margin. A gen2.5 floorplan (`PREPACK_ulx3s_penumbra2_5_top`,
per-TOP, gen2 untouched) is the recovery lever, **deferred until gen2.5
either starts failing timing or has all its features landed** — each
remaining feature loads the same cluster, so floorplanning now would be
redone after each; do it once.

**Tooling / pre-existing.** `NEXTPNR_SEED` is not in the build dependency
graph, so a seed sweep needs `rm build/<top>.config` before each
`make fpga` (synth stays cached). `make test-modules` resolution now
prunes variant-fork dirs (a fork shares the base module name); the gen2.5
EX unit test rides `VARIANT_MODULE_TESTS` with an explicit source path.
Separately, `make test-modules` has pre-existing bitrot unrelated to
gen2.5: `tb_penumbra2_{id,ex,mem,wb}_stage` reference the `o_stall` port
renamed in `dc5dd5c` (to `o_local_stall` / `o_stall_{load,store}` /
`o_funit_stall`), and `tb_penumbra2_irq` / `tb_txn_arbiter` /
`tb_penumbra2_tlb_unit` fail on other committed port drift — fix is
per-testbench reconciliation.

### gen2.5 status: RAS landed (second feature)

> **Removed in the fmax-closure round**
> ([gen2.5 status: fmax-closure round](#gen25-status-fmax-closure-round-375-mhz)):
> the RAS rode the same ID-stage redirect that BTFN did, so it left with BTFN.
> Returns now take the full EX-resolve penalty. The measurements below are
> retained as the record of what ID-stage return prediction bought.

A **return-address stack** is implemented and committed — the
`penumbra2_ras` leaf (circular, overwrite-oldest on overflow, DEPTH=8), the
ID-stage call/return detection (a call is any GPR-writing branch/jump —
`BL`/`JALR`; a return is `JMP R13` that is not a call) with issue-gated
push/pop, the predicted-target carry ID->EX, and EX's indirect-target
verification — a return's RAS-predicted target is checked against the
resolved `R13`, sourcing the jump target directly (`i_op_a`, not
`branch_target`) so the added 32-bit compare stays off the ALU-result path.
Returns now redirect at ID over the existing BTFN path (3->2 bubbles)
instead of always mispredicting in EX. Unit test `tb_penumbra2_5_ras`
(LIFO / exact-full / overflow past a full revolution / underflow) and
integration `test_ras.s` (a clobbered-return case exercising the EX target
check); gen2 untouched.

**Fault-shadow drift, accepted.** The stack mutates only on issuing,
non-faulting slots, so a branch mispredict (1-cycle EX resolve) and the
drain-commit redirects (ERET/WRSYS, pipe drained) never corrupt it. A
precise fault is the exception: it commits at WB and squashes an
already-issued shadow of up to two younger slots that may have moved the
stack; those re-run after the handler, leaving a bounded, self-healing
pointer drift. EX corrects every misprediction, so this is perf-only, never
a wrong target — accepted rather than adding a committed-shadow repair
(faults are rare per instruction; the repair would recover noise).

**Measured (RAS off vs on, same gen2.5 build, fmax 27.4 MHz — RAS costs
~0).** Dhrystone DMIPS 7.20 -> 7.29 (+1.25%), CPI 3.09 -> 3.05, from ~1.6M
fewer flush cycles — about one bubble saved per return across ~1.6M returns
(~100% return coverage). NetBSD pbench kernel paths, where BTFN was flat:
getpid and pipe_pingpong -2.8%, clock_gettime -1.6% cyc/op (fork_* too noisy
at iters=1 to read). The RAS reaches the returns BTFN left on the table, as
predicted.

**The flush bucket is real front-end fill, but the corrected counters halve it
and demote it.** This section originally read flush at 37-42% on libc size=1
loops and called the BTB the dominant remaining lever. The carried-cause
counters (see the RESOLVED finding above) put those same loops at ~17-26%
(memcpy/memset/strlen size=1) and kernel paths at 12-18%; the missing ~half was
back-end latency tails, now correctly in load/store/hazard. What flush measures
now is genuine — the 2-bubble front-end fill of every taken conditional branch,
which ID-stage prediction (BTFN/RAS) structurally cannot remove and only a
fetch-time **BTB** (target RAM read in parallel with the icache, 2->0/1 bubbles)
reaches. So the BTB stays a real lever; it is just no longer the #1 it appeared
to be, and on the representative fork/exec/pipe paths `ifetch` (the I-cache it
cannot touch) is the larger front-end cost.

### gen2.5 status: BTB landed (third feature)

A **fetch-time branch target buffer** is implemented and committed — the
`penumbra2_btb` leaf (PC-indexed tagged target RAM, default 32 entries,
allocate-on-taken / tag-checked-invalidate-on-not-taken), the forked
integration (core lookup + redirect, `if2_stage` predicted-taken tag carried to
EX, spine training wiring, EX train/verify), unit test `tb_penumbra2_5_btb`, and
integration `test_btb.s`; gen2 untouched. A correctly-predicted taken direct
branch (B/Bcc/BL) is steered at fetch, turning its last front-end bubble into
none — below what ID-stage BTFN/RAS reach. EX stays the authority, so a stale or
aliased prediction costs at most an extra flush, never a wrong result.

**Measured (BTB off vs on, same gen2.5 build, both @ 25 MHz HW).** Dhrystone
DMIPS 7.29 -> 7.48 (+2.6%), CPI 3.05 -> 2.96, flush 9.2% -> 6.1% (-6.6M flush
cycles, -5.0M wrong-path I-fetches). The +2.6% roughly matches the whole
BTFN+RAS step (+2.5%), so the BTB doubles the prediction stack's Dhrystone gain
(7.11 -> 7.48, +5.2% total). The win is workload-concentrated: the hot libc
routines move 3-16% cyc/op (memset/256 -15.7%, memcpy/256 -6.2%, strlen/256
-3.4%), while the kernel syscall paths barely move (getpid -1.9%,
clock_gettime -1.8%, pipe_pingpong flat) — their flush is return/ifetch-bound,
not taken-branch-bound.

**Kept, but on probation against forwarding.** Both builds PASS at 25 MHz, so
the cycle win is banked; the BTB's cost is timing margin, thinned to ~25.11 MHz
on the default seed (placement noise on the shared dcache->MMU cone, recoverable
by the end-of-features floorplan — not chased per-feature). The standing risk is
that **forwarding — the #1 lever, loading the same cluster — may not close
25 MHz on top of the margin the BTB already spent.** If it cannot and the
floorplan cannot recover it, the BTB drops first: forwarding outranks it on
every workload. So retention is conditional on forwarding fitting beside it.

### gen2.5 status: forwarding landed (fourth feature)

Operand **forwarding + regfile write-through** is implemented and committed — the
`penumbra2_forward` leaf (youngest-first EX/MEM-then-MEM/WB select, the GPR
analogue of `penumbra2_flag_bypass`); the forked integration (ID carries
per-operand source tags + the relaxed issue interlock; EX instantiates the
forward network for both operands and *captures* the resolved value across an EX
hold; spine adds the WB->ID write-through mux and the MEM/WB forward source); the
cache-hot result-equivalence tests; and the `pinned-stalls` capability that skips
the gen2 cycle-exact perfctr tests on the variant. gen2 untouched. A reader now
issues as soon as its operand is *reachable* (EX/MEM at d=1, MEM/WB at d=2,
write-through at d=3) rather than stalling to WB; only the irreducible 1-cycle
load-use, plus the deferred SPR-file and divmul-aux cases, still stall. Mechanism
in `doc/internals/penumbra2/hazard-model.md` (gen2.5 forwarding section).

**Measured (forwarding off vs on, gen2.5, HW @ 25 MHz).** Dhrystone DMIPS
7.48 -> 10.16 (**+35.8%**), CPI 2.96 -> 2.17, hazard 30.6% -> 5.6% — the hazard
stall collapsed 7.3x (58.5M -> 8.0M cycles), exactly the #1 lever the corrected
profile named. By far the largest single-feature gain (the whole BTFN+RAS+BTB
prediction stack was +5.2%); total gen2 -> gen2.5 is now +43% (7.11 -> 10.16).

**BTB probation lifted.** Forwarding closes 25 MHz beside the BTB, so the BTB
stays — but timing is thin (a few `NEXTPNR_SEED` values needed to hit 25 MHz; the
capture adds two EX operand latches + an `i_fresh` mux on the operand path). The
end-of-features floorplan is the margin-recovery lever, still deferred until the
store buffer also lands.

**Three bugs, all hidden by an unrepresentative suite — the durable lesson.**
Forwarding only fires cache-hot (uncached fetch spaces instructions past the
pipeline depth, so the regfile always serves the operand), and the one cached
test, `memcpy`, has a single memory op per iteration. So a green suite hid:
1. **WRSYS value bypassed the forward network** — its sysreg-write datum is read
   from the registered `idex_op_b` in the spine, never the EX muxes; a relaxed
   WRSYS wrote stale data and the TLB miss handler re-faulted forever. Fix: the
   WRSYS value operand keeps the conservative stall.
2. **Operand lost when held in EX past the producer's drain** — a consumer held
   by a downstream MEM stall (store burst / slow load) past its producer's WB
   retirement reverted to the stale registered operand. Fix: EX captures the
   forwarded value and retains it across the hold (seeded on the slot's first EX
   cycle from the registered ID-issue signal).
3. **divmul-aux = R0 over-match** — a divmul discarding its high half (Rdh = R0)
   made the aux-match leg stall every R0 reader, which the scoreboard ties valid;
   caught by the subset assertion (relaxed stall must imply the scoreboard
   stall). Fix: the destination-match legs skip R0.
Coverage is now cache-hot, warm-loop result-equivalence tests
(`hw/sim/programs/penumbra2/test_forward_*`, `isa/test_forward*`) spanning
ALU/load/store/divmul/SPR/sysreg producers x op_a/op_b/store-base/store-data/
indirect-target/SPR-read consumers x d=1/2/3 x held-across-drain. Takeaway for any
future bypass work: **test it cache-hot, with the shapes real compilers emit**
(varargs prologues, store bursts, switch jump tables, stack-reloaded returns) — a
cold-fetch suite does not exercise forwarding at all.

**Deferred (intentional).** SPR-file forwarding (ESR/EPC/SCRn — needs a
write-through on each backend) and divmul-aux (Rdh) forwarding keep the
conservative stall; cost is negligible (SPR-scratch is the TLB-miss spill path,
divmul is ~33 cycles).

**Where the work stood when gen2/2.5 was abandoned (post-forwarding,
HW-measured).** These were the next levers; none will be built — gen2/2.5 is
abandoned (see the fmax-closure round below) and gen3 picks up the same
problems from a fresh design. Retained as the record of where the profile
pointed:

1. **Cacheable store buffer** — with hazard crushed, store was the #1 stall
   (Dhrystone 26.1%, ~37M cycles — essentially unchanged in absolute terms, just
   the largest share of a smaller pie; getpid ~22%). The write-through round-trip
   to L2 was the lever — but it could not fit in gen2's remaining fmax margin.
2. **I-cache / L1<->L2 path** — `ifetch` is ~0 on Dhrystone but #1-2 on the
   fork/exec/pipe paths (16-20%), untouched by any landed feature. The levers
   were the overview's 8-16 KB I-cache target and/or the full-line L1<->L2
   transfer reshape (which also cuts the load/store miss tails).

### gen2.5 status: fmax-closure round (37.5 MHz)

A timing-closure round took the gen2 *family* to a **37.5 MHz** operating
point (up from 25 MHz). The ULX3S top's `CPU_HZ` makes the 25 → 37.5 MHz
flip these changes were buying; the flip itself lands once per-board
placement seeds are pinned. The limiter the round attacked was the floor
recorded above — the D-side memory-hit cone plus the cross-module
`L2 → fill_sequencer → arbiter → L1` chain — which a same-cycle
combinational verdict spread across the die; floorplanning cannot fix a
cross-module combinational path, so the fix is to register the boundaries
and shorten the cones.

**Shared-machine changes (gen2 and gen2.5 both).**

- **Fetch enable off the redirect cone.** `o_fetch_en` (→ MMU `i_a_req`,
  → I-L1 `i_en`) gated on `~hold` alone, with the `~i_flush` term dropped
  from IF2's `o_fetch_re`. This severs every redirect source from the
  fetch/MMU launch path; the redirect still writes the registered PC, so a
  redirect target launches one cycle later only when it coincides with a
  hold. CPI-neutral in practice.
- **L1 PLRU touched at fill engage**, off the L2-verdict path (the touch no
  longer waits on `i_l2_busy`). Approximate replacement metadata, so a
  one-cycle-different touch is at most a marginally-different eviction.
- **Fill-sequencer output stream registered.** The L2's per-beat
  data/busy fed the L1 array write + valid set combinationally; registering
  `{o_fill_we, o_fill_word, o_fill_wdata, o_fill_done, o_fill_fault}`
  breaks that cross-module path. Cost: +1 cycle latency per line fill,
  pipelined (throughput unchanged; ~1–2 % of instructions are fills).
- **L2 read verdict registered (`HIT_LATENCY=3`)** on `machine_penumbra2`
  (gen1's L2 instances keep 2). The combinational stage-1 hit verdict
  crossed into the L1 fill install + arbiter; an output flop registers it
  before it leaves the L2. Cost: +1 read-hit cycle and back-to-back L2
  reads serialise (fill-throughput hit on L1 misses, recoverable by the L2
  read-pipeline decouple). Detail in
  [the L2 cache design](internals/l2-cache.md#pipeline-hit_latency3-the-gen2-machine)
  and [the gen2 memory interface](internals/penumbra2/memory-interface.md#deferred-fill-speed-directions).
- **L2 read-hit PLRU touch deferred** one cycle (captured at the hit,
  applied against the live array next cycle), off the L2 tag-compare path.
  A read hit never changes state, so the deferred apply always lands back
  in `S_IDLE`; no correctness impact.
- **L1 associativity 4-way → 2-way** on the gen2 machine
  (`machine_penumbra2.sv` `NUM_WAYS=2`; `cache_bram_vipt` still supports
  {2,4} and defaults 4). The parallel per-way tag compares + way-mux were a
  larger share of the hit-verdict cone than expected; halving them shortens
  it and relieves per-way-BRAM routing congestion. Cost: more conflict
  misses (a 2-way index holds half as many lines) — the higher clock more
  than offsets it on Dhrystone.

**gen2.5-only changes.**

- **BTB training write registered.** `o_btb_update` + payload {pc, target,
  taken} were combinationally gated by the dcache hit (via the MEM stall) —
  the same pattern as the old `predict_redirect`. BTB training is
  speculative bookkeeping, so it registers in EX/spine and writes the BTB
  next cycle. No correctness or IPC cost.
- **ID-stage predictor (BTFN + RAS) removed.** `penumbra2_predict` and
  `penumbra2_ras` and their ID→EX redirect/tag ports are gone. The
  predictor's redirect folded in the MEM stall, putting
  `dcache tag → dmem_busy → id_issue → predict_redirect → if2_flush` on the
  gen2.5 critical path. Lifting it off the stall by registering it would
  fire it the cycle the branch is already in EX — the same edge EX resolves
  it — so a registered ID prediction is redundant with EX's own redirect:
  zero cycles saved. Direct branches stay predicted at fetch by the BTB;
  everything the BTB misses (cold direct branches, every indirect jump /
  return) resolves in EX. Result-based `test_btfn` / `test_ras` pass
  unchanged (EX confirmed everything anyway). Cost: returns lose their
  one-cycle prediction and take the full EX-resolve penalty — the accepted
  tradeoff to clear the path. The BTFN/RAS landing measurements are
  retained above under their status entries.

**Open at abandonment.** Recorded only as where the design was left when work
stopped — not as planned work; gen2/2.5 is abandoned:

- Registering the L2↔external-bus boundary (the next fmax step) was blocked by
  a suspected SDRAM-adapter deadlock on gapped reads: a register slice there
  turns a line fill into gapped single reads, and the adapter's
  speculative-prefetch FSM deadlocks on that stream (`test_l2_ifetch` hangs).
- The `HIT_LATENCY=3` register cost back-to-back L2 read throughput; an L2
  read-pipeline decouple would have recovered it.
- Removing the RAS gave up return prediction; only a fetch-domain return
  stack (predicting in the fetch domain, never reaching back from ID) could
  have restored it timing-safely.

**The design is a failure and abandoned.** None of the above will be pursued.

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
  the machine-shaped probe (it exposes the IF2 tag-compare/way-mux path
  that gated Decision 11's L1-associativity choice; the fmax-closure
  round resolved it to 2-way — see
  [gen2.5 status: fmax-closure round](#gen25-status-fmax-closure-round-375-mhz)).
- A bus-fault return path through L2/sequencer/arbiter/L1 — **done.**
  Unlike gen1 (no-device wired straight into the core as a sideband),
  the gen2 core is decoupled from the bus by the arbiter's registered
  request and the L1's S_BEAT capture, so the fault must ride each
  layer's completion inward: a `fault` companion travels with the
  busy-drop of a single beat and with the fill-done of a line transfer
  (a faulting beat mid-line aborts the fill). It enters the core on
  both the data (MEM) and fetch (IF2) paths and vectors to
  `VEC_BUS_FAULT` with the faulting vaddr in FAULT_ADDR. The no-device
  fault is the *absence of any slave's claim*, not a fabric-side map:
  each sim slave (`boot_rom` over ROM, `sdram_sim` over RAM) claims only
  the addresses it backs via its own `bus_devsel`, and the fabric
  faults an access nothing claims — matching how the external async
  bus works (ranges are autoconfig-discovered, so no master/fabric can
  hold a static map). An unclaimed address traps rather than alias-
  responding. Tests: `isa/test_bus_fault`, `test_bus_fault_mmu`,
  `test_bus_ignore`, `test_bus_fault_fetch`. The `bus` capability split
  into `bus-fault` (the fault path, now on gen2) and `bus` (the
  SYSDEV_BUS device, gen1-only). Deferred: a fault on L2's *own* line
  fill while L2 is enabled — the only unhandled corner, asserted loud
  in `l2_cache.sv` (reachable only with L2 enabled AND a cacheable
  mapping to an unclaimed address; pass-through faults are the live
  path and abort correctly).
- The remaining capability gaps vs gen1's runner: wrspr, timer, uart,
  machid, `busctl` / the SYSDEV_BUS device — all done (the device lives
  in `machine_penumbra2`, exposing `o_bus_rst` / `o_bus_cfg_en`; gen2
  advertises the `bus` capability and `test_busctl` passes). Remaining:
  the autoconfig daisy chain those bits drive — no conformance test
  exercises it, so it lands with the wrapper-level device-discovery / SD
  path, where `machine_penumbra2_sim` grows an `autoconfig_dev` chain off
  `o_bus_cfg_en`. RAM probing is unblocked by the bus-fault path.

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

## Hardware: Penumbra/2 SDRAM-backed sim + EX-frontier interrupts — done; future work

The gen2 RTL sim now drives the **full SDRAM stack** (`boot_rom` over
the ROM region; `sdram_sim` — adapter + CDC + controller + sim PHY +
behavioral W9825 — over RAM, decoded by `bus_devsel`), dual-clocked at
the 4:1 ratio, instead of the never-stall `unified_bus_mem` (89b7ca7;
`unified_bus_mem` survives only in the FPGA timing-probe top). The
conformance suite runs against realistic variable-latency stalls, which
is the point: it immediately surfaced a real interrupt bug a 1-cycle
memory could not.

That bug and its fix (a775e4f): gen2 recognized interrupts at the fetch
boundary and captured EPC from the *speculative* front-end PC, so a
timer IRQ during a cache fill saved a wrong-path EPC and an interrupted
loop resumed past its branch. The interrupt is now a **synthetic fault
the EX stage tags onto its instruction** — it rides the precise-fault
path (EPC ← its own resolved PC, access suppressed in MEM, register
write dropped at WB, re-execute after ERET); the drain FSM, the
fetch-stop, and the boundary-PC capture are gone. The structural payoff:
the EX/MEM boundary *is* the issued/not-issued line for a memory access,
so cutting the interrupt there makes a non-idempotent access
non-speculative for free — no L1 gate, no store buffer. Regressions:
`test_cache_memcpy_irq` (cacheable fill) and `test_uncached_memcpy_irq`
(uncacheable access).

Future work this opened up:

- **External bus bridge + async no-device watchdog.** The combinational
  `(re|we) & ~(OR of selects)` no-device fault is only the *sync* on-chip
  form. A real external/discrete bus has no central decoder, so its
  no-device fault is a no-acknowledge timeout in a bridge — itself an
  autoconfig device fronting a secondary bus. The model is specified in
  [`bus-protocol.md`](../hardware/bus-protocol.md) (e65a1d9); the bridge
  and watchdog themselves are unbuilt.
- **PTE `S` (speculatable) bit — finalize semantics.** Bit 1 (`0x02`) is
  reserved and earmarked (11dfdc8) for an uncacheable-but-idempotent
  override (the boot ROM is the motivating case), with semantics left
  open until the speculation model settles. Not load-bearing today — the
  EX/MEM boundary provides the temporal non-speculation; `S` is a future
  perf lever (it would also ungate the SDRAM adapter's `addr+N` prefetch
  for such regions).
- **CPU-side store buffer (perf, not correctness).** gen2 writes stores
  through in MEM; the EX-frontier cut keeps that correct (cacheable
  replay is idempotent, uncacheable is non-speculative). A store buffer
  would decouple stores from MEM and enable write coalescing — a
  throughput win, deferred. Distinct from the L2 write buffer below.
- **gen2-on-FPGA validation — done.** The gen2 board top
  (`ulx3s_penumbra2_top`) synthesizes, flashes, and runs Dhrystone on the
  ULX3S; both clock domains close timing. fmax findings below.

## Hardware: Penumbra/2 CPU fmax — critical-path findings

gen2 targets 25 MHz CPU / 100 MHz SDRAM on the ULX3S (ECP5-85F sg6).
Unlike gen1's single full-instruction cone, the pipelined core's fmax
floor is the single-cycle **memory-hit cone**: TLB translate (the 2-way
associative match in `penumbra2_tlb_perm`) feeding the VIPT L1 tag compare, once on
the fetch side (port A) and once on the data side (port B). Restructuring
that cone is the gen2 fmax story. Operating point after the work below:
~34 MHz CPU (the D-side memory-hit cone is the structural floor — see
"Campaign outcome" below), ~117–123 MHz SDRAM (single-synth snapshots;
they jitter run-to-run).

### SDRAM domain: floorplanned to recover margin — DONE (`08cbd63`)

The SDRAM controller closed only ~101 MHz (1.5 % over its 100 MHz target)
where the identical RTL reaches ~140 MHz on gen1. The cause was
geometric, not logical: the denser gen2 core fills the central fabric and
crowds the controller into a thin 4-column sliver against the right-edge
SDRAM pads, so its internal routes stretch. A nextpnr `--pre-pack` region
floorplan (`ulx3s_penumbra2_floorplan.py`) corrals `u_sdram_ctrl` +
`u_sdram_cdc` into a compact block by the pads → ~117–127 MHz.

Two non-results, recorded so they are not re-tried:
- **Over-constraining** `clk_sdram` via `FREQUENCY NET` did nothing.
  nextpnr's analytical placer always minimizes its cost — it does not ease
  off a met target, so a tighter number only relabels PASS→FAIL.
  Over-constraining helps a path that is *under-prioritized*, not one
  already getting full attention.
- Regions are not expressible in nextpnr's LPF (`LOCATE COMP` only), so
  floorplans are Python `--pre-pack` scripts. Design-intent constraints
  live in a layered overlay (`ulx3s_penumbra2_design.lpf` + the pre-pack
  script), never the vendor board LPF — a file that can only speak
  REGION/FREQUENCY cannot misclaim a pin.

`keep_hierarchy` on the TLB cone + pipeline stages (`245aff2`) both made
timing reports legible (real net names, not post-flatten gibberish) and
recovered placement the dense core had scattered.

### CPU domain: the stall ripple, decoupled — DONE (`b5cb675`)

The CPU critical path was a **combinational stall ripple** spanning the
whole pipe: a load/store's TLB + D-cache hit check drives `i_dmem_busy`,
which propagates `mem_stall → ex_stall → id_stall` back to the fetch
enable in a single cycle (pure-stall back-pressure). About half of it was
routing — the stall wire physically crossing the die from MEM to IF1.

Fix: a 2-entry elastic FIFO (`penumbra2_fetch_buffer`) at the IF2→ID seam.
IF2 now back-pressures on the buffer's *registered* `o_enq_ready` instead
of `id_stall`, so the back-end stall no longer reaches `o_fetch_en`
combinationally. Cost: one IF2→ID cycle — an extra wrong-path slot,
flushed with the front end on the shared `if2_flush`. The buffer is
generic + unit-tested; the integration passes the gen2 conformance suite.
Sim + synthesis only — not yet flashed-and-run on the board.

### Standing lever: the I-side PLRU update sits on the hit path

After the buffer, the limiter relocated to the symmetric *fetch-side*
cone — proof the buffer did real work, not placement noise. The path
(31.29 ns, `a_asid_q → … → u_icache.plru[*].CE`):

```
  TLB port-A match cone (u_perm_a)     9.3 ns
  TLB → MMU paddr                       1.7 ns
  I-cache tag compare + way-mux         7.4 ns
  PLRU replacement-bit update          12.4 ns   ← 40 %
```

The surprise: the **PLRU update is 40 % of the path**. In
`cache_bram_vipt`, the per-set tree-PLRU is touched on a read/write hit in
the *same cycle* as the hit, so `rd_hit_resolve` (= the deep `hit` signal)
gates the write-enable of the per-set replacement registers and fans the
hit cone across all 64 sets. But PLRU bits are metadata consumed only on
the *next miss* (victim pick), and PLRU is approximate — they have no
business in the hit cycle.

**Lever — defer the touch (DONE, `e9ad4b6f37b5`).** Register
`{touched, set, way}` at the hit; apply
`plru[set] <= plru_update(plru[set], way)` the next cycle against the
*live* PLRU array. Design points to get right:
- back-to-back touches to the same set must compose (apply against live
  `plru[set]`, not a snapshot — then the second update sees the first);
- a fill-completion vs deferred-touch collision at the apply cycle needs a
  priority rule;
- cost is one access of staleness on a victim pick — invisible in hit
  rate, since PLRU is already approximate.

It lives in the shared `cache_bram_vipt`, so it helps the D-side cone too.
The ~19 ns / ~50 MHz projection did **not** hold: the defer worked but
exposed co-equal masking paths, and the real floor is the D-hit stall cone
at ~29 ns / ~34 MHz. See "Campaign outcome" below for the full arc.

Caveat on the rollup, same as gen1: nextpnr attributes fused
post-flatten LUTs by net-name prefix, not dataflow, so per-module labels
can mislead (the I/D `penumbra2_tlb_perm` instances especially). Trust the hop trace
and the start/end points.

### Campaign outcome: the D-side memory-hit cone is the floor (~34 MHz)

Past the stall-ripple and PLRU work above, the limiter walked through the
rest of the memory subsystem; the deferrals/retimings that landed (all
gen2 conformance-clean, sim + synth):

- **Main TLB → async LUTRAM + verdict register** (`c7d7d7e0`): the BRAM
  TLB forced the whole translate cone into the resolve cycle; async
  distributed-RAM storage (the gen1 `tlb.sv` recipe) runs it
  combinationally in the launch cycle, verdict registered once at the MMU
  output. Collapsed three register sets (BRAM read, pinned alignment,
  query capture) into one. CPI-neutral (same 2-cycle contract). The gen2
  MMU stack was subsequently renamed off the misleading `_bram` suffix and
  moved to `penumbra2/` (`penumbra2_mmu` / `_tlb` / `_tlb_unit` /
  `_tlb_perm`).
- **L1 fill install deferred** (`cb6a4452`): tag/valid/PLRU install moved
  off the L2-hit-driven `i_fill_done` into the already-existing S_SERVE
  cycle. Zero-cost — nothing in S_SERVE reads the freshly-installed line.
- **MMU port-A translate degated** (`f1203d58`): after the async TLB,
  port A's lookup-enable (`i_a_req && mode`) put the late I-side fetch
  request — carrying the IF2→IF1 back-pressure off a busy I-cache — on the
  verdict cone. The per-request gate was redundant with the verdict
  register CE, so dropped for port A (no sysreg duty; read address is
  always the PC). Port B keeps it: `i_b_lookup_en` also selects the
  port-B readback address and backs the contention guard. CPI-neutral.
- **Arbiter transaction-aware completion** (`aa55a768`): defer only the
  line-fill re-grant (beats stay back-to-back), keeping the L2 hit verdict
  (`i_fill_done`) off the arbiter's same-cycle re-grant path. Cost: +1
  cycle per back-to-back line fill (≈0 compute-bound, ~7–10 % pure
  streaming; recoverable later by a wider L1↔L2 datapath). Worth ~1.9 MHz
  at the final config — without it the L2→arbiter path caps fmax at
  32.35 MHz. (It was briefly reverted on the mistaken read that it bought
  nothing — see the first lesson below.)

Net fmax: ~33.5 → ~34.2 MHz (≈ +2 %). The headline barely moved; the big
intermediate swings (down to ~31.5, back up) were mostly the placer
redistributing near the wall. The durable value is the structural cleanup
and knowing where the floor is.

> **Superseded by the fmax-closure round**
> ([gen2.5 status: fmax-closure round](#gen25-status-fmax-closure-round-375-mhz)):
> the floor below was measured with a 4-way L1 and the L2 read verdict
> combinational. Halving L1 associativity to 2-way shortened the tag-compare
> leg, and registering the L2 read verdict + the fill stream lifted the
> cross-module limiter off this cone — the gen2 family then closed 37.5 MHz.
> The two-halves analysis still describes the *shape* of the residual D-side
> cone; the absolute numbers predate those changes.

**The floor** is the D-side memory-hit cone — D-cache 4-way tag compare →
`dmem_busy` → stall network → ID `drain_commit`, ~29 ns — in two halves,
both fundamental:
- cache hit determination ~10 ns: BRAM tag clk-to-Q + the 4-way compare,
  routing-bound across the per-way BRAMs;
- stall→commit tail ~12 ns: pure-stall requires the hit verdict to gate
  commit the *same* cycle; the flatten removed the ripple, but the signal
  still crosses MEM→spine→ID.

Reaching this floor needs the arbiter change above; it is otherwise not
deferrable — the compare is a BRAM read (L1-D tags ~5.5 Kb could go async
LUTRAM, but the compare is bounded by the registered paddr arriving at
resolve, so it would not help), and the stall→commit gate is the in-order
pure-stall tax.

**Lessons (do not re-run this campaign blindly):**
- Re-measure a change's fmax value at the *final* config, not by its
  mid-campaign delta. The arbiter measured as "noise" (33.87→33.44) while
  a co-equal path masked it, but became worth ~1.9 MHz once port-A cleared
  that path — which is why reverting it dropped fmax to 32.35, not the
  ~33.9 the stale delta implied. A fix is worth the gap to the *next*
  path, and that gap grows as you clear the ones above it.
- Performance is fmax × IPC; ceiling you don't clock into is worthless —
  fix the operating clock first, then judge CPI-for-fmax trades against
  it. The arbiter's +1/fill is worth it *here* only because headroom is
  the goal; at a clock far below the ceiling it would be pure loss.
- The deferrals cleared *masking paths* — incidental cones near the floor
  that retime away. The D-hit cone was always there; it surfaces once the
  masks are gone. Targeted fixes converging on a path present from the
  start = the floor, not another obstacle.
- Deeper fmax needs a microarch change (forwarding / non-blocking loads;
  OoO), not more single-cycle deferrals. Forwarding + branch prediction
  will reshape the EX/MEM/stall/commit region — exactly the floor — so
  budget those designs timing-aware here; don't pre-optimize the stall
  tail, it will be rewritten.
- nextpnr labels fused LUTs by net-name prefix, not dataflow: "the path
  goes through X" repeatedly meant a LUT *named* X (`wrsys_resync`,
  `ac_spi_sel` were registered/unrelated signals), not X's logic. Confirm
  against the RTL before trusting a per-module label.

Operating-clock note: the campaign cleared headroom to bump the CPU PLL
from 25 → 30 MHz (≈ 14 % margin under the ~34 MHz floor, room for the
forwarding/BP logic to eat into). That PLL change + a flash-and-run on the
board is the remaining step.

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

## Libc: string-routine optimization — strlen word-at-a-time, strcmp/strcpy unrolled (DONE)

The shared string routines in
`common/lib/libc/arch/penumbra/string/{strcpy,strcmp,strlen,memcmp}.S`
are byte-at-a-time.  On Dhrystone they are not a minor cost — they are
most of the program.

Wiring the bare-metal benchmarks to these routines (replacing hand-rolled
byte-loop `string.c` helpers) moved gen2 HW Dhrystone 5.03 → 6.32 DMIPS,
entirely from fewer retired instructions — CPI was flat (3.03 → 3.06).
That win was the word-at-a-time `memcpy` alone: Dhrystone copies one
~48-byte `Rec_Type` per iteration as a `bl memcpy`, and byte → word
fill cut it ~288 → ~93 insns.  The L1-D store delta confirms it
(0.75 × 48 × iters of stores removed).  But the *remaining* per-iteration
count stayed high, which prompted a full attribution.

Instruction-count profile — deterministic, ISS-traced over 100
steady-state iterations (architectural counts, not timing): **727
insns/iter.**  Method is reusable and noted at the end.

| function                          | insns/iter | share |
|-----------------------------------|-----------:|------:|
| strcpy (byte loop, 6 insns/byte)  |      189.0 | 26.0% |
| strcmp (byte loop, 9 insns/byte)  |      178.0 | 24.5% |
| dhrystone_main (incl. inlined Proc_1–5) | 138.0 | 19.0% |
| memcpy (word loop, *called* from Proc_1/3) | 100.0 | 13.8% |
| Proc_8 (2-D array index)          |       62.0 |  8.5% |
| Proc_6                            |       22.0 |  3.0% |
| Func_2 / Proc_7 / Func_1          |       38.0 |  5.2% |

The three mem/string library routines are **64%** of every iteration;
`strcpy`+`strcmp` alone are **51%**.

The dynamic opcode mix shows the shape: `add` 157/iter (22%, mostly
pointer bumps — Penumbra has no post-increment addressing, so every
copy/compare step needs an explicit `add`); `ldb`+`stb` 109 (15%, byte
traffic); compare+branch (`test`/`cmp`/`bne`/`beq`/`b`) ~200 (28%, the
byte-loop control).  The genuinely ISA-structural tax these loops get
blamed on — 2-operand `mov` (49/iter) plus address materialization
`lli`+`lui` (40/iter, no gp-relative addressing) — is only ~12%, not the
main cost.

Reference point: a typical RV32 Dhrystone runs ~331 insns/iter, but it
links a libc whose strcpy/strcmp/memcpy are word-at-a-time and inlines
the small constant-size memcpy.  Most of the 2.2× gap is library
algorithm, wearing the costume of an ISA gap.

Outcome (HW-validated):

Word-at-a-time was tried for all three routines, then kept only for
`strlen`.  For `strcmp`/`strcpy` it *regressed* Dhrystone (750 vs 736
insns/iter): Penumbra has no unaligned access, so the word path needs
both operands co-aligned, and Dhrystone's are not — `strcmp(Str_1_Loc,
Str_2_Loc)` compares two adjacent `char[31]` stack arrays the compiler
packs at byte alignment, so the word path never triggers and the
routine just paid its setup (align prologue + two `LLI`+`LUI` constants
+ callee-saved spills) for a byte fallback.

Cross-platform practice settles it: the strict-alignment arches (mips,
m68k, sparc32) keep `strcmp`/`strcpy` as byte loops; only x86 word-
optimizes them, and only because cheap unaligned access lets it align
just one side.  Penumbra is in the strict-alignment camp.  `strlen`,
with one pointer and always-reachable alignment, is word-at-a-time on
nearly every optimized arch.

Resolution:
- `strlen` — word-at-a-time, frame deferred past the align prologue so
  a head-NUL returns without spilling.  Zero-byte lane located by
  shifting the detector mask's 0x80 to the sign bit (Penumbra has no
  CLZ; cf. sparc64's byte-wise mask scan).
- `strcmp`/`strcpy` — byte loops, unrolled 4× with offset addressing
  (m68k precedent), redundant trailing branch removed.

Real-HW results (gen2 FPGA):
- Dhrystone: 6.32 → 7.11 DMIPS (+12.5%), −242 cycles/iter, from the
  `strcmp`/`strcpy` unroll.
- `strlen` length sweep (pbench): +11% on len-8 (setup cost), but −30%
  at 64 B and −41% at 4 KiB.  Crossover ~len 12–16.

The instruction saving (−94/iter) and cycle saving (−242/iter) are
consistent across ISS, RTL sim, and HW; only the baseline differs.
Caution learned the hard way: the prebuilt RTL sim was stale (an older,
slower gen2 whose retire counter over-counts flushed slots, reading
~4× the instructions), and under-reported the win as −2.4% where HW
showed −10.7%.  Treat a sim as ground truth only if it is current; the
FPGA always is.

These optimizations *appreciate* on gen2.5.  The cycle saving is a
roughly fixed amount of removed work; gen2.5's lower flush/hazard
stalls shrink the baseline it is measured against, so the same saving
is a larger fraction.  `strlen`'s word loop is itself hazard-bound
today — the `haszero` chain (`sub`→`and`→`and`) is a dependent ALU
sequence an in-order scalar core cannot overlap, so it runs ~1.7×, not
the 4× the stride implies; that gap closes as gen2.5 hides hazards.

Correctness for all three is covered by a bare-metal test
(`benchmark/strtest`) over every start alignment × length.

Still open: inline constant-size `memcpy` in the backend.  Dhrystone's
48-byte `Rec_Type` copy lowers to `bl memcpy` (~100 insns/iter incl.
call + prologue); inlining ~12 word load/stores would drop it to ~25.
Complements the word-fast routine, which still serves the variable-size
case.

Profiling method (reusable): trace a short run with `+trace`, find a
loop-carried PC in the measurement function (exactly one hit per
iteration), then bucket trace PCs by symbol address-range between two
mid-run occurrences of that marker.  This isolates steady-state from
one-time setup/reporting and yields exact per-function and per-opcode
instruction attribution.

Related: "Libc: memcpy misses same-offset misaligned shortcut" and
"Libc: cache-line-align the memcpy/memset hot loops" tune the same
routine directory along the variable-size and code-placement axes.

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

## Libc: cache-line-align the memcpy/memset hot loops

The 2026-06-13 pbench snapshot showed the small-size `memcpy` rows
(size=1/16/64, and `memcpy_align` n=7/31) regressing while small-size
`memset` *improved* — across a pure compiler rebuild that touched no
libc source.  Diagnosis: the routines are hand-written assembly
(`common/lib/libc/arch/penumbra/string/{memcpy,memset}.S`), so their
instruction bytes are fixed; only their *placement* moved.  The L1
I-cache is 1 KiB **direct-mapped** with 16-byte (4-word) lines, and
neither `.S` carried any `.p2align` — so a rebuild relocates the fixed
routine bytes and the hot loops land at new, arbitrary offsets relative
to the line grid (and to whatever caller they share a set with).  The
memset-up / memcpy-down anti-correlation is the layout-roulette
signature.

Cross-arch check: penumbra was the *only* port declaring these routines
with bare `.globl`/`.type`, bypassing its own `ENTRY()` macro.  Peers get
at least the macro's entry alignment, and the hand-tuned ports (sparc64,
aarch64, sh3) explicitly `.p2align` their inner loops — sparc64 literally
comments `.align 32  ! ICache align.`.

Fixed in commit f0a9e9e0ec9a: `.p2align 4` on the function entries and
the hot loops — memcpy `.Lword` + `.Lbyte` (the byte loop is the whole
copy when operands are misaligned, so it is hot), memset `.Lword` only
(its `.Lhead`/`.Ltail_loop` run ≤3 times).  The loops now land on
16-byte boundaries at every build, so hot-loop placement is
deterministic and the small-size rows stop swinging on unrelated
code-size changes — consistency was the goal, restoring the benchmark's
power to detect real libc regressions.

HW result (min trial, vs the regressed 06-13 baseline): the fix recovers
the iteration-scaling part of the regression, and recovers *more* the
more the loop iterates — memcpy size=1 8.19→7.50 us (~23%), size=16
12.14→10.52 (~43%), memcpy_align n=7 11.90→10.51 (~38%), n=31
17.75→16.40 (~46%), n=127 41.25→39.95 (~full).  memset unchanged.  This
is the predicted split: `.p2align` removes the per-iteration line
straddle (so the win grows with trip count); the residual small-size
floor is the per-call **cross-routine set conflict** (routine vs. caller
congruent mod 1 KiB in the direct-mapped index), which `.p2align` pins
only the low 4 bits and so cannot touch — only associativity can (the
4-way L2, the gen2 cache direction).

Follow-up — the *compiler* has the same gap for compiler-generated code;
see "Compiler: align functions and hot loops in codegen" below.

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

## Compiler: signed sub-word loads in relational compares miss LDBS/LDHS

Re-measured and re-scoped 2026-06-22. The original framing (sext through
a `G_PHI`, mirroring `penumbra_zextload_promote`) is **superseded**:
upstream's `combines_for_extload` (in the post-legalizer list) now folds
`G_SEXT(G_LOAD)` → `G_SEXTLOAD` for the single-use, through-`G_PHI`
(including divergent-merge), and mixed sext+zext-consumer cases — all
verified emitting `LDBS`/`LDHS` (or `LDB` + a single mask) via `llc`. The
eq/ne char compares that were most of the 2026-06-09 Dhrystone finding
fold to plain `LDB`, so the Dhrystone binary now carries **zero** adjacent
sext chains.

The residual gap is narrower: a **signed relational** compare of a
sub-word load inside a loop. The canonical
`while (*p > 0 && *a == *b)` still emits `LDB; SHL r,24; SAR r,24` for the
`*p > 0` test (the `*a == *b` eq part folds to `LDB`) — the legalizer
widens the signed `G_ICMP` with `G_SEXT` and `combines_for_extload` does
not fold it to `G_SEXTLOAD` in that loop-carried shape, where the
isolated shapes do fold. Why the in-loop relational case bails when the
isolated cases fold is the thing to diagnose before writing any rule.

**Measured opportunity (static, register-checked: a `ldb`/`ldh`
immediately followed by a width-matched `shl;sar` on the same reg, which
`LDBS`/`LDHS` collapses 3→1):** kernel 41 (3 byte + 38 half), libc.so 20
(all byte), `sh` 0, Dhrystone 1. A lower bound (non-adjacent foldable
sites exist), but the immediately-adjacent count is small and ~2
instructions each — **low value**, consistent with the original "low
priority" tag. Not a hot-path lever; pick it up only if the relational
diagnosis turns out cheap, or if a kernel bench surfaces one of these in
a hot loop.

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
  call.  The scheduler's real home is pipelined gen2; gate the
  enablement on the gen1/gen2 subtarget split (the
  SchedMachineModel-per-processor mechanism in that entry below) rather
  than turning it on target-wide.  Revisit for gen1 only if a kernel
  pbench A/B surfaces reducible pressure that Dhrystone lacks.
- **Tail calls**: `PenumbraCallLowering.cpp` hardcodes
  `Info.IsTailCall = false` (`// TODO: tail calls`).  Wrapper-heavy
  kernel code pays a full frame per hop.  Measured opportunity and a
  small first-cut plan are in the dedicated "Compiler: tail-call
  optimization" entry below — ~5% of static call sites, a footprint +
  call-overhead win (not a spill win), ≈0 on Dhrystone.
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
  (see the "Dhrystone hot-path code shape" entry below), that is real on
  a hot wrapper, not cosmetic.

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

## Compiler: G_GLOBAL_VALUE materialization defeats CSE and rematerialization

`PenumbraInstructionSelector` lowers `G_GLOBAL_VALUE` (and the
`@sym + offset` form) unconditionally to a raw `LLI rd, %lo ; LUI rd,
%hi` pair at selection time, in `emitLoadSymbolAddr` (the static/absolute
path; PIC takes the GOT path below). That eager two-instruction
expansion is a *double* defeat:

- **CSE can't see a shared base.** Each pair materializes the full
  `sym+offset`, so two references to the same symbol at different
  offsets — or two globals the linker happens to place near each other —
  look like unrelated constants. MachineCSE has nothing to share.
- **Rematerialization can't fire.** An address is the textbook remat
  candidate (cheap to recompute, no inputs), but remat works on a single
  def, not a two-instruction dependent chain. So when an address must
  survive a call, the allocator's only options are spill/reload or
  burning a callee-saved register — never "recompute it after the call,"
  which is usually cheapest.

A representative kernel sequence (hot path) materializes four addresses
that are all `copyright + constant` through four independent pairs — two
of them 11 bytes apart:

```
lli r1, 0xF1DD ; lui r1, 0x8027   ; copyright+0x13dc3
lli r2, 0x6D84 ; lui r2, 0x8028   ; copyright+0x1b96a
lli r3, 0x0A7E ; lui r3, 0x8027   ; copyright+0x5664
lli r4, 0x0A89 ; lui r4, 0x8027   ; copyright+0x566f  (11 B past r3)
```

Dynamic share on Dhrystone (new ISS `+opstats` instruction-class
histogram): explicit `LLI/LUI` is ~5% of executed instructions, but the
absence of base-sharing also inflates the immediate-ALU bucket (~30% —
offset arithmetic that would otherwise fold into a load) and, in
disassembly, the pattern is pervasive on hot paths.

### Instruction-count vs footprint: which layer moves which

The "Dhrystone hot-path code shape" entry above established that
I-footprint, not dynamic instruction count, gates HW Dhrystone —
count-*neutral* folds (the select-CMPi fold) measured flat because they
don't change which cache lines are touched. Be precise about which layer
of this fix is count-neutral and which actually removes instructions:

- **Layer 1 is count-neutral by construction.** `PseudoMOVADDR` expands
  back to the identical `LLI+LUI`, so the static instruction stream is
  unchanged unless CSE or remat fires. MachineCSE already deduped eager
  `LLI+LUI` pairs in straight-line code, so layer 1 adds little there;
  its real win is *rematerialization* — trading a spill/reload (or a
  burned callee-saved register) for a recompute when an address is live
  across a call under register pressure. That is a register-allocation
  *quality* win, not a footprint win, and it only engages where those
  cross-call-live address sites exist. It is the concrete mechanism
  behind that entry's finding #1 ("register-pressure-aware
  address-arithmetic CSE").
- **Layer 2 is infeasible on Penumbra; layer 3 still removes
  instructions (corrected 2026-06-22).** The textbook RISC-V fold —
  materialize a `%hi` base and let the load carry `%lo` in its offset
  field (`LUI + LD %lo`, 2 insns) — does **not** apply: Penumbra's `LUI`
  *ORs* the high half in (`Rd = Rd | (imm16<<16)`,
  `instruction-set.md`; ISS `case 2`), it does **not** clear the low 16
  bits, so a `%hi`-only base would first need `MOV rd,R0` — `MOV+LUI+LD`
  is three instructions, identical to `LLI+LUI+LD`. A single isolated
  global load is therefore structurally **3 instructions** with no
  compiler fold to 2 (closing that would need a clear-low load-upper
  instruction — an ISA change, weighed against the discrete-logic
  constraint; the OR-`LUI` exists to make the `LLI`-then-`LUI` idiom
  work without a separate combine). What *does* remove instructions is
  **base-sharing**: with the layer-1 pseudo, MachineCSE materializes a
  reused base once and each access folds its *residual constant offset*
  into the load's simm16 field when it fits ±32 KB (the layer-3
  mechanism), collapsing redundant `LLI+LUI` pairs and offset `ADD`s.
  That helps clustered/reused globals, not the isolated single-access
  case — which is most of Dhrystone's near-use globals, consistent with
  the measured-flat result below.

**Measured 2026-06-18 (HW): layer 1 alone is flat on Dhrystone** — as
expected from the above. Dhrystone accesses its globals near their use,
not held across calls, so remat has nothing to engage and CSE already
captured the straight-line case. Layer 1's value is the *enabler*
(single SSA def that layers 2-3 and the allocator build on) plus
RA-quality on cross-call, register-pressure-bound code — kernel-shaped,
not Dhrystone-shaped. To see layer 1 move a number, measure a workload
with cross-call address liveness (kernel syscall / fork+exec benches).
Footprint only shrinks where a base is *reused* (base-sharing collapses
the redundant `LLI+LUI`) or carries an offset that fits the load's
simm16; Dhrystone's isolated near-use globals largely lack that shape,
so even the full fix may stay flat there. The per-load `%lo`-into-load
shrink once hoped for here is infeasible (OR-`LUI`, above).

### Fix — three layers (layer 1 enables; base-sharing + layer-3 offset-merge carry the footprint win; the RISC-V `%lo`-into-load fold is infeasible)

1. **Rematerializable materialization pseudo.** Stop emitting `LLI+LUI`
   at selection; lower the static/absolute `G_GLOBAL_VALUE` to a single
   `PseudoMOVADDR rd, @sym` (`isReMaterializable`, `isPseudo`,
   `Size = 8`), expanded to `LLI+LUI` in a *post-RA*
   `PenumbraExpandPseudoInsts`. Now it is one SSA def: MachineCSE shares
   identical bases pre-RA, and the allocator owns the cross-call
   keep-vs-recompute decision via remat — exactly how AArch64 never
   "carries" a base over a call (its `ADRP` is a rematerializable single
   instruction it just recomputes after the call). This one change fixes
   the kernel example and the cross-call carrying; the rest is gravy.
   Expand it in a *post-RA* pass (a new `PenumbraExpandPseudoInsts`, or
   the `PenumbraInstrInfo::expandPostRAPseudo` hook) — **not** at
   AsmPrinter time: the pseudo must survive register allocation as one
   `MachineInstr` for the allocator's `reMaterialize` to copy it. Note
   the existing PIC carriers (`PICLLI`/`PICLUI`/`PICADDPC`) expand inside
   `PenumbraAsmPrinter` because their operands are label-difference,
   MC-level expressions — that is the right idiom for *PIC*, the wrong
   one for this expansion site. RISC-V's `PseudoLLA` + post-RA
   `RISCVExpandPseudoInsts` is the matching template.
2. **~~Fold `%lo` into the load offset~~ — infeasible on Penumbra
   (corrected 2026-06-22).** The RISC-V form (`LUI + LD %lo`) assumes a
   clear-low `LUI`; Penumbra's `LUI` ORs the high half without clearing
   the low 16 (`Rd = Rd | (imm16<<16)`, `instruction-set.md`; ISS
   `case 2`), so a `%hi`-only base cannot be built in one instruction and
   the fold saves nothing — there is no symbol-`%lo`-into-load
   instruction-count win (see "which layer moves which" above). The
   surviving instruction-removal is the **constant-offset** fold under
   layer 3, *not* the symbol-`%lo` fold: after base-sharing materializes
   a base once, fold a residual constant offset that fits the load's
   simm16 field directly into the memory op (`LDW [base + off]`), else
   keep the `ADD`. The `isLegalAddressingMode` base+simm16 advertisement
   is still worth fixing for that — the same gap as the "[R0 + offset]
   absolute addressing" entry, which remains independently valid (R0 is
   hardwired zero, so a small absolute address rides the load's offset
   with no `%hi` at all). `selectAddrRegImm` already folds a *resolved*
   base+simm16 into the Format-M offset field for frame indices and
   constant GEPs; (a) confirm the Format-M memory-offset operand can
   carry a *symbolic* relocation for the offset-merge case, and (b) drop
   the `HasBaseReg`/base+simm16 reject in `isLegalAddressingMode`.
3. **Merge residual constant offsets** into the pseudo's symbol operand
   (`%hi(sym+o)`) and into memory ops, modeled on RISC-V's
   `RISCVMergeBaseOffset` — for offsets that can't fold into a load
   (out of range, or an address materialized into a register, e.g. a
   pointer argument as in the kernel example).

Note the cross-call case is **not** a job for a greedy merge pass — it
is a register-allocation cost decision (remat vs CSR vs spill), and the
pseudo is precisely what hands it to the allocator. Don't hand-roll
cross-call liveness in a MIR pass.

### Remat aggressiveness is per-microarchitecture, not a pseudo flag

Split *capability* from *policy*. The pseudo's capability attributes
(`isReMaterializable`, `Size = 8`, `hasSideEffects = 0`) are
generation-neutral and stay unconditional in TableGen — every Penumbra
core can recompute an absolute address. How *aggressively* the allocator
should exploit that is microarch-specific: remat duplicates the 8-byte
sequence, which is cheap to hide in gen2's 4 KB 2-way L1 but can evict a
hot line in gen1's 1 KB direct-mapped I$. That tuning is policy, so it
belongs in the per-subtarget scheduling/cost model (today a single
`penumbra1` `ProcessorModel`; `-mcpu` already selects it, default
`penumbra1`), gated behind a `SubtargetFeature` when a `penumbra2` model
lands — never baked into the pseudo's static flags. For the first cut:
enable remat, leave `isAsCheapAsAMove` *off* (it tells the sinker the
sequence is free and invites per-use-site duplication), and tune
conservatively for gen1; flip the policy via a feature bit when the
genN subtargets split. The same per-subtarget seam will host any later
cost-model divergence (branch cost, alignment padding, LSR weights), so
when `penumbra2` is added, audit these together rather than one flag at
a time.

### TLS and PIC are orthogonal

Only the static/absolute path changes. PIC (`G_GLOBAL_VALUE` → GOT load)
is a *load*, not a materialization — CSE-able but **not** rematerializable
(it touches memory), so it gets its own pseudo with different properties.
TLS (GD/LD/IE/LE) is model-specific (thread pointer, possibly a
`__tls_get_addr` call) and stays its own path. The pseudo model actually
*simplifies* the current selector tangle: emit a distinct pseudo per
relocation/TLS model and move the byte sequences into the post-RA
expander, the way RISC-V's `PseudoLA` / `PseudoLA_TLS_*` family does.

### Templates and tooling

- RISC-V: `PseudoLLA`/`PseudoLA`/`PseudoLA_TLS_*` in `RISCVInstrInfo.td`,
  the post-RA `RISCVExpandPseudoInsts.cpp`, and `RISCVMergeBaseOffset.cpp`
  are near-direct templates (Penumbra's static expansion is absolute
  `LLI+LUI` where RISC-V's is PC-relative `auipc+addi`).
- ISS diagnosis tooling added this round: `+opstats` (instruction-class
  histogram), `+branchstats` (branch-direction histogram), and
  `sw/tools/branch_ceiling.py`. Re-run Dhrystone with `+opstats` after
  layer 1 to confirm the immediate-ALU / `LLI`+`LUI` buckets drop.

### 2026-06-19 empirical re-measurement — Layer 2 doesn't translate; base-sharing is the static lever

A disassembly study of the just-built binaries (kernel `MINIMAL`, an
`ET_EXEC` executable, libc.so) reshaped the plan above. Method:
`llvm-objdump -d` annotates every `lui` with its resolved 32-bit value
(the disassembler tracks GPR state), so address-forming `lli+lui` pairs
can be bucketed by target. Filtering the kernel to values that land
inside the kernel image (`0x80010000`–`0x802c0000`) drops 32-bit
*constant* materialisations and leaves **41,590 true absolute address
materialisations**. Findings:

- **Same-symbol multi-offset is already optimal.** Struct-field and
  array-element accesses (`s.a/s.b/s.d`, `g[2]/g[5]`) already materialise
  the base once and fold each offset into the load
  (`selectAddrRegImm`): `2 + N`, not `3N`. Layer 1's CSE-able pseudo plus
  the existing constant-offset fold delivers the base-amortisation win
  Layer 2 was reaching for — and the base is the *exact* `&s`, so the
  hardware's signed-offset add is correct with no carry handling.
- **Layer 2's per-access `LLI+LUI+LD → LUI+LD` win cannot exist here.**
  That reduction assumes a one-instruction high-only base. RISC-V's `lui`
  *replaces* the low bits; Penumbra's `LUI` **ORs** (`Rd = Rd |
  (imm16<<16)`, encoding-table opcode `0010`, tied `$Rd=$Rd_in`), so a
  clean `hi<<16` base needs `LLI rd,#0; LUI rd,%hi` — two instructions —
  and folding `%lo` into the load buys nothing. Layer 2 ports the RISC-V
  *mechanism* but not its *benefit*. **Treat Layer 2 (the `%lo`-into-
  offset fold) as not applicable; do not implement it as written.**
- **A "load-upper-and-clear" (`LUIC`) opcode is a minor lever, not the
  fix.** It would enable `LUIC %hi_carry; LD %lo` (3→2), but only **11.2%**
  of kernel address materialisations feed a single direct `[Rd+0]`
  load/store where the fold applies (~4,654 sites, ~18 KB `.text`); the
  other ~62% use the address as a pointer/base, where `LUIC %hi; ADDi
  %lo` = 2 = the existing `LLI;LUI`, no gain. It also does **not** merge
  unrelated-but-close symbols (different symbols → different `%hi_carry`
  relocations that don't CSE at compile time). Encoding space exists
  (Format-L opcodes `1101/1110/1111` reserved); RTL is trivial and
  actually drops the `Rd` read/tied operand vs OR-`LUI`. But it must be
  *additive* — the `LLI;LUI` 32-bit-constant idiom needs OR semantics, so
  `LUIC` cannot replace `LUI` — and it needs a carry-adjusted `%hi`
  relocation. Verdict: defer; sequence after base-sharing (which already
  converts many `[Rd+0]` loads to `[base+off]`, overlapping its win) and
  re-measure.
- **Base-sharing of unrelated-but-close absolute addresses is the real
  static lever** — the `copyright+const` shape, e.g. `phys_bias`
  (`0x80297000`), `bootinfo_store` (`0x80297020`), `penumbra_bootinfo`
  (`0x8029802c`) each rebuilt from scratch in the same `lui 32809` page.
  Of the 41,590 materialisations, **40.9% are shareable under a
  conservative model** (cluster targets within 60 KB inside a straight-
  line run split on calls), 78.1% function-wide. This is an *upper
  bound*: holding a base live across a call-free region still competes
  for one of ~12 allocatable registers in a 2-operand ISA, so the
  realisable share is lower; the clean first cut is call-free,
  same-section clusters.
- **The transform is layout-neutral, no ISA change needed.** For two
  *local same-section* symbols `A`,`B`, the distance `B-A` is an
  assembly-time label difference (the assembler folds it — no
  relocation, no carry, and the data never moves). So a cluster of K
  references becomes `LLI %lo(A); LUI %hi(A)` (exact `&A`) + K loads at
  `[&A + (sym_i - A)]` = `2 + K` vs `3K`. This is GlobalMerge's win
  *without* GlobalMerge's data-relocation cache penalty — GlobalMerge
  physically coalesces globals into one struct, which measured −2.4%
  DMIPS on gen1 Dhrystone (fewer instructions but more misses in the
  1 KB direct-mapped L1 from the relocated data). Label-difference
  base-sharing moves no data, so that penalty never arises, on either
  generation. (Whether the original GlobalMerge penalty still holds on
  gen2's 4 KB 2-way L1 is untested and moot for this approach.)
  Cross-section clusters need a link-time relocation but stay
  layout-neutral.
- **Implementation shape:** a target MIR pass over the `PseudoMOVADDR`
  defs (Layer 1 already gives CSE-able single-def bases), modelled on
  `RISCVMergeBaseOffset` but extended from same-symbol to same-section
  cross-symbol. Open scope decision: same-section-local only
  (assembly-time diffs, zero new relocations — recommended first cut) vs
  also cross-section (needs a base-relative `R_PENUMBRA_*` and the
  linker). The register-pressure call belongs in the allocator, not a
  greedy merge — same caution as the cross-call remat note above.

`fold_classify`/clustering scripts used for these numbers are throwaway
disassembly miners (not committed); the durable artifacts are the
numbers and conclusions here.

## Compiler: PIC/GOT global access — non-preemptible direct addressing — RESOLVED

The high-leverage lever — PC-relative-direct addressing for
non-preemptible PIC globals — landed in `8c8c28df7f51`.
`PenumbraInstructionSelector::selectGlobalValue` now gates on
`!GV->hasExternalWeakLinkage() && GV->isDSOLocal()`: a non-preemptible
symbol materialises its address with `%pcrel` + a single deref, skipping
the GOT slot, the GOT load (which sat on the critical path — the deref
could not issue until it returned), and the startup `R_PENUMBRA_RELATIVE`
reloc. A non-zero offset folds straight into `%pcrel_lo16(sym+off)`,
which the GOT path could not do (a GOT entry holds the base symbol only).
Undefined `extern_weak` stays GOT-indirect even when hidden — it may
resolve to 0, which a `%pcrel` anchor cannot encode. Shape coverage:
`pic-dso-local.ll`; execution coverage: `pcrel-dso-local.c` in the
`make test-compiler-pic` suite.

**Measured on a fresh NetBSD `libc.so.12.220.1` (reloc histogram +
`.got`-range bucketing via `llvm-readobj --dyn-relocations`), against the
pre-fix 2026-06-19 baseline:**

| Metric | Pre-fix | Now |
|--------|---------|-----|
| GOT slots bound by `RELATIVE` (non-preemptible) | ~5,028 | **56** |
| GOT slots bound by `GLOB_DAT` (preemptible) | 237 | **237** |
| Total `.got` data entries | ~5,265 | **299** |

The non-preemptible GOT population collapsed ~99%. `GLOB_DAT` is
unchanged to the symbol — proof the change is preemptibility-safe: it
removed only the slots it was entitled to. Of the 2,291 `RELATIVE`
relocs left in the binary, only 56 land in `.got`; the rest are ordinary
`.data`/`.data.rel.ro` pointer relocs, never a GOT-codegen concern.

**Why this was the dominant cost (kept for context):** the GOT cost lived
in the `ET_DYN` shared libraries, where each global read was a
5-instruction GOT-indirect sequence (`lli; lui; add rX,pc; ldw(GOT);
ldw(deref)`) and the PC-anchored immediates made every site unique,
defeating CSE. Dynamic relocs ran ~20:1 `RELATIVE`:`GLOB_DAT`, i.e. the
large majority of GOT-bound symbols were non-preemptible and never needed
a slot. Executables (`ET_EXEC`) are a *separate* problem — their own
globals use absolute `lli+lui` (base-sharing applies), not the GOT; calls
into libc go through the PLT (`JUMP_SLOT`).

**Residual GOT slots, not worth pursuing** (the GOT cost itself is now
too small to justify more work):

- The **56** non-preemptible GOT slots that remain are symbols the
  compiler could not prove `dso_local` at compile time but the linker
  bound locally (default-visibility data globals, function-address
  takes). Below the bar for a dedicated pass.
- A **GOT/GP base register** for the preemptible remainder would target
  only ~293 slots, and the 2-operand ISA makes a pinned GP costly in
  register pressure.

**The follow-on lever is NOT closed by this fix — it grew.** Each of the
~5,000 now-direct accesses still carries its *own* per-site
`lli; lui; add pc` triple anchored to its own PC, so repeats do not CSE.
**PC-anchor sharing** (regional base-CSE: materialise one PC-derived
anchor per region, then `anchor + (sym - .Lanchor)` per access) collapses
those triples. It is **layout-neutral — it does not relocate data**, so
it is the *opposite* transform from GlobalMerge: the net-negative
Dhrystone result belongs to GlobalMerge (which packed hot scalars beside
a cold 10 KB array and thrashed the **gen1 1 KB direct-mapped** D-cache),
*not* to base-CSE, whose only cost is register pressure. That gen1 cache
number does not transfer to gen2 (4 KB 2-way) / gen3, which is where
userland runs. This is the same pass as absolute base-sharing for the
kernel + executables' own globals, differing only in base kind
(PC-derived vs absolute) — tracked under "G_GLOBAL_VALUE materialization
defeats CSE and rematerialization". Realized win is global-density
dependent (real code is far less global-dense than Dhrystone), so measure
on gen2/gen3 hardware before productionising.

**Priority across the remaining addressing investigation, by leverage:**
(1) regional base-CSE — one parameterised pass serving both PC-anchor
sharing (the ~5,000 direct PIC accesses above) and absolute base-sharing
for the kernel + executables' own globals (~41% of kernel
materialisations); layout-neutral, register-pressure-gated, measured on
gen2/gen3; (2) `LUIC` and other secondary folds that compose with it.

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

## Kernel/compiler: dedicate R12 (TP) to curlwp — DONE

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

### Kernel wiring — DONE (`b625d49e43af`)

curlwp is read from R12, scoped to `_KERNEL` in `cpu.h` (`_KMEMUSER`
keeps the memory-load macro so libkvm consumers are unaffected).  Four
boundaries keep R12 == curlwp whenever kernel C runs:

1. `locore.S` sets R12 = `&lwp0` before the first C call at boot.
2. `cpu_switchto` already saved/restored R12 via `pcb_context` (like
   SP/LR), so it threads across voluntary switches unchanged; it also
   already writes `ci_curlwp`.  No change was needed there — the
   originally-planned "additionally set R12 = newlwp" is redundant.
3. `cpu_lwp_fork` seeds the child's `pcb_context` R12 slot — the
   `*pcb2 = *pcb1` copy would otherwise leave the parent lwp there.
4. `_trap_common` reloads R12 from `cpu_info_store` after the trapframe
   save, before running C.  A userland trap arrives with R12 holding the
   user TLS pointer, which the trapframe save/restore preserves; the
   reload is unconditional (correct from both entry modes, cheaper than
   branching).

`cpu_info_store.ci_curlwp` stays the canonical copy — the trap-entry
reload and any cross-lwp reads still consult it.

### Measured benefit

A `curlwp->field` access drops from `lli`+`lui`+`ldw`+`ldw` (4) to a
single `ldw [r12+off]` — the load takes R12 as its base directly, so it
is 4→1, better than the 4→2 originally estimated — and survives calls
for free (R12 reserved).  Same-session pbench A/B on the ULX3S (min;
identical clang and rootfs, only the kernel source differing, libc rows
flat to confirm isolation): `getpid` 122.49→118.53 µs (−3.2%),
`clock_gettime` 227.33→197.90 µs (−12.9%, with much lower variance).
fork/`pipe_pingpong` are within noise — curlwp is a negligible fraction
of their millisecond-scale cost, and their swings track 1 KB
direct-mapped I-cache layout (the kernel is 8 KB smaller), not this
change.

## Hardware + kernel: local console (HDMI text-video + USB keyboard)

Design complete and committed as docs; implementation underway — the USB
host MAC's CRC generators are the first RTL to land, with the rest of USB
and all of text-video still to start. The goal is a standalone local
console — character-cell video out over GPDI/HDMI and a USB keyboard in —
so the machine needs no host terminal. NetBSD is the first consumer (boot
may stay on UART initially); a boot-ROM local console is a later,
well-defined follow-on.

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
- `usb_crc5` / `usb_crc16` — token and data CRC generators, the
  board-neutral MAC's first leaf cells (Galois LFSRs; CRC5 bit-serial,
  CRC16 byte-parallel for the one-byte-per-cycle datapath). The bare
  modules emit the remainder; the MAC adds the on-wire complement +
  bit-reverse. Reference-checked unit tests + a `sw/tools/usb_crc.py`
  model (`6251a87`, `b74a5ef`).
- `usb_bit_stuff_tx` — SIE transmit bit-stuffer: inserts a 0 after six
  consecutive 1s, with a consume-handshake (`o_stuff`) that back-pressures
  the serializer. Reference-checked unit test (`ca1b502`). Per-packet
  `i_init` re-seeds the run count with SYNC's terminating 1, the
  unstuffer's mirror (`768a244`).
- `usb_nrzi_encode` / `usb_nrzi_decode` — SIE line coding: a 0 toggles the
  line level and a 1 holds it (the decoder is the exact inverse). Each
  reference-checked; the decoder round-trips the encoder (`d2b915d`).
  Made line-true for chain composition (`de8bd71`): the encoder gains a
  per-packet `i_init` reloading its idle-J reference and holds `o_line`
  registered for a full bit time; both cells reset to idle J — a K-shaped
  seed makes the first post-reset sample decode as a phantom 1, which is
  SYNC's end marker.
- `usb_bit_unstuff_rx` — SIE receive bit-unstuffer: removes the stuffed 0
  after six consecutive 1s (`o_valid` drops on it) and flags a bit-stuff
  error (a 1 where the 0 was due → seam `o_rx_error`) in the same decision.
  Round-trip + error-injection unit test (`65dfccd`). Per-packet `i_init`
  re-seeds the run count with SYNC's terminating 1 (USB counts it), and
  `o_valid`/`o_error` are qualified by `i_en` for sparsely-paced
  consumers (`9f12a7a`).
- `usb_serialize_tx` — SIE transmit serializer: shifts each byte out LSB
  first across a byte handshake (`o_byte_ready`) above and the bit-time
  tick below, folding the stuffer's back-pressure in as `i_hold` so an
  inserted stuff bit holds the current bit instead of dropping it; goes
  empty on the final bit to keep the line bubble-free between bytes.
  Unit test across five pacings (gated tick + injected hold) (`dc8a57c`).
- `usb_deserialize_rx` — SIE receive deserializer, the serializer's
  mirror: shifts recovered bits in at the MSB so eight assemble into an
  LSB-first byte; `i_valid` ignores removed stuff 0s and `i_init`
  re-aligns byte boundaries per packet (a concurrent assertion guards
  that `i_init` never rides a data bit). Mirror-checked against the
  serializer's bit order + a cross-packet realignment case (`b9e9a8e`).
- `usb_oversample_rx` — SIE receive clock recovery: oversamples the
  resolved J/K line (5×/40× from `usb_pkg`) and locks a per-bit phase
  counter to NRZI edges, strobing the sampled level at each bit midpoint
  into `usb_nrzi_decode`. The edge lock re-centers to phase 1 and
  suppresses a sample that coincides with an edge, which is what holds
  lock at the 5× full-speed margin. `usb_pkg.sv` lands here for the
  PHY-internal constants (divisors, speed, J/K/SE0). Waveform-recovery
  unit test at both speeds ± edge jitter (`cfb3f07`).
- `usb_line_state` — SIE differential decode: resolves the D+/D- pair
  into SE0 / J / K / SE1, with the speed-dependent J/K swap (J is D+ high
  at full-speed, D- high at low-speed). Feeds the sampler its J/K symbol
  and the framing layer its SE0. Exhaustive unit test (`b72f044`).
- `usb_rx_framing` — SIE receive packet bracketing: SOP on the first K
  out of idle, SYNC end on its terminating 1 (an end-marker, not a
  full-pattern match, so hub-stripped SYNC still detects), payload
  gating to the unstuffer, EOP on SE0. Unit test incl. stripped SYNC +
  back-to-back packets (`6ac3503`).
- `usb_rx_test` — receive-chain integration DUT (line state →
  oversample → NRZI → framing → unstuff → deserialize), the shape of
  `usb_phy_<target>`'s receive half. End-to-end test drives raw D+/D-
  packet waveforms and requires the payload bytes back: both speeds,
  edge jitter, hub-stripped SYNC, stuffing edge cases, back-to-back
  packets (`6ac3503`).
- `usb_tx_framing` — SIE transmit packet bracketing, `usb_rx_framing`'s
  mirror: owns the bit-clock pacer (5×/40× divider — transmit generates
  timing, receive recovers it), feeds SYNC to the encoder, seeds the
  stuffer on SYNC's last bit, pumps payload until the serializer empties
  *and* the stuffer owes nothing (a packet ending in six 1s still gets
  its trailing stuff bit), then drives the SE0/SE0/J EOP raw below the
  encoder and releases. Unit test: phases, spacing, trailing stuff,
  re-arm (`1bba2b9`).
- `usb_tx_test` / `usb_loop_test` — transmit-chain integration DUT
  (serializer → stuffer → framing/encoder → line mux → registered pins,
  the shape of `usb_phy_<target>`'s transmit half) with a structural
  waveform testbench (SYNC pattern, exact stuffed payload, EOP shape),
  and the TX→RX loopback closing the loop over every SIE cell: bytes fed
  in come back out, both speeds, multi-packet (`1bba2b9`).
- **The SIE is complete**: all bit-, timing-, framing-, and line-level
  cells built, unit-tested, and proven end-to-end in both directions.
- Seam verified against the UTMI+ spec rev 1.0 (saved locally as
  `~/UTMI-PLUS-SPECIFICATION.pdf`) so a real ULPI/UTMI PHY satisfies it
  unmodified: opmode = the UTMI+ operational modes; bus reset = the
  HS-termination drive state (`xcvr_sel 00` + `term_sel 0`, SE0 by
  electrical result — FS/LS-only PHYs drive SE0 directly, caps carve-out
  documented); resume = opmode 10 + held `00h` data; LS keep-alive = the
  transceiver-decoded one-byte `A5h`; `line_state` = raw {D-, D+} with
  SE0 glitch filtering; `usb_speed_e` now carries the XcvrSelect
  encoding end to end (`6959ad2`, `0cba7d2`). `XFER_STATUS` gained
  `RXTOGGLE` — hardware ACKs CRC-good IN data and reports the received
  toggle; retransmission detection stays in software (`6959ad2`).
- **MAC — in progress.** Decomposition agreed, leaf-cell style, all on
  the 60 MHz clock (the CDC crosses later), build order as listed.
  Done: `usbhc_pkt_tx` — token/SOF/DATAx/handshake transmit, the
  on-wire CRC complement+reflect above the bare CRC cells, the bare-PID
  keep-alive transmit; PID codes in `usb_pkg`; golden-vector testbench
  anchored to `sw/tools/usb_crc.py`, including byte-per-cycle pacing
  (`0932a66`). Done: `usbhc_pkt_rx` — PID classify + check-nibble,
  packet body → buffer as received (CRC bytes included, per the DATA
  contract), CRC16-residual check, EOP-latched `{pid, len, ok, err,
  overflow}` (`87c0a57`). Next: `usbhc_txn` (token → [data] → handshake
  FSM, 16–18-bit-time turnaround timeout, host-ACK, RESULT
  classification; gates rx→buffer stores to IN transactions — a stray
  DATAx during an OUT must not scribble the TX payload), `usbhc_frame`
  (1 ms timer,
  FRAME counter, SOF/keep-alive request, never splits a transaction),
  `usbhc_port` (connect/speed detect — FS-polarity trick: idle-J ⇒ FS,
  idle-K ⇒ LS — debounce, reset/resume recipes, opmode/xcvr policy),
  then the `usbhc_mac` composition. MAC-level test drives the seam with
  a byte-level C++ device responder (the embryo of `UsbDeviceSim`; its
  keyboard input must NOT take terminal stdin — the UART console owns
  it — separate pty/socket, mechanism TBD).
- `usb_phy_sim` / `usb_phy_ecp5`: compose the SIE cells behind the seam
  (`usb_rx_test`/`usb_tx_test` are the two halves' shapes); `phy_sim`
  replaces the line layer with a byte-level packet port + connect/speed
  sideband exported through `machine_sim` like the SPI/SD model, pacing
  bytes at real bit-time rates so MAC timing stays honest.
- US2 wiring: RX diff on `usb_fpga_dp/dn`, TX on `usb_fpga_bd_dp/dn`,
  pulls on `usb_fpga_pu_*`; dual-clock-BRAM + handshake CDC
  (`usbhc_regs` + `usbhc_cdc`).
- `autoconfig_dev` wrapper (`CLASS_USBHC`); machine_sim integration test
  (poll CONNECT → reset → GET_DESCRIPTOR → R1) under `make test`.
- **ISS: model `CLASS_USBHC` at register level.** Driver bring-up (the
  NetBSD HCD, the ROM keyboard reader) needs the fast simulator — the RTL
  sim is far too slow for that iteration loop. Add the register contract
  (autoconfig + TOKEN/XFER/PORT/FRAME/DATA) to `sw/sim/penumbra_iss.cpp`
  with a behavioral device behind it; transaction-level only, no
  SIE/timing model. The byte-level device responder from the MAC test
  (`UsbDeviceSim`) should back both sims, including the
  keyboard-input-not-on-stdin constraint.

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
