# Penumbra CPU - Datapath

## Overview

The Penumbra CPU uses a three-bus datapath controlled by horizontal microcode. The PC is separate from the register file and ALU, with its own dedicated adder for branches. All data operations flow through the ALU; all control flow operations go through the PC unit.

## Block Diagram

```
                         ┌─────────────────────────────────┐
                         │       Instruction Register (IR)  │
                         │       (32-bit latch from MDR)    │
                         └───┬──────────┬──────────┬───────┘
                             │          │          │
                        field extract   │    branch offset
                      ┌──────┴───┐  ┌───┴───────┐  │
                      │  Rd/Rs/  │  │ Immediate │  │
                      │  Rb/op   │  │ Extractor │  │
                      │ selects  │  │(sign/zero │  │
                      └──┬───┬───┘  │ extend)   │  │
                         │   │      └─────┬─────┘  │
                 ┌───────┘   │            │         │
                 v           v            │         │
          ┌─────────────────────────┐     │         │
          │     Register File       │     │         │
          │  15 entries: R0-R14     │     │         │
          │  R0: reads as 0         │     │         │
          │  R14: USP/SSP banked    │     │         │
          │  R15 addr: returns PC ──┼─────┼─────────┼── (from PC register)
          │                         │     │         │
          │  Read Port A ──► A-bus  │     │         │
          │  Read Port B ──► B-mux  │     │         │
          │                  │      │     │         │
          │  Write Port ◄── W-mux   │     │         │
          └─────────────────────────┘     │         │
              │          │          │     │         │
           A-bus      B-mux      W-mux   │         │
              │       ┌──┴──┐   ┌──┴──┐  │         │
              │       │     │   │     │  │         │
              │       v     v   │     │  │         │
              │   Reg Port B   │     │  │         │
              │       │   Imm ──┘     │  │         │
              │       └──┬──┘        │  │         │
              │       B-bus          │  │         │
              │          │           │  │         │
              v          v           │  │         │
          ┌──────────────────┐       │  │         │
          │       ALU        │       │  │         │
          │  A-bus × B-bus   │       │  │         │
          │                  │       │  │         │
          │  flags ────────► SR     │  │         │
          └────────┬─────────┘       │  │         │
                   │                 │  │         │
                R-bus                │  │         │
                   │                 │  │         │
            ┌──────┼──────┐          │  │         │
            v      │      v          │  │         │
          MAR      │   W-mux in 0 ──┘  │         │
          (R-bus   │                    │         │
           input)  │                    │         │
            │      │                    │         │
      ┌─────┴──────┴──────┐            │         │
      │    MAR register    │            │         │
      │  ┌──────────────┐ │            │         │
      │  │ 2:1 mux      │ │            │         │
      │  │ R-bus / PC ───┤ │            │         │
      │  └──────┬───────┘ │            │         │
      │         v         │            │         │
      │   To MMU / Cache  │            │         │
      └─────────┬─────────┘            │         │
                │                      │         │
         ┌──────┴──────┐               │         │
         │ MMU / Cache  │               │         │
         │   / Bus      │               │         │
         └──────┬───────┘               │         │
                │                      │         │
       ┌────────┴────────┐             │         │
       │  MDR register   │             │         │
       │                 │             │         │
       │  Read data ─────┼──► W-mux in 1         │
       │  Write data ◄───┼── A-bus               │
       │  Instruction ───┼──► IR load             │
       └─────────────────┘             │         │
                                       │         │
          ┌────────────────────────────┼─────────┼──────────┐
          │          PC Unit           │         │          │
          │                            │         │          │
          │  ┌─────────────────┐       │         │          │
          │  │   PC Register   │───────┼──► R15 read mux   │
          │  └──┬──────────────┘       │                    │
          │     │                      │                    │
          │     │   ┌──────────────┐   │                    │
          │     ├──►│  PC Adder    │   │                    │
          │     │   │  (shared)    │   │                    │
          │     │   │              │   │                    │
          │     │   │  in B: mux ◄─┼───┼── 4 (fetch)       │
          │     │   │         ◄────┼───┘   offset (branch,  │
          │     │   │              │       from IR)         │
          │     │   └──────┬───────┘                        │
          │     │          │                                │
          │     │    ┌─────┴─────────────┐                  │
          │     │    │  PC source mux    │                  │
          │     │    │  00: hold         │                  │
          │  ┌──┤    │  01: PC + 4       │                  │
          │  │  │    │  10: PC + offset  │                  │
          │  │  │    │  11: A-bus (JMP)  │◄── A-bus         │
          │  │  │    │  MDR (exception)  │◄── MDR           │
          │  │  │    └──────┬────────────┘                  │
          │  │  │           │                               │
          │  │  │    ┌──────┴───────┐                       │
          │  │  └───►│ new PC value │──► PC Register        │
          │  │       └──────────────┘                       │
          │  │                                              │
          │  └──► MAR (for instruction fetch)               │
          │                                                 │
          │  PC+4 ──► Register File Write Port (for BL)     │
          └─────────────────────────────────────────────────┘


The ALU is **single-cycle only**: arithmetic, logic, shifts, comparisons, and
PASS-through all complete combinationally; the ALU has no internal state and
never stalls. Multi-cycle compute lives in **peer units** that sit alongside
the ALU — the divmul unit handles MUL/DIV (32×32→64 multiply, 32/32 divide;
the optional write-only `Rdh` operand carries the product high half or the
remainder — see [divmul.md](./divmul.md)), and a
future FPU unit will handle floating-point.
Each peer unit owns its own start/busy/op signals (`divmul_start`,
`divmul_busy`, `divmul_op`; later `fpu_start`/`fpu_busy`/`fpu_op`) and its
own internal accumulators and FSM. The micro-sequencer treats them
identically: pulse the relevant `*_start` in the start micro-op, then a
STALL micro-op holds micro-PC until the unified busy gate
(`cache_busy | divmul_busy | …`) clears.

A peer unit's writeback shape can differ from the ALU's. The divmul produces
two 32-bit halves that drive R-bus on consecutive cycles (low half → `Rd`,
high half → `Rdh` from IR[15:12]), so its micro-routine has two writeback
micro-ops, not one. The ALU's own writeback is unchanged — single cycle,
single register.

MUL/DIV execute on the divmul peer unit ([divmul.md](./divmul.md)); the
two-writeback micro-routine above is theirs. FPU operations follow the same
template once their unit exists.
```

