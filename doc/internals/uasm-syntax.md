# Microcode Assembler (uasm.py) Syntax

The Penumbra microcode is written in a symbolic assembly format and translated into a hexadecimal memory file for the FPGA/Verilator ROM using `hw/tools/uasm.py`.

## File Structure

- **Comments:** Lines starting with `#` are comments.
- **Directives:** `.org <address>` sets the dispatch address for the following micro-routine.
- **Labels:** `label:` defines a symbolic name for a micro-op (mainly for human reference; the sequencer uses relative offsets).
- **Micro-ops:** Each line following a label (or on its own) defines one 49-bit micro-word.

## Micro-Word Syntax

A micro-op is defined by a space-separated list of `field=VALUE` assignments. Fields not specified default to zero (hardware defaults).

### Field Reference

| Field | Possible Values | Description |
|-------|-----------------|-------------|
| `a_src` | `REG`, `ESR`, `EPC`, `VEC`, `SPR` | A-bus source selection |
| `reg_a` | `IR_RD`, `IR_RS`, `R2`–`R15` | Register file read port A address |
| `reg_b` | `IR_RD`, `IR_RS`, `R2`–`R15` | Register file read port B address |
| `reg_w` | `IR_RD`, `IR_RS`, `R2`–`R15` | Register file write port address |
| `w_en` | `0`, `1` | Register file write enable |
| `alu` | `ADD`, `SUB`, `AND`, `OR`, `XOR`, `SHL`, `SHR`, `SAR`, `PASS_A`, `PASS_B`, `NOT`, `ADC`, `SBC`, `MUL`, `DIV`, etc. | ALU operation select |
| `bmux` | `REG`, `IMM`, `CONST4`, `CONST8` | B-mux source selection |
| `wmux` | `RBUS`, `MDR` | Write-back source selection (W-mux) |
| `imm` | `ZERO`, `SIGN`, `L16` | Immediate handling: zero/sign extend or shift-left-16 |
| `w_flags` | `0`, `1` | Update SR condition flags (NZCV) |
| `sr_load` | `0`, `1` | Load full SR from W-mux (for ERET) |
| `mar_load` | `0`, `1` | Load MAR from R-bus |
| `mdr_load` | `0`, `MEM`, `A_BUS` | Load MDR from memory or A-bus |
| `mem` | `0`, `READ`, `WRITE` | Initiate memory access |
| `size` | `BYTE`, `HALF`, `WORD` | Memory access size |
| `se` | `0`, `1` | Sign-extend sub-word memory load |
| `pc` | `HOLD`, `NEXT`, `OFFSET`, `ABUS`, `MDR` | PC source selection |
| `sys` | `NONE`, `SPR_W`, `SYS_R`, `SYS_W` | System/SPR operation |
| `start` | `0`, `1` | Start multi-cycle ALU operation |
| `branch` | `SEQ`, `FETCH`, `STALL`, `BRT`, `BRF`, `PRIV`, `SKIP` | Micro-sequencer control |
| `skip` | `0`–`7` | Forward skip offset (used with `branch=SKIP`) |

### Defaults (applied if field is omitted)

- `pc=NEXT` (for most ALU/L-format instructions) or determined by `branch`
- `branch=FETCH` (most routines are 1 micro-op)
- All other fields default to `0` / hardware default (e.g., `a_src=REG`, `alu=ADD`).

## Examples

### 1. Simple ALU Operation (ADD)
```uasm
.org 0x00
add:
  reg_a=IR_RD reg_b=IR_RS reg_w=IR_RD w_en=1 alu=ADD bmux=REG wmux=RBUS w_flags=1
```

### 2. Multi-step Load (LDW)
```uasm
.org 0x80
ldw:
  # Step 1: Compute address
  reg_a=IR_RS bmux=IMM imm=SIGN alu=ADD mar_load=1 branch=SEQ
  # Step 2: Read memory (STALL until busy deasserts)
  mem=READ size=WORD mdr_load=MEM branch=STALL
  # Step 3: Write back to register
  reg_w=IR_RD w_en=1 wmux=MDR branch=FETCH
```

### 3. Conditional Branch (BEQ)
```uasm
.org 0x60
branch:
  # Evaluates ISA condition in hardware, gates pc=OFFSET
  pc=OFFSET branch=BRT
```

## Building

The microcode is assembled as part of the main hardware build process:

```sh
# Manually assemble
python3 hw/tools/uasm.py hw/microcode/microcode.uasm -o build/microcode.hex

# Or via Makefile (recommended)
make -C hw/rom
```
