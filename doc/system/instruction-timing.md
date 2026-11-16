# Penumbra Instruction Timing — Programmer's Reference

What each instruction costs on each generation of the Penumbra CPU:
the figures a compiler's cost model, a hand-tuned loop, libc or the
kernel's hot paths are written against. Timing belongs to a
generation, not to the ISA. Software may tune for it; a correct
program never depends on it.

The generations are named the way `-mcpu` names them:

| Generation | Core |
|---|---|
| `penumbra1` | Microcoded and serial: each instruction runs to completion before the next is fetched |
| `penumbra2` | Pipelined; no operand forwarding, no branch prediction |
| `penumbra2_5` | Pipelined, with operand forwarding and a branch target buffer |
| `penumbra3` | Pipelined in seven stages, with operand forwarding, a store buffer, and branch prediction at fetch |

How the cycles arise inside each core, and how every figure here was
measured, is in
[`internals/instruction-timing.md`](../internals/instruction-timing.md).

The `penumbra3` figures were measured at `b22144471804`, before that
generation's design was settled. A change to its RTL can move them;
re-measure them after one.

## Facts and typical figures

Two kinds of number appear below.

- **Facts** are cycles spent inside the CPU core and its caches. Every
  machine built with a generation gives the same figure: an L1 hit
  costs the same on any `penumbra2_5` machine.
- **Typical** figures depend on the machine around the core — the
  memory it uses, how that memory's clock relates to the CPU's, the
  caches it builds. They are marked *typical*, and a footnote names
  the machine they were measured on. A port to another board may add
  its own.

For most decisions the cost class matters more than the exact figure:

| Class | Cycles | For example |
|---|---|---|
| Cheap | 1–2 | ALU operation, L1-hit load, not-taken or predicted taken branch |
| Medium | 3–10 | Store, mispredicted branch, return, indirect jump, `WRSYS` |
| Expensive | 11–50 | `MUL`, `DIV`, L1 miss, trap entry and return |
| Very expensive | over 50 | TLB refill |

The examples are for the pipelined generations. On `penumbra1` an ALU
operation costs 2 and a load 4, so its loads are medium. On
`penumbra3` a store is cheap while its store buffer has room, and so
is a return its return stack predicts; an L1 miss the L2 serves is
medium.

## Issue and latency

Two figures describe an instruction, the way pipelined machines have
long been documented:

- **Issue** is how many cycles the instruction holds the pipeline:
  the next instruction can start that many cycles later, if it does
  not need this one's result.
- **Latency** is how many cycles pass from the start of the
  instruction until an instruction that reads its result can start
  without waiting.

A reader waits for whatever part of its producer's latency the
producer's own issue and the instructions between them have not
covered. On `penumbra2_5` a load issues in 2 cycles and has a latency
of 3: its reader, placed directly after it, waits one cycle; one
unrelated instruction in between hides the wait. On `penumbra3` a load
issues in 1 cycle with the same latency of 3: its reader directly
after waits two cycles, and two unrelated instructions in between
hide the wait.