## Data Flow by Instruction Type

### ALU Register-Register (e.g., ADD Rd, Rs)

Single micro-op:
```
reg_a_sel = Rd       → A-bus = Rd value
reg_b_sel = Rs       → B-bus = Rs value (b_mux_sel = register)
alu_op = ADD
R-bus = A + B        → W-mux selects R-bus → register file writes Rd
flag_w_en = 1        → SR updated
pc_src = PC+4        → advance to next instruction
```

### ALU Flag-Only (e.g., CMP Rd, Rs)

Same micro-routine as ADD. The micro-word sets `reg_w_en = 1`, but hardware gates it: `actual_w_en = reg_w_en & ~(format_R & IR[16])`. The F bit (IR[16]) suppresses the register write for all Format R flag-only variants (CMP, TEST). Flags are updated, Rd is not modified.

### Immediate (e.g., INC Rd, #imm16)

Single micro-op:
```
reg_a_sel = Rd       → A-bus = Rd value
b_mux_sel = immediate, imm_mode = zero_extend
alu_op = ADD
R-bus = Rd + imm     → W-mux selects R-bus → register file writes Rd
```

### Immediate (e.g., LLI Rd, #imm16)

Single micro-op:
```
b_mux_sel = immediate, imm_mode = zero_extend
alu_op = PASS_B      → R-bus = zero_extend(imm16)
W-mux selects R-bus  → register file writes Rd
```

### Immediate (e.g., LUI Rd, #imm16)

Single micro-op:
```
reg_a_sel = Rd       → A-bus = Rd value
b_mux_sel = immediate, imm_mode = shift_left_16
alu_op = OR          → R-bus = Rd | (imm16 << 16)
W-mux selects R-bus  → register file writes Rd
```

