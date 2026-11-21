# Penumbra CPU — Instruction Timing: Mechanism and Measurement

> **Applies to:** all generations · reference. Where each core's cycles
> come from, and how the figures in the programmer's reference,
> [`system/instruction-timing.md`](../system/instruction-timing.md),
> are measured. The figures live only there; this doc explains them
> and says how to re-measure them after an RTL change.

## Where the cycles go

### Penumbra/1

The sequencer alternates `S_FETCH` and `S_EXEC`
(`hw/rtl/penumbra1/sequencer.sv`). The fetch completes in the first
`S_FETCH` cycle on an I-cache hit — the gen1 L1's zero-cycle
combinational hit ([l1-cache.md](penumbra1/l1-cache.md)) — and the
microcode ROM read is combinational, so an instruction costs one
fetch cycle plus one cycle per µop.

- **ALU operations**, register and immediate forms alike, are a
  single µop ([microcode.md](penumbra1/microcode.md)): 1 + 1 = **2**.
- **MUL / DIV** are three µops. Step 0 launches the divmul unit and
  stalls on `busy = divmul_launch | ~i_divmul_done | i_mem_busy`,
  which holds it through the launch cycle, the 32 iteration steps and
  the cycle `done` is first visible: 34 cycles. Step 1 writes the low
  half to Rd and latches the flags; a shared tail µop writes the high
  half to Rdh. The tail runs even for the two-operand form, whose
  Rdh = R0 write the regfile drops. 1 + 34 + 1 + 1 = **37**.
- **Branches and jumps** are a single µop: `Bcc` uses `branch=BRT`, on
  which the sequencer starts the next fetch whether or not the
  condition holds and only the PC source differs, so taken and not
  taken cost the same **2**, as does `JMP`. `BL` writes the link
  register in a µop of its own before loading the PC: **3**. There is
  no prediction, so the figures do not depend on history.

Penumbra/1 is serial, so no instruction ever waits on another beyond
its own µops, and nothing overlaps a control transfer.

### Penumbra/2 and Penumbra/2.5

An ALU operation occupies each stage for one cycle: **1**.

**Loads.** The MEM stage holds the pipeline for every access's
launch cycle, hit or miss (`mem_first` in
`hw/rtl/penumbra2/penumbra2_mem_stage.sv`), and advances in the cycle
the access completes, so a hit costs **2**. The loaded value enters
the forwarding network only from the MEM/WB register, a cycle after
that: a reader in ID of a load in EX waits one cycle
(`load_use_stall` in `hw/rtl/penumbra2_5/penumbra2_id_stage.sv`). The
launch-cycle freeze does not cover that wait — during it the consumer
makes no progress — so the two costs add. Penumbra/2 has no
forwarding at all: a reader waits until its producer leaves WB
([hazard-model.md](penumbra2/hazard-model.md)).