All figures are CPU clock cycles in steady state, with every
instruction fetch and data access an L1 hit and no interrupts. To
cost a sequence, add the issue cycles, add each reader's wait, then
apply [Taken branches beside a stall](#taken-branches-beside-a-stall).

## Instructions with a result

Each cell is *issue / latency*. A dash marks an instruction that
writes no register.

| Instruction | `penumbra1` | `penumbra2` | `penumbra2_5` | `penumbra3` |
|---|---|---|---|---|
| ALU operation ¹ | 2 / 2 | 1 / 4 | 1 / 1 | 1 / 1 |
| Load ² | 4 / 4 | 2 / 5 | 2 / 3 | 1 / 3 |
| Store, *typical* ³ | 8 / — | 6 / — | 6 / — | 1 / — |
| `MUL`, `MULU` ⁵ | 37 / 37 | 11 / 14 | 11 / 11 | 11 / 14 |
| `MUL`, `MULU`, high half ⁵ | 37 / 37 | 11 / 14 | 11 / 14 | 11 / 14 |
| `DIV`, `DIVU` ⁵ | 37 / 37 | 35 / 38 | 35 / 35 | 35 / 38 |
| `DIV`, `DIVU`, high half ⁵ | 37 / 37 | 35 / 38 | 35 / 38 | 35 / 38 |
| `RDSYS` ⁴ | 3 / 3 | 2 / 5 | 2 / 3 | 1 / 3 |
| `WRSYS` ⁴ | 2 / — | 6 / — | 7 / — | 7 / — |
| `RDSPR` | 2 / 2 | 1 / 4 | 1 / 1 | 1 / 1 |
| `WRSPR` | 2 / — | 1 / — | 1 / — | 1 / — |

¹ `ADD`, `SUB`, `ADC`, `SBC`, `AND`, `OR`, `XOR`, `NOT`, the shifts,
`MOV`, `CMP`, `TEST`, `LLI` and `LUI`, register and immediate forms.
A `Bcc` may directly follow the instruction that sets its flags on
every generation. On `penumbra2`, consecutive flag-setting
instructions wait for one another: four back-to-back `CMP`s take 7
cycles.

² `LDW`, `LDH`, `LDHS`, `LDB` and `LDBS`. The latency holds whether the
value is used as an operand, as an address or as a store's data, and
on `penumbra3` also for a compare a branch reads and for a jump's
target. A miss adds its cost to both figures.

³ A store writes through to memory and completes when the memory
system accepts the write, so part of its cost belongs to the machine.
Measured in RTL simulation of the ULX3S machine, SDRAM clocked at four
times the CPU. Back-to-back stores can cost more on a memory system
that cannot accept writes as fast as they come. On `penumbra3` a store
enters a store buffer and costs its one issue cycle while the buffer
has room. The buffer writes a store through to memory in about 5
cycles, so stores closer together than that fill it, and then each
one waits that long.

⁴ To the CPU's own devices: the MMU, the caches and the CPU's
identification and performance counters.

⁵ On `penumbra3`, a `MUL` or `DIV` directly followed by another holds
the pipeline one cycle less: 10 or 34.

`MUL` and `DIV` take the same time whatever their operands, and
whether or not the two-register forms write the high half. The high
half arrives later than the low half on `penumbra2` and `penumbra2_5`,
and with it on `penumbra3`.

## Control transfers

Issue cycles; a control transfer writes no register other than `BL`'s
link.

| Transfer | `penumbra1` | `penumbra2` | `penumbra2_5` | `penumbra3` |
|---|---:|---:|---:|---:|
| `Bcc`, not taken | 2 | 1 | 1 | 1 |
| `B`, `Bcc` taken, predicted | 2 | — | 2 | 2 |
| `B`, `Bcc` taken, not predicted | 2 | 5 | 5 | 4 |
| `BL`, predicted | 3 | — | 2 | 2 |
| `BL`, not predicted | 3 | 5 | 5 | — |
| `JMP R13`, return predicted | — | — | — | 2 |
| `JMP Rn`, including returns | 2 | 5 | 5 | 4 |

A mispredicted branch — one predicted taken that falls through —
costs the same as a taken branch that was not predicted. See
[Branch prediction](#branch-prediction) for what each generation
predicts. On `penumbra3`, `JALR` costs what `JMP` does.

## Taken branches beside a stall

On `penumbra2_5` a predicted taken branch costs 2: the branch and one
discarded fetch. When a load, a store, a `MUL` or a `DIV` is one of
the two instructions directly before that branch, its stall hides the
discarded fetch, and the pair costs one cycle less than the sum of
their issue cycles. In a loop closed by `SUB R13, #1; BNE`, a body of
`ADD R8, R9; LDW R7, [R6]` takes one cycle less than the same two
instructions in the other order. `penumbra1` and `penumbra2` have no
such overlap.

On `penumbra3` a load or store that hits holds the pipeline only for
its one issue cycle, so it hides nothing. A stall does: a `MUL` or
`DIV`, a miss, or a reader waiting for its operand. Fetch runs on
through it and takes in the discarded fetch of a predicted taken
transfer up to five instructions behind the stalled one. A loop of
`MUL R5, R8` and two independent ALU operations, closed by
`SUB R13, #1; BNE`, takes 10 cycles more than the same loop without
the `MUL`.

## Branch prediction

- **`penumbra1`** predicts nothing and has no need to: every control
  transfer costs the same whether or not it is taken.
- **`penumbra2`** predicts nothing: every taken branch, jump and
  return costs 5.
- **`penumbra2_5`** has a direct-mapped branch target buffer, indexed
  by the address bits above the word offset and tagged with the rest
  of the address. It holds the targets of direct branches (`B`, `Bcc`,
  `BL`). A branch that resolves taken writes its entry; one that
  resolves not taken removes it, so each branch site predicts the
  direction it last went. Returns and other `JMP Rn` are never
  predicted and always cost 5. Two branch sites whose addresses agree
  in the index bits share an entry and displace each other.
- **`penumbra3`** predicts as it fetches, from the instruction word:
  every `B` and `BL`, and every `Bcc` its direction table calls taken,
  steers fetch to the target the word holds. The table is a
  direct-mapped array of two-bit counters, indexed by the address bits
  above the word offset, each stepped toward the outcome of the
  branches that use it; branch sites whose addresses agree in the
  index bits share a counter. A return, `JMP R13`, is predicted from a
  return stack, which `BL` and `JALR` push and returns pop. The stack
  wraps, so a return nested deeper than it holds is predicted wrongly.
  Other `JMP Rn` and `JALR` itself are never predicted and cost 4.

How many entries each structure has is a build's choice, as the
caches' sizes are, and no register reports it. The baseline builds
have:

| Baseline | `penumbra2_5` | `penumbra3` |
|---|---|---|
| Branch target buffer | 32 entries, indexed by address bits 6..2 | — |
| Direction table | — | 1024 counters, indexed by address bits 11..2 |
| Return stack | — | 8 entries |

## Memory system

### What a generation fixes

- Cache lines are 16 bytes.
- The L1 caches are virtually indexed and physically tagged. They are
  write-through and do not allocate on a write: a store costs the same
  whether or not its line is in the L1, and never brings a line in.
- An L1 hit costs what the tables above say.
- An L1 miss that the L2 serves costs, beyond a hit:

  | `penumbra1` | `penumbra2` | `penumbra2_5` | `penumbra3` |
  |---:|---:|---:|---:|
  | 11 | 16 | 16 | 4 |

  This is fixed by the design of the core and its L2, whatever size
  of L2 a machine builds.

### What a machine decides

Cache sizes and associativity, the TLB's geometry and the store
buffer's depth belong to the machine rather than the generation. Each
generation's well-known build is a baseline; a smaller chip may build
smaller caches and a larger one bigger caches, with different
associativity:

| Baseline | `penumbra1` | `penumbra2`, `penumbra2_5` | `penumbra3` |
|---|---|---|---|
| L1-I | 1 KiB, direct-mapped | 4 KiB, 2-way | 8 KiB, 2-way |
| L1-D | 1 KiB, direct-mapped | 4 KiB, 2-way | 8 KiB, 2-way |
| L2 | 64 KiB, 4-way | 64 KiB, 4-way | 64 KiB, 4-way |
| TLB | 32 sets × 2 ways | 32 sets × 2 ways | 256 sets × 2 ways |
| Store buffer | — | — | 16 entries |

Software that depends on a geometry reads it at run time: each
cache's [`INFO`](./sysregs.md#info-reg-0) register, which also reports
its write policy, and the MMU's
[`MMU_CONFIG`](./mmu.md#mmu_config-layout).

What lies beyond the L2 differs from machine to machine, and on a
machine whose memory runs on its own clock it differs from access to
access as well: the phase between the two clocks, refresh and the
memory's row state all move it. No figure is exact; these are typical:

| Event, `penumbra2_5` | Typical cycles | Class |
|---|---:|---|
| L1 miss that also misses the L2 ⁴ | 38 | Expensive |
| Trap entry and `ERET` ⁵ | 31 | Expensive |
| TLB refill, kernel handler included ⁵ | 140–170 | Very expensive |

⁴ Measured on the ULX3S (SDRAM) at 37.5 MHz, by `benchmark/membench`,
which reports the per-miss cost of whatever machine it runs on.

⁵ Measured on the ULX3S machine in RTL simulation, with NetBSD's refill
handler. The vector fetch is a memory access, so trap entry varies
with the machine; a refill also varies with where the page table
sits.

## Consequences for code

- **Memory operations dominate.** On `penumbra2` and `penumbra2_5` a
  load costs as much as two ALU operations and a store as much as six.
  Keeping a value in a register beats reloading it; one wide store
  beats several narrow ones. On `penumbra3` a load or store issues in
  one cycle; a load's cost is what its reader waits, and stores cost
  more only in a run the store buffer cannot drain as fast.
- **Separate a load from its first use.** On `penumbra2_5` one
  instruction in between hides the wait; on `penumbra3` two do; on
  `penumbra2` each instruction in between saves a cycle of it.
- **Calls cost their return.** On `penumbra2` and `penumbra2_5` a
  return is never predicted: a call and its return cost at least 7
  cycles on `penumbra2_5`, before the callee does anything. On
  `penumbra3` the return stack predicts it, and the pair costs 4 while
  calls nest no deeper than the stack holds.
- **Compare `MUL` and `DIV` against the ALU sequences that replace
  them** on the generation being tuned for: a `MUL` is worth about 18
  ALU operations on `penumbra1` and 11 on `penumbra2_5` and
  `penumbra3`.
- **Lay out hot branches** so that no two share the bits that index
  their generation's predictor: bits 6..2 on the baseline
  `penumbra2_5`, 11..2 on the baseline `penumbra3`.