### Load (e.g., LDW Rd, [Rb + offset])

Micro-op 0 — compute address:
```
reg_a_sel = Rb       → A-bus = Rb value
b_mux_sel = immediate, imm_mode = sign_extend (offset from IR)
alu_op = ADD
R-bus = Rb + offset  → MAR loaded from R-bus
```

Micro-op 1 — memory read:
```
mem_read = 1, mem_size = word
(stall until cache/bus ready)
MDR loaded with memory data
```

Micro-op 2 — write back:
```
W-mux selects MDR    → register file writes Rd
pc_src = PC+4        → advance to next instruction
```

### Store (e.g., STW Rd, [Rb + offset])

Micro-op 0 — compute address:
```
reg_a_sel = Rb       → A-bus = Rb value
b_mux_sel = immediate, imm_mode = sign_extend
alu_op = ADD
R-bus = Rb + offset  → MAR loaded
```

Micro-op 1 — drive data and write:
```
reg_a_sel = Rd       → A-bus = Rd value → MDR loaded from A-bus
mem_write = 1, mem_size = word
(stall until cache/bus ready)
pc_src = PC+4        → advance to next instruction
```

### Branch (e.g., BEQ offset)

Micro-op 0 — evaluate condition and branch:
```
Check SR flags against condition field from IR
If taken:  pc_src = PC + offset (branch adder, offset from IR)
If not taken: pc_src = PC + 4
```

Single micro-op — the branch adder computes PC + offset in parallel with the condition check. The `branch_cond` field (BRT or BRF) gates `pc_src`: if the ISA condition is met, `pc_src = PC + offset` takes effect; otherwise hardware forces `pc_src = PC + 4`. The micro-sequencer hands off to the fetch unit regardless of the condition outcome. See `microcode-validation.md` §3 for the detailed micro-word.

### Branch-and-Link (BL offset)

Micro-op 0 — save return address:
```
reg_a_sel = R15      → A-bus = PC value (via R15 read mux)
b_mux_sel = immediate, imm = 4
alu_op = ADD
R-bus = PC + 4       → W-mux selects R-bus → register file writes R13 (link register)
```

Micro-op 1 — branch:
```
pc_src = PC + offset (branch adder)
```

### JMP Rs (indirect jump / function return)

Single micro-op:
```
reg_a_sel = Rs       → A-bus = Rs value
pc_src = A-bus       → PC loaded from A-bus
```

Assembler alias: `RET` = `JMP R13`

### EI (Enable Interrupts)

Single micro-op:
```
SR.I = 1
ei_shadow = 1              → flip-flop: suppress IRQ check for next instruction
→ return to fetch
```

The `ei_shadow` flip-flop is checked by the fetch unit during the interrupt check. When set, the pending-interrupt check is skipped for one instruction cycle, then the flip-flop clears. This provides the one-instruction delay guarantee.

### DI (Disable Interrupts)

Single micro-op:
```
SR.I = 0                   → immediate effect
→ return to fetch
```

### RDSPR Rd, {ESR|EPC|USP}

Single micro-op:
```
a_src = SPR (hardware decodes IR[15:12] to select ESR, EPC, or R14 cross_bank)
ALU PASS_A → R-bus → Rd
```

### WRSPR {ESR|EPC|USP}, Rd

Single micro-op:
```
reg_a_sel = Rd       → A-bus = Rd value → ALU PASS_A → R-bus
sys_op = SPR_WRITE (hardware decodes IR[15:12] to route R-bus to ESR, EPC, or R14 cross_bank write)
```

### WRSYS Rd, #dev, #reg

Single micro-op:
```
reg_a_sel = Rd       → A-bus = Rd value → data bus
sys_op = SYS_WRITE, sys_dev = IR[15:12], sys_reg = IR[11:8]
```

### RDSYS Rd, #dev, #reg

Single micro-op:
```
sys_op = SYS_READ, sys_dev = IR[15:12], sys_reg = IR[11:8]
Device drives data bus → MDR
W-mux selects MDR → register file writes Rd
```

