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

How the cycles arise inside each core, and how every figure here was
measured, is in
[`internals/instruction-timing.md`](../internals/instruction-timing.md).

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
operation costs 2 and a load 4, so its loads are medium.

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
unrelated instruction in between hides the wait.

All figures are CPU clock cycles in steady state, with every
instruction fetch and data access an L1 hit and no interrupts. To
cost a sequence, add the issue cycles, add each reader's wait, then
apply [Taken branches beside a stall](#taken-branches-beside-a-stall).

## Instructions with a result

Each cell is *issue / latency*. A dash marks an instruction that
writes no register.

| Instruction | `penumbra1` | `penumbra2` | `penumbra2_5` |
|---|---|---|---|
| ALU operation ¹ | 2 / 2 | 1 / 4 | 1 / 1 |
| Load ² | 4 / 4 | 2 / 5 | 2 / 3 |
| Store, *typical* ³ | 8 / — | 6 / — | 6 / — |
| `MUL`, `MULU` | 37 / 37 | 11 / 14 | 11 / 11 |
| `MUL`, `MULU`, high half | 37 / 37 | 11 / 14 | 11 / 14 |
| `DIV`, `DIVU` | 37 / 37 | 35 / 38 | 35 / 35 |
| `DIV`, `DIVU`, high half | 37 / 37 | 35 / 38 | 35 / 38 |
| `RDSYS` ⁴ | 3 / 3 | 2 / 5 | 2 / 3 |
| `WRSYS` ⁴ | 2 / — | 6 / — | 7 / — |
| `RDSPR` | 2 / 2 | 1 / 4 | 1 / 1 |
| `WRSPR` | 2 / — | 1 / — | 1 / — |

¹ `ADD`, `SUB`, `ADC`, `SBC`, `AND`, `OR`, `XOR`, `NOT`, the shifts,
`MOV`, `CMP`, `TEST`, `LLI` and `LUI`, register and immediate forms.
A `Bcc` may directly follow the instruction that sets its flags on
every generation. On `penumbra2`, consecutive flag-setting
instructions wait for one another: four back-to-back `CMP`s take 7
cycles.

² `LDW`, `LDH`, `LDHS`, `LDB` and `LDBS`. The latency holds whether the
value is used as an operand, as an address or as a store's data. A
miss adds its cost to both figures.

³ A store writes through to memory and completes when the memory
system accepts the write, so part of its cost belongs to the machine.
Measured in RTL simulation of the ULX3S machine, SDRAM clocked at four
times the CPU. Back-to-back stores can cost more on a memory system
that cannot accept writes as fast as they come.

⁴ To the CPU's own devices: the MMU, the caches and the CPU's
identification and performance counters.

`MUL` and `DIV` take the same time whatever their operands, and
whether or not the two-register forms write the high half; the high
half arrives later on the pipelined generations.

## Control transfers

Issue cycles; a control transfer writes no register other than `BL`'s
link.

| Transfer | `penumbra1` | `penumbra2` | `penumbra2_5` |
|---|---:|---:|---:|
| `Bcc`, not taken | 2 | 1 | 1 |
| `B`, `Bcc` taken, predicted | 2 | — | 2 |
| `B`, `Bcc` taken, not predicted | 2 | 5 | 5 |
| `BL`, predicted | 3 | — | 2 |
| `BL`, not predicted | 3 | 5 | 5 |
| `JMP Rn`, including returns | 2 | 5 | 5 |

A mispredicted branch — one predicted taken that falls through —
costs the same as a taken branch that was not predicted. See
[Branch prediction](#branch-prediction) for what each generation
predicts.

## Taken branches beside a stall

On `penumbra2_5` a predicted taken branch costs 2: the branch and one
discarded fetch. When a load, a store, a `MUL` or a `DIV` is one of
the two instructions directly before that branch, its stall hides the
discarded fetch, and the pair costs one cycle less than the sum of
their issue cycles. In a loop closed by `SUB R13, #1; BNE`, a body of
`ADD R8, R9; LDW R7, [R6]` takes one cycle less than the same two
instructions in the other order. `penumbra1` and `penumbra2` have no
such overlap.

## Branch prediction

- **`penumbra1`** predicts nothing and has no need to: every control
  transfer costs the same whether or not it is taken.
- **`penumbra2`** predicts nothing: every taken branch, jump and
  return costs 5.
- **`penumbra2_5`** has a branch target buffer of 32 entries, one per
  value of address bits 6..2, each tagged with the rest of the
  address. It holds the targets of direct branches (`B`, `Bcc`, `BL`).
  A branch that resolves taken writes its entry; one that resolves not
  taken removes it, so each branch site predicts the direction it
  last went. Returns and other `JMP Rn` are never predicted and always
  cost 5. Two branch sites whose addresses agree in bits 6..2 share an
  entry and displace each other.

## Memory system

### What a generation fixes

- Cache lines are 16 bytes.
- The L1 caches are virtually indexed and physically tagged. They are
  write-through and do not allocate on a write: a store costs the same
  whether or not its line is in the L1, and never brings a line in.
- An L1 hit costs what the tables above say.
- An L1 miss that the L2 serves costs, beyond a hit:

  | `penumbra1` | `penumbra2` | `penumbra2_5` |
  |---:|---:|---:|
  | 11 | 16 | 16 |

  This is fixed by the design of the core and its L2, whatever size
  of L2 a machine builds.

### What a machine decides

Cache sizes and associativity, and the TLB's geometry, belong to the
machine rather than the generation. Each generation's well-known
build is a baseline; a smaller chip may build smaller caches and a
larger one bigger caches, with different associativity:

| Baseline | `penumbra1` | `penumbra2`, `penumbra2_5` |
|---|---|---|
| L1-I | 1 KiB, direct-mapped | 4 KiB, 2-way |
| L1-D | 1 KiB, direct-mapped | 4 KiB, 2-way |
| L2 | 64 KiB, 4-way | 64 KiB, 4-way |
| TLB | 32 sets × 2 ways | 32 sets × 2 ways |

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

- **Memory operations dominate.** On the pipelined generations a load
  costs as much as two ALU operations and a store as much as six.
  Keeping a value in a register beats reloading it; one wide store
  beats several narrow ones.
- **Separate a load from its first use.** On `penumbra2_5` one
  instruction in between hides the wait; on `penumbra2` each
  instruction in between saves a cycle of it.
- **Calls cost their return.** On the pipelined generations a return
  is never predicted: a call and its return cost at least 7 cycles on
  `penumbra2_5`, before the callee does anything.
- **Compare `MUL` and `DIV` against the ALU sequences that replace
  them** on the generation being tuned for: a `MUL` is worth about 18
  ALU operations on `penumbra1` and 11 on `penumbra2_5`.
- **Lay out hot branches** so that no two share bits 6..2 of their
  addresses on `penumbra2_5`.
