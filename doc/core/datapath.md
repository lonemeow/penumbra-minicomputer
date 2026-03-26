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
          │  R14: USP/KSP banked    │     │         │
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


Long-latency units (directly connected to A-bus, B-bus, R-bus):

          A-bus ──► operand_a ──┐
          B-bus ──► operand_b ──┤  ┌──────────┐
                                ├──│ MUL unit │──► R-bus (via mux)
                  start ────────┤  └──────────┘
                                │  ┌──────────┐
                                ├──│ DIV unit │──► R-bus (via mux)
                                │  └──────────┘
                                │  ┌──────────┐
                                └──│ FPU      │──► R-bus (via mux)
                                   │ (future) │
                                   └──────────┘
                  busy/done ◄───── selected unit
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

Same as above, but `reg_w_en = 0`. Flags are updated, Rd is not modified.

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
R-bus = Rb + offset  → MAR loaded (mar_src = R-bus)
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

Single micro-op — the branch adder computes PC + offset in parallel with the condition check. The pc_src mux selects based on the condition result.

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

### MTSYS Rd, #dev, #reg

Single micro-op:
```
reg_a_sel = Rd       → A-bus = Rd value → data bus
sys_cycle = 1, sys_dev = IR[15:12], sys_reg = IR[11:8], sys_we = 1
```

### MFSYS Rd, #dev, #reg

Single micro-op:
```
sys_cycle = 1, sys_dev = IR[15:12], sys_reg = IR[11:8], sys_we = 0
Device drives data bus → MDR
W-mux selects MDR → register file writes Rd
```

### Exception / Interrupt Entry

Multi-step microcode sequence:
```
micro-op 0: Save SR   → MDR = SR, MAR = KSP - 4, mem_write
micro-op 1: Save PC   → MDR = PC, MAR = KSP - 8, mem_write
micro-op 2: Update SP  → KSP = KSP - 8
micro-op 3: Set mode   → SR.S = 1, SR.I = 0, swap to KSP
micro-op 4: Load vector → MAR = vector_table_base + (vector_num × 4), mem_read
micro-op 5: Jump       → PC = MDR (vector address)
```

## Micro-Word Format

### Fields

| Field | Bits | Description |
|-------|------|-------------|
| `reg_a_sel[3:0]` | 4 | Register file read port A address |
| `reg_b_sel[3:0]` | 4 | Register file read port B address |
| `reg_w_sel[3:0]` | 4 | Register file write port address |
| `reg_w_en` | 1 | Register file write enable |
| `alu_op[3:0]` | 4 | ALU operation (ADD, SUB, AND, OR, XOR, SHL, SHR, SAR, PASS_A, PASS_B, NOT) |
| `b_mux_sel` | 1 | B-bus source: 0=register port B, 1=immediate |
| `w_mux_sel` | 1 | Write-back source: 0=R-bus, 1=MDR |
| `imm_mode[1:0]` | 2 | Immediate handling: 00=zero-extend, 01=sign-extend, 10=shift-left-16 |
| `flag_w_en` | 1 | Update SR condition flags from ALU |
| `mar_load` | 1 | Load MAR from R-bus |
| `mar_src` | 1 | MAR input: 0=R-bus, 1=PC (for instruction fetch) |
| `mdr_load_mem` | 1 | Load MDR from memory/cache (read) |
| `mdr_load_a` | 1 | Load MDR from A-bus (for stores) |
| `mem_read` | 1 | Initiate memory/cache read |
| `mem_write` | 1 | Initiate memory/cache write |
| `mem_size[1:0]` | 2 | Access size: 00=byte, 01=half, 10=word |
| `sign_ext` | 1 | Sign-extend sub-word load result |
| `pc_src[1:0]` | 2 | PC source: 00=hold, 01=PC+4, 10=PC+offset, 11=A-bus |
| `pc_mdr_load` | 1 | Load PC from MDR (exception vector) |
| `stall_sel[1:0]` | 2 | Stall source: 00=none, 01=MUL, 10=DIV, 11=FPU |
| `sys_cycle` | 1 | System register bus cycle |
| `sys_we` | 1 | System register write enable |
| `lu_start` | 1 | Start long-latency unit operation |
| `lu_sel[1:0]` | 2 | Long-latency unit select: 00=MUL, 01=DIV, 10=FPU |
| `lu_to_rbus` | 1 | Drive long-latency unit result onto R-bus |
| `next_addr[9:0]` | 10 | Micro-branch target address |
| `branch_cond[2:0]` | 3 | Micro-branch condition |

**Total: 52 bits**

### Micro-Branch Conditions

| `branch_cond` | Meaning |
|---------------|---------|
| 000 | Never (sequential, micro-PC increments) |
| 001 | Always (unconditional micro-jump) |
| 010 | If stalled (selected unit busy) |
| 011 | If ISA condition true (for conditional branches, checks IR cond field against SR flags) |
| 100 | If ISA condition false |
| 101-111 | (reserved) |

### Microcode ROM Organization

The microcode ROM uses **direct mapping** from instruction bits to micro-PC start address. The top bits of the instruction (format prefix + opcode) form the entry point address:

```
Format R: micro-PC = {00, op[4:0]}       → entries 0-31
Format L: micro-PC = {01, op[2:0], 00}   → entries 32-39 (×4 spacing for multi-op routines)
Format M: micro-PC = {10, L, sz[1:0], SE} → entries 40-55 (×4 spacing)
Format B: micro-PC = {11, 0000000}        → entry 56 (single routine, condition checked internally)
```

Exact mapping TBD during microcode development. Entry points are spaced to allow multi-micro-op routines to occupy consecutive addresses without colliding.

Microcode ROM size: 1024 entries × 52 bits = 52 Kbit (3 EBRs on ECP5, or 7 byte-wide ROM chips in discrete).

## Instruction Fetch Cycle

Before each instruction executes, the fetch micro-routine runs:

```
fetch-0: mar_src=PC, mar_load=1           → MAR = PC (drive fetch address)
fetch-1: mem_read=1, stall until ready     → read instruction from I-cache/memory
fetch-2: MDR → IR load, pc_src=PC+4        → latch instruction, advance PC
fetch-3: micro-PC = direct map from IR     → jump to instruction's micro-routine
```

After the instruction's micro-routine completes, control returns to fetch-0.