### Exception / Interrupt Entry

Exception entry has two phases: hardware pre-actions (atomic, before microcode) and a microcode sequence.

**Hardware pre-actions** (triggered atomically by the fetch unit when an interrupt/exception is recognized):
1. Latch exception registers: `ESR ← SR`, `EPC ← PC` (return address; faulting PC for exceptions)
2. Mode switch: `SR.S ← 1`, `SR.I ← 0`
3. SP bank swap: R14 now reads/writes SSP
4. Latch vector number from source (priority encoder for IRQs, hardwired per exception type)

**Microcode sequence** (7 micro-ops + stall loops; uses `a_src` for exception registers, `b_mux_sel` for constants 4/8):
```
int-0: reg_a=R14(SSP), b_mux=const_4, SUB → MAR = SSP - 4
int-1: a_src=ESR → MDR, mem_write     (stall loop)
int-2: reg_a=R14(SSP), b_mux=const_8, SUB → MAR = SSP - 8, also write R14 = SSP - 8
int-3: a_src=EPC → MDR, mem_write     (stall loop)
int-4: a_src=vector_addr, PASS_A → MAR      (vec_num × 4, pre-shifted)
int-5: mem_read                              (stall loop)
int-6: pc_src=MDR → PC = handler address, hand off to fetch unit
```

See `microcode-validation.md` §4 for the full bit-level micro-word table and signal trace.

## Condition Flags

### Overview

The ALU produces four condition flags: Z (zero), N (negative), C (carry), V (overflow). These are latched into the SR when `flag_w_en` is asserted in the micro-word. The carry convention is **ARM-style** (C = NOT borrow on subtraction), which matches the ISA's condition code table directly.

### Carry Convention

Subtraction is implemented as `A + ~B + 1` using the adder with B inverted and carry-in set to 1. The carry flag is the carry-out of this addition:

- `5 - 3`: `5 + ~3 + 1` = `5 + 0xFFFFFFFC + 1` → Cout = 1 → **C=1** (no borrow, A ≥ B)
- `3 - 5`: `3 + ~5 + 1` = `3 + 0xFFFFFFFA + 1` → Cout = 0 → **C=0** (borrow, A < B)

This means: C=1 after SUB/CMP ↔ unsigned A ≥ B. No inversion needed — Cout of the adder is used directly as C for both ADD and SUB.

### Hardware Implementation

```
sub_mode = 1 for SUB/CMP/DEC/CMPI, 0 for ADD/INC

B_eff = B XOR {32{sub_mode}}     (invert B for subtraction)
Cin   = sub_mode                   (add 1 for two's complement)

{Cout, result} = A + B_eff + Cin

Z = (result == 0)                  (32-input NOR)
N = result[31]                     (wire)
C = Cout                           (carry out of bit 31, direct from adder)
V = Cout[31] XOR Cout[30]         (carry into bit 31 differs from carry out)
```

V can equivalently be computed as:
```
ADD: V = (A[31] == B[31])     && (result[31] != A[31])
SUB: V = (A[31] != B[31])     && (result[31] != A[31])
```

Both forms use the same gates when computed on the adder's actual inputs (A and B_eff).

### Flag Generation Per Operation

| Operation | Z | N | C | V |
|-----------|---|---|---|---|
| ADD, INC | result==0 | result[31] | Cout | signed overflow |
| SUB, CMP, DEC, CMPI | result==0 | result[31] | Cout (= NOT borrow) | signed overflow |
| AND, TEST | result==0 | result[31] | 0 | 0 |
| OR | result==0 | result[31] | 0 | 0 |
| XOR | result==0 | result[31] | 0 | 0 |
| NOT | result==0 | result[31] | 0 | 0 |
| SHL | result==0 | result[31] | last bit shifted out (msb) | 0 |
| SHR | result==0 | result[31] | last bit shifted out (lsb) | 0 |
| SAR | result==0 | result[31] | last bit shifted out (lsb) | 0 |
| MUL, MULU, DIV, DIVU | Rd == 0 (low half / quotient) | Rd[31] | 0 | 0 |