**Stores** take the same launch cycle, then wait in MEM until the
write-through is accepted below the L1: about three cycles of L1,
arbiter and MEM hand-offs and about three of the memory system's own
handshake, which on the ULX3S SDRAM stacks releases a write as soon
as its clock-domain crossing accepts it
([memory-performance.md](penumbra3/memory-performance.md#store-latency-below-the-arbiter-chain-b)).
That second part is why the store figure is typical, not a fact.

**Multiply and divide** hold EX for the divmul unit's launch-to-done
cycles — a divide's launch, 32 steps and done cycle make 34, and the
multiply retires four bits per step, so its 8 steps make 10 — on top
of the instruction's own cycle: **35** and **11**. Rd and Rdh are
written through the regfile's single write port on consecutive WB
cycles ([hazard-model.md](penumbra2/hazard-model.md#interaction-with-divmul)),
which is why a reader of Rdh waits and, on Penumbra/2.5, a reader of
Rd does not.

**`WRSYS`** serializes: the pipeline drains before the write and
refetches after it.

### Control transfers on Penumbra/2.5

The front end runs IF1 (PC, fetch launch) → IF2 (I-cache tag compare)
→ a two-entry fetch buffer, which adds one IF2→ID cycle → ID → EX, and
EX resolves every branch (`hw/rtl/penumbra2_5/penumbra2_core.sv`).
Fetch continues sequentially unless something steers it, so a
not-taken conditional branch costs **1**.

The branch target buffer (`penumbra2_btb.sv`) is a 32-entry
direct-mapped table indexed by `PC[6:2]` and tagged with the rest of
`PC[31:2]`, holding the target of a direct branch (`B`, `Bcc`, `BL`).
Its read launches with the fetch in IF1 and is compared in IF2. On a
hit, as the branch moves into IF2, IF1 is steered to the cached target
and the sequential fetch IF1 launched that cycle is discarded: a
correctly predicted taken branch costs **2**, the branch plus that one
discarded fetch.

EX trains the table from every resolved direct branch, through a
register: a taken branch allocates or refreshes its entry, a not-taken
one clears its entry if the tag matches. An entry's presence is the
prediction, so each branch site predicts the direction it last
resolved.

EX redirects the front end in the cycle a branch resolves against its
prediction — a taken branch with no entry, a predicted branch that
falls through, and every indirect jump and return, which the BTB never
holds. The instructions fetched behind it in IF1, IF2, the fetch
buffer and ID are discarded: **5**, the branch plus four slots.

### Stalls beside a predicted taken branch

The fetch buffer keeps fetch running while the back end is held. A
predicted taken branch's discarded fetch is a bubble in the front
end; when a load, store, `MUL` or `DIV` one or two instructions ahead
of the branch holds the back end while that bubble is still in the
front end, fetch fills the bubble during the stall and the branch's
second cycle disappears. Three or more instructions ahead, the bubble
has already passed into ID before the stall begins, and it costs its
cycle. Penumbra/2 redirects every taken branch from EX, so it has no
such bubble to hide.

### Penumbra/3

An ALU operation occupies each stage for one cycle: **1**.

**Loads** issue in **1**: MEM1 and MEM2 take a hit without holding
the pipeline. The loaded value is forwarded only from the MEM2/WB
register
([memory-completion.md](penumbra3/memory-completion.md#load-hit--a-registered-load-use-hazard)),
so ID holds a reader directly behind a load for two cycles, and for
one with a single instruction between them: a latency of **3**, for an
operand, an address, a store's data, the flags of a compare and a
jump's target alike. `RDSYS` takes the same path.

**Stores** enter the store buffer (`penumbra3_store_buffer`,
[uncore.md](penumbra3/uncore.md)) at MEM2 and cost their issue cycle
while it has room. The bus master writes each posted store as a
transaction of its own — accept, presentation and ack, with its
completion the cycle it takes the next store — which is the ~4 cycles
a store takes in a run
([memory-performance.md](penumbra3/memory-performance.md#sustained-writes-on-the-sdram-stack));
a full buffer holds MEM2.

**Multiply and divide** hold EX for the divmul unit, 10 cycles for a
multiply and 34 for a divide with the instruction's own cycle
included. WB then writes Rd and Rdh on consecutive cycles, and its
hold on the second (`writing_aux` in `penumbra3_wb_stage.sv`) stalls
everything behind it, two-register form or not: **11** and **35**. A
`MUL` or `DIV` directly behind waits in EX for its own unit through
that cycle, so the first of the pair costs one less. The low half
forwards from EX/MEM1 on, as an ALU result does, and its reader waits
for nothing: a latency of **11** and **35**. No leg carries the high
half, so the spine lists it as a late writer through MEM2 and WB's
hold covers its write: ID holds a reader directly behind for 3
cycles, one fewer for each instruction between, a latency of **14**
and **38**.

**Control transfers.** IF2 pre-decodes each word and steers IF1 for a
`B`, a `BL`, a `Bcc` the direction table calls taken, and a `JMP R13`
from the return stack's top
([branch-prediction.md](penumbra3/branch-prediction.md#design)); the
sequential fetch IF1 launched in that cycle is discarded, so a
predicted taken transfer costs **2**. EX checks every prediction and
redirects on a mismatch, discarding what the front end and ID hold
behind the transfer: **4**, in either direction, and for every `JMP`
and `JALR` the return stack does not predict.

**Stalls beside a predicted taken branch.** IF1, IF2 and the fetch
buffer keep fetching while the back end stalls. A predicted taken
transfer that reaches IF2 during a stall has its discarded fetch
absorbed: one to five instructions behind the stalled instruction,
whether it stalls in EX (a `MUL` or `DIV`), in ID (a reader waiting)
or in MEM2 (a miss). Further back, the transfer leaves IF2 after the
stall and its bubble costs its cycle. A hit holds nothing, so a load
or store beside a branch hides nothing, unlike on Penumbra/2.5.

## Measuring

The figures come from bare-metal programs run on the program
testbenches, which simulate a whole machine with its SDRAM clocked at
four times the CPU: `build/V<core>_tb_cpu_prog` for Penumbra/1,
`build/V<core>_tb_penumbra2_prog` for Penumbra/2 and /2.5 and
`build/Vpenumbra3_tb_penumbra3_prog` for Penumbra/3, each program
assembled with `pasm.py --org 0xFFFF0000` and passed as `+rom_hex=`.
Each program:

1. maps its data page and the ROM page cacheable, and enables the MMU
   and the L1 caches — and the L2, through its own `CACHE_CTRL`, when
   a miss is being measured;
2. reads `RDSYS Rn, #CPU, #CPU_CYCLES` around a loop of 64 iterations
   of the body under test, closed by `SUB`/`BNE`;
3. runs the whole measurement three times and keeps the last pass, so
   every fetch and access is warm;
4. ends with `R1 = 0`, so the testbench dumps the registers the
   results were left in.

**Keep the measured instruction away from the loop branch.** Follow
the body with two independent ALU operations and time a loop of only
those two as the baseline; the body's cost is the difference. Without
that padding, a load, store, `MUL` or `DIV` at the end of the body
hides the closing branch's bubble on Penumbra/2.5 (see
[Stalls beside a predicted taken branch](#stalls-beside-a-predicted-taken-branch))
and reads a cycle low: that is how loads were once recorded as 1
cycle, stores as 5, `MUL` as 10 and `DIV` as 34. The overlap itself
is measured by the opposite placement: the instruction directly
before the `SUB`, or between it and the `BNE`.

Penumbra/3 hides the bubble from further back: a stall absorbs it
with the `BNE` up to five instructions behind
([Penumbra/3](#penumbra3)). Two fillers read a `MUL` there as 10 and
a miss a cycle low; four or more fillers ahead of the `SUB` keep every
figure clean. A reader's wait shrinks the same way when it is a stall
of its own, so its probes need the padding too: with two fillers the
reader of a `MUL`'s high half waits 4, 2, 1 and 0 cycles at distances
one to four, where the true waits are 3, 2, 1 and 0.

On Penumbra/3 two independent `MUL`s or `DIV`s in a row cost one
cycle less than two apart, so a pair measures a different cost from
one; time one, with the padding.

**Fillers must depend on nothing.** `ADD R0, R9` writes the zero
register and reads a constant, so no instruction waits on it. A
two-address `ADD Rn, Rm` reads its own destination: four in a row are
a dependency chain, which costs 13 cycles on Penumbra/2 where four
independent operations cost 4.

**Latency** comes from the wait: a producer followed directly by its
reader, against the same producer followed by an independent filler.
The difference is how long the reader waited, and the producer's
issue cycles plus that wait is the latency. A second probe, with one
filler between producer and reader, checks it: the reader's wait
should shrink by the filler's issue cycles, down to none.

**Miss costs** come from a dependent pointer chase — each load's
address is the previous load's result, so nothing overlaps it —
through a working set larger than every baseline L1 and smaller than
the L2: 16 KiB, one pointer per 16-byte line, each 37 lines past the
last, so consecutive loads share neither a line's neighbour nor an L1
set. Run it once around the ring before timing it, so every line is
in the L2 and none in the L1. Against a chase that
keeps hitting one line, the difference is the cost of an L1 miss
served by the L2; with the L2 disabled, the same chase measures the
memory path instead. Mapping the extra pages takes `TLB_VPN` values
of the page number shifted left by 8, as [mmu.md](../system/mmu.md)
lays the register out, not the page's address.

Store costs and anything past the L2 depend on the machine: the
testbenches clock the SDRAM at four times the CPU, as the Penumbra/1
ULX3S board does, while Penumbra/2.5 boards run 2.67 times. The
programmer's reference records those as typical figures with the
machine they were measured on. A figure that cannot be measured yet
is marked there as inferred.
