# Penumbra/3 — What Was Not Built

> **Applies to:** gen3 (`penumbra3`) as finished.

gen3 stopped after the store buffer, branch prediction and stream
prefetch beside the L2
([where the build stopped](./overview.md#where-the-build-stopped)).
These are the changes it analysed and left, each with the evidence it
has, for a later generation to weigh: what it would change, what it
would save or cost, and what was never measured.

## Staged levers

The overview ranks every lever by what it saves on the shell loop,
`pbench kernel` and Doom `demo3`
([levers](./overview.md#levers-ranked-by-measured-benefit)). gen3
stopped before these:

- **The next-line prediction buffer for L1-I,** the largest left on
  the kernel paths. Its design and the cache model's counts are in
  [memory-performance](./memory-performance.md#next-line-prediction-buffer-for-l1-i);
  its buffer-hit and lookup cycles are estimates, as nothing built
  measures them. Its compare sits beside the fetch side's miss verdict,
  and the fetch side's cones set the CPU clock on most seeds of gen3's
  sweeps.
- **A 128 KB L2 on the ECP5,** about 35 more block RAMs. The Nexys
  Video builds 256 KB
  ([measured](./memory-performance.md#l2-size-and-stream-buffers-at-gen3s-geometry)).
- **Selective `WRSYS` serialization.** A 256×2 TLB leaves too few
  refills for it to pay much
  ([timing outline](./timing-outline.md#multi-cycle-and-serializing-instructions-divmul-wrsys-eret)),
  and it pairs with [a sysreg access the target paces](#sysreg-accesses-act-on-their-target-at-a-fixed-cycle).
- **Write combining in the SDRAM adapter and a native port for on-board
  RAM.** Both change the memory stack every generation shares, so they
  belong with its next revision
  ([write combining](./memory-performance.md#sdram-side-write-combining)).
- **A write-back L2.** Stores never wait on the final builds' Dhrystone
  and CoreMark ([final builds](./memory-performance.md#the-final-builds-on-hardware)),
  so the store buffer left write-back nothing to earn
  ([the store path](./overview.md#the-store-path)).
- **16 KB 4-way L1s on the Artix-7** (`doc/TODO/hw-nexysvideo.md`,
  Ruled out).

## A divmul's second write stalls the whole back end

The divmul's two-cycle WB writeback makes WB occupy two cycles, and
that hold (`wb_local_stall`) ripples up the pure-stall chain
WB->MEM2->MEM1->EX->ID. It is 1-bit and flop-shallow -- no cache/TLB
verdict in it, so it is correct and within the "1-bit ready crosses
backward" allowance -- but it is the longest-*reach* combinational
stall and may bind at 50 MHz on routing distance alone. Floorplanning
is NOT the remedy (placement cannot be the fix for a structural reach).

The structural fix is localized and does not touch the core shape:
retire the divmul writeback through the **load-completion path**
rather than the WB stage. divmul holds in EX until it is the oldest
in-flight -- `drained` = `~mem1_valid & ~mem2_valid & ~memwb_valid &
~mem_pending` (the drain-commit condition extended with `mem_pending`)
-- then writes Rd/Rdh over two cycles through the completion
write-port mux. The port is free **by the in-order single-outstanding
invariant** (oldest => no older writer; younger blocked behind the held
divmul), NOT by any cycle count -- so a slow uncached MMIO access ahead
of the divmul simply makes it wait in EX, never write early or out of
order. WB then never sees a divmul: the dual-write FSM and
`wb_local_stall` both delete, and back-pressure reduces to the
`mem_pending` fan-out plus the one-hop EX->ID stall.

Not built: no full-top sweep showed the reach binding
([where the build stopped](./overview.md#where-the-build-stopped)).
The WB dual-write is correct as it stands: WB is the oldest by pipeline
position, so it is already drain-gated, and only the reach is longer
than it needs to be.

## An exception's vector read always goes to memory

`penumbra3_vecfetch` reads the vector table uncached. It is the CPU's
only fetch by physical address, so threading a PA index into the VIPT
caches is awkward; the read has to bypass the MMU but not the caches.
Making it L2-cacheable saves a full SDRAM round-trip per trap.

Behind gen3's 256×2 TLB, traps are rare. Counted 2026-11-17 by the
ISS's `+opstats` at `+core=penumbra3`, over the windows of
[the load miss's cycle](#a-load-miss-holds-the-launch-side-a-cycle-past-its-done):

| Per 1000 instructions | shell loop | `pbench kernel` | Doom `demo3` |
|---|---:|---:|---:|
| Exceptions | 0.823 | 1.668 | 0.053 |
| … TLB misses | 0.600 | 0.603 | 0.049 |
| … system calls | 0.216 | 1.056 | 0.001 |
| … interrupts | 0.004 | 0.004 | 0.004 |
| … TLB protection faults | 0.003 | 0.005 | — |

At an estimated 10–20 cycles saved a trap, that is about 8–16, 17–33
and under 1 cycle per 1000 instructions: a kernel-path lever the size
of selective `WRSYS` (`doc/internals/penumbra3/overview.md`, "Levers
ranked by measured benefit"), and nothing for Doom.

Not built, and not measured: what a trap's vector read costs on gen3
against an L2 hit, which prices the saving.

## Sysreg accesses act on their target at a fixed cycle

A gen3 `WRSYS` lands in its device on the cycle it commits from EX,
and an `RDSYS` takes its response from the device in MEM2. Neither
lets the target say when it can take the access, so whatever the
target is doing in that cycle has to be reconciled in logic.

The main-TLB install shows the cost. A `TLB_PTE` commit writes both
copies of `penumbra3_tlb_store` while their read ports keep reading;
the I-copy is serving the fetches behind the `WRSYS`. yosys cannot
prove that the write and a read never meet on one set, so it keeps
the RTL's read-first result with a read-during-write bypass: the
write delayed a cycle through flip-flops, and a compare and mux on
the block RAM outputs, inside the verdict cone after the EBR
(`doc/internals/penumbra3/mem-stage.md`, "The MEM2 verdict cone").
`synth_ecp5` puts it at 134 LUTs and 137 flip-flops per copy: 313
LUTs and 203 flip-flops against 179 and 66 without it.

`penumbra3_tlb_store` gates each copy's read off in the install
cycle, and yosys drops the bypass. That is free only because no
overlapping lookup is consumed today: the `WRSYS` commits with the
pipe drained behind it, and the resync re-fetches its successor the
next cycle, discarding every lookup the install overlapped. It stays
free only while TLB installs serialize, and the refill path wants them
not to (`doc/internals/penumbra3/timing-outline.md`, "Multi-cycle and
serializing instructions (div/mul, WRSYS, ERET)"). A non-serializing
install makes the I-copy's overlapping lookup live, and that lookup
then has to wait a cycle rather than read the block RAM mid-write.

`RDSYS` has the mirror problem: the response is taken from the
device in a fixed cycle, so a device whose status is not already
registered puts its status logic in the core's result path.

Not built: a sysreg access the target paces. The target accepts a write
in a cycle of its choosing and answers a read from a register once
it is ready, so an install lands in a cycle with no lookup launched
and a status read leaves the device registered. This fits the gen3
rule that rare and slow operations pay cycles, never critical-path
logic.

## The L1's valid bits are most of its logic

`penumbra3_l1_store` keeps its 512 valid bits in flops, as gen2's L1
does, so that reset and `INVAL_ALL` clear them in one cycle. Synthesized
for the ECP5 on its own (yosys, out of context), the store is 1719 LUT4
with them and 509 without: about 1200 LUT4 and 512 flops for the write
decode, the clear and two 256:1 read muxes. The store bypass beside
them is about 80 LUT4. Each L1 pays this, and the PLRU bits in
`penumbra3_l1` are a second flop array of the same shape, not measured
apart.

Not measured: the only timing evidence is the P0.5 probe, which holds
both L1s whole and clears 37.5 MHz with 13–19 % slack over ten seeds
(`doc/internals/penumbra3/phase0-probes.md`, "P0.5 -- physical-tag
compare behind the TLB verdict"). That covers the cones through the L1,
not what the arrays cost a full machine's routing. No replacement was
looked at.

## A load miss holds the launch side a cycle past its done

A load's completion takes the MEM2/WB register in the cycle of the
L1-D's `done`, so the launch side resumes a cycle after the done rather
than with it (`doc/internals/penumbra3/memory-completion.md`, "Load
miss"). A miss with anything but a lookup of the filled set behind it,
an ALU op, a branch, an access to another set, pays that cycle alone.
When the load or store held in MEM2 behind the miss has its lookup in
the set the line went into, the L1-D's busy covers the same cycle, as
the L1 shows a held lookup an install a cycle late
(`doc/internals/penumbra3/l1-cache.md`, "Busy and done"). Every install
is a load miss's, so that cycle always falls in a completion's: taking
it out alone saves nothing.

memspeed shows the overlap. Each of its misses has a load of the same
line behind it, which paid the busy cycle before the completion moved
into MEM2/WB, and its Nexys rates at `7797ec8` match those at `3539275`
to the hundredth: 2.12 cycles a word from L2, 8.18 from memory.
Modelled on libc's `memcpy` word loop, where an add follows each load
and so holds no lookup, the cycle costs a quarter cycle a word: 6.25
against 6.00 from L2.

The fix takes the cycle out wherever the instruction after the miss
holds no lookup of the filled set. **The completion forwards in its own
cycle:** a leg of its own in EX's operand mux, its select formed from
the L1-D's `done` ahead of the youngest-first compare. A build of that,
swept before `854284a`, closed every domain on 2 of 10 seeds against 6,
but on 8 of them its critical path ended in the zero flag's chain that
commit removed, so the leg's own cost is not measured.

Counted 2026-11-17 by the ISS cache model at `+core=penumbra3` (an
8 KB 2-way L1-D), the instruction after each L1-D load miss. The
workloads are the multiuser image's shell loop and `pbench kernel`
(`doc/internals/penumbra3/memory-performance.md`, "Miss profiles from
the shadow models") and Doom `demo3` from its first
`R_RenderPlayerView` to `G_CheckDemoStatus`:

| Per 1000 instructions | shell loop | `pbench kernel` | Doom `demo3` |
|---|---:|---:|---:|
| Load misses | 10.09 | 6.39 | 12.91 |
| … then no access | 7.03 | 4.44 | 10.73 |
| … then an access to another set | 2.31 | 1.38 | 1.72 |
| … then an access to the filled set | 0.74 | 0.58 | 0.46 |

The leg takes the cycle out of 91–96 % of misses: about 9.3, 5.8 and
12.5 cycles per 1000 instructions. The model runs no clock, so it never
sees a bubble between a miss and the instruction after it, and an empty
slot holds no lookup either: the figures are floors. The rest, a
lookup of the filled set seeing the install at once, is ruled out
(`doc/TODO/hw-core.md`, "Showing a held gen3 lookup the install at
once").

Not built: the leg was never swept on top of `854284a`.

## A drain-commit's wait sits on the issue stall

ERET, WRSYS, EI and DI commit from EX, so EX holds one until nothing
older is unfinished. That test is an OR over EX/MEM1, MEM2, WB, a
pending access and its completion. It feeds `ex_local_stall` and so
ID's stall, which every instruction's issue reads, though only these
four need it. No measurement says what it costs: no full-top sweep has
been read without the gate, and a path's place in a sweep attributes
nothing.

Two other shapes avoid the gate. Neither is built.

- **A registered `drained`.** While a drain-commit waits in EX nothing
  enters behind it, so the back end only empties. A flop sampling the
  gate with the drain-commit already in EX therefore cannot go stale.
  It costs a cycle per drain-commit and no new flush logic.
- **Commit at WB.** The instruction flows down the pipe like any other
  and commits as the oldest, so there is nothing to wait for. Whatever
  entered behind it ran on the old state, so the commit flushes it,
  reusing the fault commit's flush and adding a redirect. Each one then
  costs a refetch where EX waits only for the back end, and they are
  not rare: `_real_miss_handler` runs at least three WRSYS and an ERET
  per TLB refill.

Not measured: the gate's weight in a full-top seed sweep, with and
without it, and how often real workloads run these four against what a
refetch costs. The test any shape has to keep is the `dc_*` group in
`tb_penumbra3_spine`.