### Which Instructions Update Flags

The `flag_w_en` micro-word bit controls whether ALU flag outputs are latched into SR. The microcode sets this per instruction:

| Instruction | Flags updated | Notes |
|-------------|--------------|-------|
| ADD, SUB, AND, OR, XOR, NOT | Yes | Standard ALU operations |
| SHL, SHR, SAR | Yes | C = last bit shifted out; shift by 0 clears C |
| CMP (SUB with F=1) | Yes | Flags only, no register write |
| TEST (AND with F=1) | Yes | Flags only, no register write |
| MUL, MULU, DIV, DIVU | Yes | Z and N meaningful; C=0, V=0 |
| INC, DEC | Yes | Arithmetic flags |
| CMPI | Yes | Compare to immediate, flags only |
| **MOV** | **No** | Data movement, must not clobber flags |
| LLI, LLIS, LUI | No | Constant loading |
| Loads, stores | No | Memory access |
| Branches | No | Control flow |
| System (WRSYS, RDSYS, RDSPR, WRSPR, JMP, ERET, etc.) | No | System operations |

### Condition Code Evaluation

The branch condition field (4 bits from Format B instructions) is evaluated against SR flags. The ARM-style carry convention ensures these mappings are correct:

| cond | Mnemonic | Test | Unsigned meaning | Signed meaning |
|------|----------|------|-----------------|----------------|
| 0000 | AL | true | always | always |
| 0001 | EQ | Z=1 | equal | equal |
| 0010 | NE | Z=0 | not equal | not equal |
| 0011 | CS/HS | C=1 | ≥ (higher or same) | — |
| 0100 | CC/LO | C=0 | < (lower) | — |
| 0101 | MI | N=1 | — | negative |
| 0110 | PL | N=0 | — | positive or zero |
| 0111 | VS | V=1 | — | overflow |
| 1000 | VC | V=0 | — | no overflow |
| 1001 | HI | C=1 & Z=0 | > (higher) | — |
| 1010 | LS | C=0 \| Z=1 | ≤ (lower or same) | — |
| 1011 | GE | N=V | — | ≥ |
| 1100 | LT | N≠V | — | < |
| 1101 | GT | Z=0 & N=V | — | > |
| 1110 | LE | Z=1 \| N≠V | — | ≤ |
| 1111 | BL | true (link) | always + save LR | always + save LR |

Condition evaluation hardware: each condition is a simple combinational function of at most 3 flag bits. A 4-to-1 mux tree selects the result based on the cond field. In discrete, this is ~3-4 chips (a few gates plus a 74x150 16:1 mux or equivalent).

## Micro-Word Format

### Fields

