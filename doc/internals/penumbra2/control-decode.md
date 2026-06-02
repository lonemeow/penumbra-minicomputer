# Penumbra/2 — Control Decode

> **Applies to:** Penumbra/2 · pipelined core.

This document specifies how Penumbra/2 turns a 32-bit instruction
word into the control signals that drive each pipeline stage. It
covers the decode architecture (hardwired, no microcode), the four
instruction formats and their field maps, the control bundle ID
emits, register/immediate extraction, branch-condition evaluation,
and the exception-detect signals decode produces. It is the
reference for implementing `decode.sv` (shared helpers) and the
per-stage decode blocks in `id_stage.sv`, `ex_stage.sv`,
`mem_stage.sv`, and `wb_stage.sv`.

The **instruction encoding** is fixed by the ISA and shared with
gen1; the authoritative source is
[`doc/system/instruction-encoding.md`](../../system/instruction-encoding.md)
and [`instruction-set.md`](../../system/instruction-set.md). This
doc recaps the field layouts it needs and cites that spec as
authoritative; it does not re-decide any encoding.

## Scope

Covered:

- The decode architecture: hardwired per-stage control, ID-centric
  decode-once with downstream subset narrowing.
- The four formats (R/L/M/B) and their field maps.
- The `IR[15:12]` field-aliasing hazard (Rdh vs SPR# vs sysreg dev).
- The ID control bundle: every signal and how it is derived.
- Register operand selection and immediate extension.
- Branch-condition evaluation ([Branch-condition evaluation](#branch-condition-evaluation) — open design point).
- `writes_flags` / `reads_flags` derivation.
- The exception-detect signals (illegal, privilege) decode emits.

Out of scope:

- The ISA→physical scoreboard register *mapping* (R14 banking,
  SPR-USP cross-bank) — fully specified in
  [ISA → physical register mapping](./hazard-model.md#isa--physical-register-mapping);
  this doc references it rather than duplicating.
- Pipeline-register field widths and stall/flush semantics — see
  [pipeline-stages.md](./pipeline-stages.md).
- The semantics of each instruction — see
  [instruction-set.md](../../system/instruction-set.md).

## Decode architecture

### Hardwired, no microcode

Penumbra/2 has no µROM and no sequencer
([Decision 6](./design-decisions.md#6-control-architecture-pure-hardwired-no-microcode)).
Every instruction is single-µop: one ALU op, or one memory access,
or one register transfer, or one branch. Control is therefore a
**combinational function of the instruction word** (plus a little
state: `SR.S` for privilege/banking, the drain-commit and divmul EX
FSMs). There is nothing to sequence, so there is no microcode to
sequence it.

### Decode once in ID, narrow downstream

The apparent tension in Decision 6 — "each stage has its own
decoder" — versus pipeline-stages.md — "ID decodes into a control
bundle carried in ID/EX" — resolves as a **hybrid**:

- **ID runs the full decoder.** It is the only stage that sees the
  raw instruction word, and it does all the expensive work once:
  format detection, opcode classification, register-field → physical
  mapping, immediate extraction, privilege/illegal detection. Its
  output is the `ctrl` bundle (~30 bits) latched into ID/EX.
- **Downstream stages do not re-decode the instruction word.** They
  carry a *progressively narrowed subset* of `ctrl` and select from
  it. EX reads `alu_op`, the operand-mux selects, the branch/condi-
  tion fields, and the drain-commit/divmul kind. At the EX/MEM
  boundary the bundle narrows to `ctrl_mem` (memory op, size,
  sign-extend, sysreg selects) plus `ctrl_wb` (write-enables and
  destinations). At MEM/WB only `ctrl_wb` survives. (Widths in
  [pipeline-stages.md §Inter-stage pipeline registers](./pipeline-stages.md#inter-stage-pipeline-registers).)

So "each stage has its own decoder" means each stage has a small
**local selector** over the carried subset — not a second full
instruction-word decoder. The instruction word itself does not ride
past ID.

### Why this partition

The alternative — carry the raw 32-bit `IR` to every stage and
fully re-decode there — trades pipeline-register width for decode
logic. Penumbra/2 carries the *decoded subset* instead, because:

- The expensive parts of decode (register→physical mapping, which
  depends on `SR.S`; immediate extension; illegal/priv detection)
  are needed exactly once, at issue. Re-deriving them downstream
  would replicate logic for no benefit and would re-introduce the
  `SR.S`-at-the-wrong-time hazard the scoreboard works to avoid
  ([SR.S quiescence for the decoder's R14 mapping](./hazard-model.md#srs-quiescence-for-the-decoders-r14-mapping)).
- The *narrowed* subsets are small (≤12 bits at EX/MEM, ≤10 at
  MEM/WB), so carrying them is cheaper than carrying the 32-bit IR
  plus re-decoders.
- A single decode point is easier to verify: the control bundle is
  defined once, and downstream stages consume named fields rather
  than re-interpreting opcodes.

Shared helpers — format detect, immediate extension, opcode-class
predicates — live in `decode.sv` and are instantiated by ID (and by
the few stages that need a predicate, e.g. EX's `is_branch`).

## Instruction formats

All instructions are 32 bits. The **format prefix** is `IR[31:30]`:

| Prefix | Format | Purpose |
|:------:|:------:|---------|
| `00` | R | Register–register ALU and system |
| `01` | L | Immediate operations |
| `10` | M | Memory load/store |
| `11` | B | Branch |

Format detection is the first decode step and gates how every other
field is interpreted.

### Format R (`00`)

```
31 30 29     25 24    21 20    17 16 15            0
[ 00 ][ op(5) ][ Rd(4) ][ Rs(4) ][F][   spare(15)  ]
```

| Field | Bits | Notes |
|-------|:----:|-------|
| op | 29:25 | 5-bit opcode (32 ops); `op[4]` splits single-cycle (0) from multi-cycle/system (1) |
| Rd | 24:21 | Destination **and** first source |
| Rs | 20:17 | Second source |
| F | 16 | Flag-only: `0` = write result+flags, `1` = flags only (CMP = SUB·F, TEST = AND·F) |
| spare | 15:0 | Repurposed by MUL/DIV and the SPR/sysreg ops — see [The `IR[15:12]` aliasing hazard](#the-ir1512-aliasing-hazard) |

Opcode map (`op[4:0]`): `00000`–`01011` single-cycle ALU/move
(ADD, SUB, AND, OR, XOR, SHL, SHR, SAR, MOV, NOT, ADC, SBC);
`10000`–`10011` divmul (MUL, MULU, DIV, DIVU); `10111`–`11111`
system (WRSYS, RDSYS, SYSCALL, BREAK, ERET, EI, DI, WRSPR, RDSPR).

### Format L (`01`)

```
31 30 29   26 25    22 21    16 15          0
[ 01 ][op(4)][ Rd(4) ][ spare(6)][ imm16(16) ]
```

| Field | Bits | Notes |
|-------|:----:|-------|
| op | 29:26 | 4-bit opcode (16 ops) |
| Rd | 25:22 | Destination and (for ALU-imm) first source |
| imm16 | 15:0 | 16-bit immediate; extension per-opcode ([Immediate extraction and extension](#immediate-extraction-and-extension)) |

Opcode map: LLI, LLIS, LUI, ADD#, SUB#, CMP#, AND#, TEST#, SHL#,
SHR#, SAR#, JMP, JALR (rest reserved). JMP/JALR take the target
register in the `Rd` field.

### Format M (`10`)

```
31 30 29 28 27 26 25    22 21    18 17           2 1 0
[ 10 ][L][ sz ][SE][ Rd(4) ][ Rb(4) ][ offset16(16) ][sp]
```

| Field | Bits | Notes |
|-------|:----:|-------|
| L | 29 | 1 = load, 0 = store |
| sz | 28:27 | `00` byte, `01` half, `10` word |
| SE | 26 | sign-extend on load (ignored for stores / word loads) |
| Rd | 25:22 | data register (dest on load, source on store) |
| Rb | 21:18 | base address register |
| offset16 | **17:2** | 16-bit **signed byte** offset; `EA = Rb + sign_ext(IR[17:2])` |
| sp | 1:0 | spare |

Note the offset occupies `IR[17:2]`, **not** `IR[15:0]`; the
decoder extracts `IR[17:2]` and sign-extends. Range ±32 KB.

### Format B (`11`)

```
31 30 29   26 25                    4 3   0
[ 11 ][cond(4)][    offset22(22)     ][ sp ]
```

| Field | Bits | Notes |
|-------|:----:|-------|
| cond | 29:26 | condition code ([Branch-condition evaluation](#branch-condition-evaluation)) |
| offset22 | **25:4** | 22-bit **signed word** offset, relative to the **branch itself** |
| sp | 3:0 | spare |

Target = `PC_branch + sign_ext(IR[25:4] << 2)`. The offset is
relative to the branch's own PC (not `PC+4`), so the EX branch-target
adder uses *this-PC*, not *next-PC* — this is deliberate, to save an
adder in the discrete build
([instruction-encoding.md](../../system/instruction-encoding.md)).
`cond = 1111` is `BL` (always-taken, links `PC+4 → R13`).

## The `IR[15:12]` aliasing hazard

`IR[15:12]` is **context-dependent** on the Format-R opcode and is
the single most error-prone field for a decoder:

| When `op[4:0]` is… | `IR[15:12]` means | `IR[11:8]` means |
|--------------------|-------------------|------------------|
| MUL/MULU/DIV/DIVU (`10000`–`10011`) | `Rdh` register | spare |
| WRSPR/RDSPR (`11110`/`11111`) | SPR number | spare |
| WRSYS/RDSYS (`10111`/`11000`) | sysreg **device** number | sysreg **register** number |
| any single-cycle ALU/move | spare (don't-care) | spare |

**The opcode must be classified before `IR[15:12]` is interpreted.**
The decoder produces `rdh_sel`, `spr_sel`, and `{sys_dev, sys_reg}`
as separate outputs, each valid only when its opcode class is
active; consumers select by the bundle's `op_class`, never by
reading `IR[15:12]` raw. Treating this field uniformly is a classic
decode bug — it would, e.g., read a divmul `Rdh` as an SPR number.

## The ID control bundle

ID emits one bundle per issued instruction. The table is the
*logical* contents; the physical `ctrl` field packs the subset
EX needs, and the EX/MEM and MEM/WB registers carry the narrowing
subsets ([Decode once in ID, narrow downstream](#decode-once-in-id-narrow-downstream)).

| Signal | Width | Derivation |
|--------|:-----:|------------|
| `op_class` | ~4 | format + opcode → {alu, alu_imm, move, load, store, branch, jmp, divmul, rdspr, wrspr, rdsys, wrsys, eret, ei, di, syscall, break} |
| `alu_op` | ~4 | the ALU function (ADD/SUB/AND/OR/XOR/SHL/SHR/SAR/ADC/SBC/NOT/pass) |
| `a_sel` | 1–2 | operand-A source: regfile(Rd) / PC (for B-target) |
| `b_sel` | 1–2 | operand-B source: regfile(Rs) / immediate |
| `imm` | 32 | sign/zero-extended immediate ([Immediate extraction and extension](#immediate-extraction-and-extension)) |
| `writes_flags` | 1 | this op updates NZCV — marks it a flag-bypass producer ([`writes_flags` / `reads_flags` derivation](#writes_flags--reads_flags-derivation)) |
| `reads_flags` | 1 | this op consumes NZCV — Bcc, ADC/SBC (carry-in), RDSPR-SR; selects the EX flag bypass ([`writes_flags` / `reads_flags` derivation](#writes_flags--reads_flags-derivation)) |
| `flag_only` | 1 | F bit: suppress GPR write, keep flag write (CMP/TEST) |
| `cond` | 4 | branch condition ([Branch-condition evaluation](#branch-condition-evaluation)); valid when `op_class=branch` |
| `mem_op` | 2 | none / load / store |
| `mem_size` | 2 | byte / half / word |
| `sign_ext` | 1 | SE bit, for sub-word loads |
| `sys_dev` | 4 | sysreg device (RDSYS/WRSYS) |
| `sys_reg` | 4 | sysreg register (RDSYS/WRSYS) |
| `spr_sel` | 4 | SPR number (RDSPR/WRSPR) |
| `drain_commit` | 1 | ERET, WRSYS, WRSPR-SR, EI, DI ([Decision 9](./design-decisions.md#9-drain-commit-primitive)) |
| `post_commit_wait` | 1 | 1 only for WRSYS (Section in exception-flow / Decision 9) |
| `gpr_we` | 1 | writes a GPR at WB |
| `spr_we` | 1 | writes an SPR at WB |
| `flag_we` | 1 | writes SR flags at WB (= `writes_flags`) |
| `phys_src_a/b` | 5 each | physical scoreboard source entries ([ISA → physical register mapping](./hazard-model.md#isa--physical-register-mapping)) |
| `phys_dst` | 5 | physical destination entry |
| `phys_dst_hi` | 5 | divmul second destination (`Rdh`) |
| `cross_bank` | 1 | SPR-USP cross-bank access ([The cross-bank SPR-USP case](./hazard-model.md#the-cross-bank-spr-usp-case)) |
| `illegal` | 1 | no legal opcode/operand form ([Exception-detect signals from decode](#exception-detect-signals-from-decode)) |
| `priv_fault` | 1 | privileged op with `SR.S=0` ([Exception-detect signals from decode](#exception-detect-signals-from-decode)) |
| `is_trap` | 1 | SYSCALL/BREAK (raise the trap vector at EX) |

## Register operand selection

The decoder maps instruction register fields to physical scoreboard
entries and selects operand sources.

- **Source/destination fields** by format: Format R uses `Rd`
  (=`IR[24:21]`, both source A and destination) and `Rs`
  (=`IR[20:17]`, source B); Format L uses `Rd` (=`IR[25:22]`);
  Format M uses `Rd` (data) and `Rb` (base, =`IR[21:18]`).
- **ISA→physical mapping** (R14→USP/SSP by `SR.S`; SPR-USP cross-bank;
  R0/R15 exclusion) is specified in
  [ISA → physical register mapping](./hazard-model.md#isa--physical-register-mapping)
  and produces `phys_src_a/b`, `phys_dst`, `phys_dst_hi`, `cross_bank`.
  This doc does not restate that mapping.
- **R0** reads as zero and discards writes; the decoder emits the R0
  index but downstream the regfile forces 0 on read and drops the
  write (so e.g. `ADD R0, R0` is the canonical NOP).
- **R15 / PC** is never a regfile source — when an instruction reads
  R15 the `a_sel`/`b_sel` mux selects the PC value instead of the
  regfile port; R15 is not scoreboarded.
- **`Rdh`** (the divmul high-half result — product high half for
  MUL, remainder for DIV/DIVU) is **write-only**: `IR[15:12]`
  selects it as the second destination `phys_dst_hi` ([The ID control bundle](#the-id-control-bundle)),
  not as a source. divmul reads only `op_a` (Rd) and `op_b` (Rs) —
  there is no third *input* operand and no 64/32 narrowing form;
  divides are always 32/32. `Rdh = R0` discards the high half (the
  common 32-bit form).

## Immediate extraction and extension

The immediate's bit position and extension rule are
format-and-opcode specific:

| Source | Field | Extension |
|--------|-------|-----------|
| L: LLI, ADD#, SUB#, CMP#, AND#, TEST# | `IR[15:0]` | **zero**-extend |
| L: LLIS | `IR[15:0]` | **sign**-extend |
| L: LUI | `IR[15:0]` | `imm << 16` (OR-ed into Rd; Rd is also a source) |
| L: SHL#/SHR#/SAR# | `IR[4:0]` | shift amount (5 bits, no extension) |
| M: load/store offset | `IR[17:2]` | **sign**-extend (byte offset) |
| B: branch offset | `IR[25:4]` | **sign**-extend then `<< 2` (word offset) |

The shared `decode.sv` immediate helper takes the format + opcode
and produces the 32-bit `imm`. Note three traps: LLI is *zero*-
extend while LLIS is *sign*-extend (one opcode bit apart); LUI is an
**OR-into-Rd**, not a replace (so `LUI` after `LLI` builds a 32-bit
constant, but `LUI` alone needs Rd pre-cleared); and the M offset is
*not* in `IR[15:0]` ([Format M (`10`)](#format-m-10)).

## Branch-condition evaluation

A `Bcc` carries a 4-bit `cond` (=`IR[29:26]`) and is resolved in EX
against the NZCV flags ([Decode architecture](#decode-architecture) of
[pipeline-stages.md](./pipeline-stages.md) places branch resolution
in EX). The decoder passes `cond` through in the bundle; EX computes
**taken / not-taken** by evaluating `cond` against the current flags.

The 16 conditions and their flag tests:

| cond | Mnemonic | Taken when |
|:----:|----------|------------|
| 0000 | B (AL) | always |
| 0001 | BEQ | Z=1 |
| 0010 | BNE | Z=0 |
| 0011 | BCS/BHS | C=1 |
| 0100 | BCC/BLO | C=0 |
| 0101 | BMI | N=1 |
| 0110 | BPL | N=0 |
| 0111 | BVS | V=1 |
| 1000 | BVC | V=0 |
| 1001 | BHI | C=1 ∧ Z=0 |
| 1010 | BLS | C=0 ∨ Z=1 |
| 1011 | BGE | N=V |
| 1100 | BLT | N≠V |
| 1101 | BGT | Z=0 ∧ N=V |
| 1110 | BLE | Z=1 ∨ N≠V |
| 1111 | BL | always (links PC+4→R13) |

### Evaluation

The condition table is fixed by the ISA and is identical to gen1's,
so gen2 **reuses gen1's `cond_eval` logic unchanged** — this is not
a gen2 design point. `cond_eval.sv` is a stateless combinational
function of `(cond, NZCV)`; whether gen2 instantiates the existing
module or inlines its body into `decode.sv` is a packaging choice,
not a redesign. EX evaluates it combinationally and feeds the result
to the taken-branch flush/redirect.

The preferred logic shape is **direct (spread) select**: compute
every condition's predicate from the NZCV flags in parallel and let
`cond[3:0]` select the result —

```
taken = cond == 0000 ? 1                       // B   (always)
      : cond == 0001 ? Z                        // BEQ
      : cond == 0010 ? ~Z                       // BNE
      : cond == 0011 ? C                         // BCS/BHS
      : cond == 0100 ? ~C                        // BCC/BLO
      : cond == 0101 ? N                         // BMI
      : cond == 0110 ? ~N                        // BPL
      : cond == 0111 ? V                         // BVS
      : cond == 1000 ? ~V                        // BVC
      : cond == 1001 ? (C & ~Z)                  // BHI
      : cond == 1010 ? (~C | Z)                  // BLS
      : cond == 1011 ? (N ~^ V)                  // BGE  (N==V)
      : cond == 1100 ? (N ^ V)                   // BLT
      : cond == 1101 ? (~Z & (N ~^ V))           // BGT
      : cond == 1110 ? (Z | (N ^ V))             // BLE
      :                 1;                        // BL  (1111, always)
```

The predicates are all parallel functions of NZCV, selected by one
mux — a single select level after the (one- or two-gate) predicate
compute.

The encoding *does* have an elegant inverse-pair structure —
`cond[3:1]` names a pair (BEQ/BNE, BCS/BCC, …) and `cond[0]` selects
the sense, so `cond` and `cond ^ 1` are inverses for `0001`–`1110`.
That is worth knowing (it is why the table is laid out this way, and
it can save area). But it is **not** the shape to code on the timing
path: a "compute base predicate, then `XOR cond[0]`" formulation
serialises a pair-select mux *and then* a sense XOR, putting a
dependent XOR after the mux. The direct form folds `cond[0]` into
the same select instead of after it. Both are 8-input → 1-bit
functions that synthesis may well map to the same handful of LUTs,
but the direct form does not *depend* on the tool flattening the
serial XOR away — which is exactly why gen1 wrote it directly, and
gen2 keeps it that way.

## `writes_flags` / `reads_flags` derivation

These two bits wire the **flag bypass**
([Flag (NZCV) hazard model](./hazard-model.md#flag-nzcv-hazard-model)):
`writes_flags` marks a NZCV producer (its EX-computed flags ride the
EX/MEM and MEM/WB registers as a bypass source); `reads_flags` marks the
EX consumer that takes the youngest forwarded NZCV. NZCV is not
scoreboarded, so neither bit gates issue — but the decoder must still
produce them precisely, because they select the bypass.

**`writes_flags` = 1** for: all Format-R and Format-L arithmetic and
logic ops — ADD, SUB, ADC, SBC, AND, OR, XOR, NOT, SHL, SHR, SAR
(and their `#imm` forms), plus CMP/TEST (which are SUB/AND with the
F bit), plus MUL/MULU/DIV/DIVU (which set N,Z and force C=V=0). The
`flag_only` (F) bit does **not** change `writes_flags` — CMP still
writes flags; F only suppresses the *GPR* write.

`WRSPR SR` and `ERET` are also `writes_flags` producers: they write the
whole SR including its flag bits, so their EX-computed NZCV feeds the
bypass like any other producer (their S/I writes are ordered separately
by drain-commit).

**`writes_flags` = 0** for: MOV, LLI, LLIS, LUI, all loads/stores,
all branches (B/Bcc/BL), JMP/JALR, and the remaining system
instructions (EI, DI, SYSCALL, BREAK, RDSPR, RDSYS, WRSYS).

**`reads_flags` = 1** for: every conditional `Bcc` (cond `0001`–
`1110`); `ADC` and `SBC` (which take the carry flag as a third input);
and `RDSPR SR` (returns NZCV among other bits). The unconditional
`B`/`BL` (cond `0000`/`1111`) do **not** read flags.

## Exception-detect signals from decode

Two of the exception sources in
[Exception sources by detecting stage](./exception-flow.md#exception-sources-by-detecting-stage)
are produced by the decoder, combinationally in ID:

- **`illegal`** — the instruction word matches no legal
  opcode/operand form (a reserved opcode, an undefined
  format/opcode combination, a reserved field constraint violated).
  Raises `VEC_ILLEGAL`.
- **`priv_fault`** — a privileged op (EI, DI, ERET, RDSPR, WRSPR,
  RDSYS, WRSYS) is decoded with `SR.S = 0`. Raises `VEC_PRIV`.

Per [How the order is realised: structure first, small muxes second](./exception-flow.md#how-the-order-is-realised-structure-first-small-muxes-second),
`illegal` outranks `priv_fault` when both are asserted (an undefined
opcode that would also be privileged decodes as illegal). The
decoder may emit both bits; the local decode-fault mux picks
`illegal`. Both set `fault_pending` + `fault_vec` in the ID/EX
register and make the instruction inert
([Fault tags ride the pipeline registers](./exception-flow.md#fault-tags-ride-the-pipeline-registers)).

`SYSCALL` and `BREAK` are not faults — the decoder sets `is_trap`
and the corresponding vector, taken at EX, with the trap EPC
convention still to be reconciled
([EPC classification (faulting-PC vs next-PC)](./exception-flow.md#epc-classification-faulting-pc-vs-next-pc)).

## Per-stage local decoders

Downstream of ID, each stage consumes named bundle fields; the
"decoder" in each is a small selector, not an instruction-word
decoder ([Decode once in ID, narrow downstream](#decode-once-in-id-narrow-downstream)):

- **EX** — selects `alu_op`; drives the A/B operand muxes from
  `a_sel`/`b_sel`; computes the branch target (PC + `imm`) and
  evaluates `cond` against the forwarded NZCV
  ([Branch-condition evaluation](#branch-condition-evaluation), with
  flags from the bypass per
  [Flag (NZCV) hazard model](./hazard-model.md#flag-nzcv-hazard-model));
  recognises `drain_commit` to enter the drain-commit FSM; pulses
  `divmul.start` when `op_class=divmul`.
- **MEM** — consumes `ctrl_mem`: `mem_op` (none/load/store),
  `mem_size`, `sign_ext` for sub-word extract; `{sys_dev, sys_reg}`
  for the RDSYS sideband; the alignment check is combinational on the
  effective address + `mem_size`.
- **WB** — consumes `ctrl_wb`: `gpr_we`, `spr_we`, `flag_we`, and the
  destinations; gated by `~fault_pending`
  ([pipeline-stages.md §WB](./pipeline-stages.md#wb--writeback)).

## Cross-references

- [instruction-encoding.md](../../system/instruction-encoding.md),
  [instruction-set.md](../../system/instruction-set.md) — the
  authoritative ISA encoding and semantics.
- [ISA → physical register mapping](./hazard-model.md#isa--physical-register-mapping)
  — ISA→physical register mapping (R14 banking, SPR-USP cross-bank);
  [Flag (NZCV) hazard model](./hazard-model.md#flag-nzcv-hazard-model) — the
  `writes_flags`/`reads_flags` NZCV interaction.
- [Exception sources by detecting stage](./exception-flow.md#exception-sources-by-detecting-stage),
  [How the order is realised: structure first, small muxes second](./exception-flow.md#how-the-order-is-realised-structure-first-small-muxes-second)
  — where decode-produced faults fit the exception path.
- [pipeline-stages.md](./pipeline-stages.md) § Inter-stage pipeline
  registers — the `ctrl`/`ctrl_mem`/`ctrl_wb` field widths this
  bundle packs into.
- [Decision 6](./design-decisions.md#6-control-architecture-pure-hardwired-no-microcode)
  — the no-microcode, per-stage-hardwired decision this doc realises.
