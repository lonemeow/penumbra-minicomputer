# Penumbra CPU — Instruction Timing by Core

> **Applies to:** all generations · reference. How many cycles an
> instruction holds each core, measured on the RTL testbenches, and
> where those cycles go. The compiler's scheduling models
> (`llvm/lib/Target/Penumbra/Penumbra.td`) state these figures; this
> doc is their source, so a model change starts here.

## What the numbers mean

Every figure is **whole-instruction cycles in steady state**: the
cycles a program's run grows by for each execution of the
instruction, fetch included, with every I-cache and D-cache access a
hit and no interrupts. They are what a cost decision should compare —
a shift/add sequence pays the fetch on every instruction, a multiply
pays it once.

On a pipelined core an instruction's figure includes the stall it
imposes on the pipeline behind it (a divide holding EX), but not the
stall a *consumer* of its result takes waiting for it. Those
dependency costs differ between otherwise identical cores and are
listed separately under
[Dependency costs](#dependency-costs).

Real code runs at a higher CPI than the ALU figures suggest: loads,
stores and multi-µop instructions are part of any instruction mix,
and cache misses, branch flushes and dependency stalls come on top.

## Per-instruction cycles

| Instruction | Penumbra/1 | Penumbra/2 | Penumbra/2.5 |
|---|---:|---:|---:|
| ALU, register or immediate operand (`ADD`, `ADDi`, `SHLi`, `LLI`, …) | 2 | 1 | 1 |
| `LDW`, D-cache hit | 4 | — | 1 |
| `STW`, write-through | 8 | — | 5 |
| `MUL` / `MULU` | 37 | — | 10 |
| `DIV` / `DIVU` | 37 | 35¹ | 34 |
| `Bcc` not taken | 2 | — | 1 |
| `Bcc` taken / `B` | 2 | — | 2 predicted, 5 mispredicted² |
| `JMP Rn` (indirect, including returns) | 2 | — | 5 |
| `BL` | 3 | — | — |

¹ Measured inside a loop whose divides read one another's results, so
it includes a small Penumbra/2 dependency stall.

² See [Control transfers](#control-transfers-on-penumbra25) for what
the BTB predicts.

"—" marks a figure not measured.

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

### Penumbra/2 and Penumbra/2.5

An ALU operation occupies each stage for one cycle: **1**. A multiply
or divide holds EX for the divmul unit's launch-to-done cycles and
writes Rd then Rdh through the regfile's single write port on
consecutive WB cycles
([hazard-model.md](penumbra2/hazard-model.md#interaction-with-divmul));
measured end to end, a divide costs **34** cycles, the unit's own
launch + 32 steps + done count. The multiply retires four bits per
step, so its 8 steps give **10** — the same whether the high half is
written to a register or discarded to R0.

### Control transfers on Penumbra/2.5

Branches resolve in EX. A not-taken conditional branch follows the
static fall-through prediction: **1**. Direct branches (`B`, `Bcc`,
`BL`) are also looked up in a fetch-time branch target buffer; a taken
branch the BTB predicts costs **2**, one cycle more than the branch
itself. Anything EX has to correct costs **5**, the branch plus a
4-cycle redirect: a mispredicted direction, a branch the BTB does not
know, and every indirect jump and return, which the BTB never
predicts. A branch that alternates direction mispredicts on every
execution.

[penumbra2/overview.md](penumbra2/overview.md) describes a correctly
predicted taken branch as flushing 0 bubbles; the measured cost is
one cycle.

## Dependency costs

What a consumer waits for a register result of a producer `d`
instructions ahead:

| Core | ALU producer | Load producer | divmul Rdh |
|---|---|---|---|
| Penumbra/1 | none | none | none |
| Penumbra/2 | stalls until the producer leaves WB: 3 cycles at `d = 1`, fewer further back | same | same |
| Penumbra/2.5 | none (EX→EX, MEM→EX, WB write-through) | 1 cycle at `d = 1` | stalls until the divmul leaves WB |

Penumbra/1 is serial, so no instruction ever waits on another beyond
its own µops. On Penumbra/2 a chain of eight `ADD R7, R8` measures 29
cycles — seven consumers at `d = 1` at 1 + 3 each, and one reading a
producer further back — against 8 on Penumbra/2.5. The mechanisms are
in [hazard-model.md](penumbra2/hazard-model.md).

## How the compiler uses these

Each core's scheduling model in `Penumbra.td` states write latencies
for three instruction classes: `WriteALU` (ALU operations and the
immediate forms), `WriteMul` and `WriteDiv`. `PenumbraSubtarget` reads
them back (`getALUCycles`, `getMulCycles`, `getDivCycles`), and
`PenumbraTTIImpl`, `isIntDivCheap()` and the constant-multiply combine
weigh operations by them.

Penumbra/2 and Penumbra/2.5 share one model. It states instruction
latencies only, so no model expresses Penumbra/2's dependency stall;
cost decisions price Penumbra/2 as if it forwarded. Nor does any model
state branch costs: the misprediction penalty and the cost of a
predicted taken branch above are not read by the compiler.

## Measuring

The figures come from bare-metal programs run on the program
testbenches (`build/V<core>_tb_cpu_prog` for Penumbra/1,
`build/V<core>_tb_penumbra2_prog` for Penumbra/2 and /2.5), assembled
with `pasm.py --org 0xFFFF0000`. Each program:

1. copies a small loop into a RAM page and maps that page cacheable —
   the ROM page is mapped uncacheable, so code run in place never hits
   the I-cache;
2. enables the MMU, the I-cache, and for load/store bodies the D-cache;
3. runs `ITERS` iterations of the body under test, followed by
   `SUB R6, #1` / `BNE loop`.

A never-taken branch is `CMP R7, R7` followed by `BNE` to the next
instruction; an always-taken one is `B` to the next instruction.
`pasm.py` has no expressions, so an indirect jump's RAM target is
computed at run time from `LA` of the label, less `LA` of the copied
routine's start, plus the RAM base. A `BL` body overwrites LR, which
the copied routine returns through, so save LR before the loop and
restore it before `RET`.

Running each program at two iteration counts and dividing the
difference in cycles by the difference in iterations cancels setup,
the cold first pass and the halt; subtracting the empty body's
per-iteration cost leaves the body's.

The Penumbra/1 testbench reports the total cycle count at halt. The
Penumbra/2 testbench reports none, so the program reads
`RDSYS Rn, #CPU, #CPU_CYCLES` immediately before and after the loop,
leaves the difference in a register, and ends with `R1 = 0` so the
testbench dumps the registers.