| Bits | Field | Width | Description |
|------|-------|-------|-------------|
| 50 | `priv` | 1 | Privileged instruction marker — sequencer checks `SR.S` on the first micro-op and traps `VEC_PRIV` if `SR.S=0`. Replaced the old `branch_cond=PRIV` value. |
| 49:47 | `a_src[2:0]` | 3 | A-bus source: 0=register file, 1=ESR, 2=EPC, 3=vector_addr, 4=SPR (decode IR[15:12]) |
| 46:43 | `reg_a_sel[3:0]` | 4 | Register file read port A address (used when a_src=00) |
| 42:39 | `reg_b_sel[3:0]` | 4 | Register file read port B address |
| 38:35 | `reg_w_sel[3:0]` | 4 | Register file write port address |
| 34 | `reg_w_en` | 1 | Register file write enable (hardware-gated by Format R F bit: `actual = reg_w_en & ~(format_R & IR[16])`) |
| 33:29 | `alu_op[4:0]` | 5 | ALU operation (see ALU Op Encoding table) |
| 28:27 | `b_mux_sel[1:0]` | 2 | B-bus source: 00=register port B, 01=IR immediate, 10=constant 4, 11=constant 8 |
| 26 | `w_mux_sel` | 1 | Write-back source: 0=R-bus, 1=MDR |
| 25:24 | `imm_mode[1:0]` | 2 | IR immediate handling: 00=zero-extend, 01=sign-extend, 10=shift-left-16 |
| 23 | `flag_w_en` | 1 | Update SR condition flags (NZCV) from ALU |
| 22 | `sr_load` | 1 | Load full SR from W-mux output (for ERET) |
| 21 | `mar_load` | 1 | Load MAR from R-bus (D-cache/bus address only; I-cache is permanently wired to PC) |
| 20 | `mdr_load_mem` | 1 | Load MDR from D-cache/memory (read data) |
| 19 | `mdr_load_a` | 1 | Load MDR from A-bus (for stores) |
| 18 | `mem_read` | 1 | Initiate D-cache/memory read |
| 17 | `mem_write` | 1 | Initiate D-cache/memory write |
| 16:15 | `mem_size[1:0]` | 2 | Access size: 00=byte, 01=half, 10=word |
| 14 | `sign_ext` | 1 | Sign-extend sub-word load result |
| 13:11 | `pc_src[2:0]` | 3 | PC source: 000=hold, 001=PC+4, 010=PC+offset, 011=A-bus, 100=MDR |
| 10:9 | `sys_op[1:0]` | 2 | System/SPR operation: 0=NONE, 1=SPR_WRITE, 2=SYS_READ, 3=SYS_WRITE |
| 8 | `alu_start` | 1 | Start multi-cycle ALU operation. Currently tied off — the ALU is single-cycle only; divmul implementation will repurpose this bit (or replace it with `divmul_start`) once the peer unit lands. |
| 7:5 | `branch_cond[2:0]` | 3 | Micro-sequencer control (see below) |
| 4:2 | `fwd_offset[2:0]` | 3 | Forward skip offset, 0-7 (used only when branch_cond=SKIP) |
| 1 | `ei_set` | 1 | Set `SR.I=1` with one-instruction delay via `ei_shadow`. Used only by `EI`. |
| 0 | `di_set` | 1 | Set `SR.I=0` immediately. Used only by `DI`. |

**Total: 51 bits**

See `doc/internals/microcode.md` for the authoritative single-source field reference, value tables, and full micro-routine catalog. The summary above mirrors `hw/rtl/core/sequencer.sv` field extraction at lines 111–136.

The ALU is the **single-cycle** compute unit. All ALU operations (ADD, SUB, AND, OR, XOR, SHL, SHR, SAR, PASS_A, PASS_B, NOT) produce results combinationally; the ALU has no internal state and never asserts a busy signal. Multi-cycle compute is delegated to **peer units** alongside the ALU — the divmul unit owns MUL/DIV (32×32→64 multiply, 32/32 divide; the optional write-only `Rdh` operand carries the product high half or remainder — see [divmul.md](./divmul.md)), and a future FPU unit will own floating-point. Each peer has its own `*_start`/`*_busy`/`*_op` signals in the micro-word, its own operand latches and FSM, and joins the unified STALL gate (`cache_busy | divmul_busy | fpu_busy`).

Future FPU note: floating-point operands live in GPRs (no separate FP register file). The FPU will be its own peer unit, structurally identical to the divmul: `fpu_start`/`fpu_busy`/`fpu_op` in the micro-word, its own internal state, joined into the same STALL gate. FP compare updates NZCV via `flag_w_en`, so normal Bcc works for FP branches. When FPU hardware is absent, the microcode ROM fills FP opcode entries with illegal-instruction exception micro-ops — zero runtime overhead, and the FPU unit need not exist in silicon. MUL/DIV already took exactly this trajectory: once illegal-instruction traps in the ROM, their opcode entries now dispatch to the divmul peer unit.

Design history: the original draft specified 52 bits (actually 55 when counted correctly). Microcode validation identified missing signals for exception entry and unnecessary sequencer complexity. The separate long-latency unit (`lu_op[1:0]`) was initially folded into the ALU as a unified compute unit, with `alu_op` expanded to 5 bits. The privilege check moved from a microcode branch (old `branch_cond=PRIV`) to a dedicated `priv` bit at [50], and the `ei_set`/`di_set` bits at [1:0] (formerly spare) absorbed the EI/DI side effects, giving the 51-bit micro-word documented in the table above.

Subsequent design note: when hardware planning for MUL/DIV began, the divmul work re-extracted them as a **peer unit** alongside the ALU — same micro-sequencer protocol (start / busy / STALL), but private `divmul_start`/`divmul_busy`/`divmul_op` signals so divmul's iteration mux does not deepen the ALU's combinational path (the fmax-critical `upc → µROM → ctrl → regfile → ALU → flag_z` chain). When divmul landed, `divmul_start` reused the old `alu_start` micro-word bit rather than widening the word (see [divmul.md](./divmul.md) and `microcode.md`). The ISA contract is unchanged — only the implementation moved.

### Micro-Sequencer

The micro-sequencer uses a micro-PC register to index into the microcode ROM. Sequencing is controlled by `branch_cond` — no absolute jump addresses are needed. All micro-routines are linear sequences with stall holds and fetch-unit handoff.

| `branch_cond` | Mnemonic | micro-PC action | `pc_src` behavior |
|---------------|----------|-----------------|-------------------|
| 000 | SEQ | micro-PC++ | unconditional |
| 001 | FETCH | hand off to fetch unit | unconditional |
| 010 | STALL | busy ? hold : (fault ? exception via fetch unit : micro-PC++) | unconditional |
| 011 | BRT | hand off to fetch unit | applied if ISA cond true, else forced to PC+4 |
| 100 | BRF | hand off to fetch unit | applied if ISA cond false, else forced to PC+4 |
| 101 | (reserved) | — | — |
| 110 | SKIP | micro-PC += 1 + fwd_offset | unconditional |
| 111 | ILLEGAL | illegal-instruction sentinel | traps to `VEC_ILLEGAL` |

FETCH, BRT, and BRF all signal "instruction complete" to the fetch unit. The difference is `pc_src` handling: FETCH applies `pc_src` unconditionally; BRT/BRF conditionally gate `pc_src` based on the ISA condition evaluator, enabling single-micro-op conditional branches.

Privilege checking uses the `priv` micro-word bit (bit 50), not a `branch_cond` value. When `priv=1` and `SR.S=0`, the sequencer signals the fetch unit to trigger a privilege violation (vector 4, `VEC_PRIV`) using the same hardware pre-actions as interrupt entry. The old `branch_cond=PRIV` (encoding 101) value was removed when this moved into a per-µ-op bit.

STALL checks a unified busy signal: the OR of every multi-cycle unit's busy line. Today that is `divmul_busy | mem_busy` (see `sequencer.sv`); the FPU adds `fpu_busy` later. Adding a peer unit means wiring its busy line into this OR — no other sequencer change. When the operation completes (`busy` deasserts), the sequencer also checks a `fault` signal. Three-way resolution:

- **busy=1:** Hold micro-PC (keep waiting).
- **busy=0, fault=0:** micro-PC++ (normal completion).
- **busy=0, fault=1:** Trigger exception. The fault vector is selected by priority: bus fault (0), alignment (8), TLB protection (3), TLB miss (2), then arithmetic fault (10) from divmul (`VEC_ARITH` — DIV0; defensive, never expected on correct code). Hardware pre-actions fire with `EPC ← PC` (still pointing at the faulting instruction, since `pc_src=001` hasn't executed). The instruction is effectively aborted mid-execution. Memory-side faults are detected by the bus/MMU; arithmetic faults come from the divmul unit's combinational `o_fault` (asserted on the start cycle).

Different peer units never overlap in the same micro-op (the µ-routine only ever pulses one `*_start` at a time), so a single OR'd busy line and a single OR'd fault line are sufficient — the fault-source priority above tells the sequencer which vector to take.

### Microcode ROM Organization

The microcode ROM uses **direct mapping** from instruction bits to micro-PC start address. The fetch unit computes the dispatch address from IR bits:

```
Format R: dispatch = {0, op[4], 0, op[3:0], 0}   → ALU 0x00–0x1E (op[4]=0), SYS 0x40–0x5E (op[4]=1)
Format L: dispatch = {01, op[3:0], 0}            → entries 0x20–0x3E (×2 spacing)
Format M: dispatch = {10, L, sz[1:0], SE, 00}    → entries 0x80–0xBC (×4 spacing)
Format B: dispatch = (cond==1111) ? 0x62 : 0x60  → Bcc at 0x60, BL at 0x62
Exception: hardwired                              → entry 0x70
```

Multi-micro-op instructions (loads, stores, JALR, RDSYS, ERET, BL) occupy consecutive ROM addresses inside their dispatch slot. The microcode assembler validates slot boundaries (see `doc/internals/microcode.md` § ROM Organization for the slot-size table per zone). Interrupt entry (`int_entry`) occupies a dedicated 4-µ-op slot at 0x70, reached by hardware dispatch on `except_entry`.

Microcode ROM size: 256 entries × 51 bits ≈ 13 Kbit (1 EBR on ECP5, or 7 byte-wide ROM chips in discrete with 5 spare bits for future expansion).

## Instruction Fetch

Instruction fetch is handled by a **hardware fetch unit**, not by microcode. This eliminates fetch overhead from the micro-routine and provides a clean upgrade path to prefetched execution.

### Fetch Unit Interface

| Signal | Direction | Purpose |
|--------|-----------|---------|
| `fetch_go` | sequencer → fetch | Triggered when branch_cond ∈ {FETCH, BRT, BRF, PRIV (on fail), STALL (on fault)} |
| `ir_valid` | fetch → sequencer | Instruction latched in IR, ready for dispatch |
| `dispatch_addr` | fetch → sequencer | Micro-PC start address for the new instruction |
| `fetch_invalidate` | datapath → fetch | Discard prefetch on branch taken (phase 2) |
| `mem_fault` | MMU → sequencer | Fault detected during memory access (checked on STALL resolution) |
| `fault_vector[3:0]` | cpu_top | Exception vector: VEC_TLB_MISS=2 (`!mmu_hit`), VEC_TLB_PROT=3 (`mmu_hit`) |

### Fetch Unit Behavior

The fetch unit serves as the **unified exception dispatch point** for all exception sources:

When `fetch_go` is asserted (instruction complete, privilege check failed, or memory fault detected):

1. **Exception check** (in priority order):
   - **Memory fault:** If triggered by STALL-with-fault → hardware pre-actions with vector from `fault_vector`, dispatch to exception entry. `EPC` = faulting instruction (PC not yet advanced).
   - **Privilege violation:** If triggered by PRIV branch_cond → hardware pre-actions with vector 3, dispatch to exception entry.
   - **Pending interrupt:** If IRQ pending, SR.I=1, and ei_shadow not set → hardware pre-actions with vector from priority encoder, dispatch to exception entry.
2. **Instruction fetch:** Read I-cache at current PC (I-cache address is permanently wired to PC — split I/D cache). On hit: latch IR, compute dispatch address, assert `ir_valid`. On miss: stall until ready.

Illegal instruction exceptions (vector 2) are handled by ROM content: undefined opcode entries contain exception-triggering micro-ops. FPU-absent traps use the same mechanism — the ROM image for systems without an FPU fills FP opcode entries with illegal instruction exception code.

PC advancement is controlled by `pc_src` in the micro-word, not the fetch unit. Non-branch instructions set `pc_src=001` (PC+4) in their last micro-op; branch instructions set `pc_src=010` (PC+offset) or `011` (A-bus). The fetch unit reads I-cache at whatever address PC holds.

### Prefetch Upgrade Path (phase 2)

The fetch unit interface supports transparent upgrade to prefetched execution:

- **Phase 1 (initial):** Fetch starts on `fetch_go`. Cost: I-cache latency per instruction.
- **Phase 2 (future):** Fetch starts autonomously when PC changes. On `fetch_go`, `ir_valid` may already be asserted → 0-cycle fetch on I-cache hit. Taken branches assert `fetch_invalidate` to restart the prefetch at the new PC.

No microcode changes are needed for the upgrade — the sequencer sees the same `ir_valid`/`dispatch_addr` interface in both phases.
