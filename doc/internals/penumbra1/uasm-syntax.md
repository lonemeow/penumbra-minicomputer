# Microcode Assembler (uasm.py) Syntax

> **Applies to:** Penumbra/1 · microcoded core.

The Penumbra microcode is written in a symbolic assembly format and translated into a hexadecimal memory file for the FPGA/Verilator ROM using `hw/tools/uasm.py`.

> **Warning — this document is stale and pending a rewrite.** The field
> names and symbolic value names listed below are from an earlier
> proposed syntax and do **not** match what `hw/tools/uasm.py` actually
> parses. Notable mismatches: `imm` → `imm_mode` (with values
> `ZERO_EXT`/`SIGN_EXT`/`SHIFT_L16`, not `ZERO`/`SIGN`/`L16`); `mdr_load`
> → `mdr_load_mem` + `mdr_load_a` (two separate 1-bit fields);
> `mem` → `mem_read` + `mem_write`; `size` → `mem_size`;
> `se` → `sign_ext`; `sys` → `sys_op` (with values
> `NONE`/`SPR_WRITE`/`SYS_READ`/`SYS_WRITE`); `start` → `alu_start`;
> `skip` → `fwd_offset`; `flags` → `w_flags`; `a_src=VEC` →
> `a_src=VECTOR`. The privileged-marker bit (`priv`) and the
> `ei_set`/`di_set` bits are also missing from the field list below.
> **Authoritative source:** `hw/tools/uasm.py` lines 37–108 (the
> `FIELDS` table). Use that when writing or reading microcode until
> this document is rewritten.

## File Structure

- **Comments:** Lines starting with `#` are comments.
- **Directives:** `.org <address>` sets the dispatch address for the following micro-routine.
- **Labels:** `label:` defines a symbolic name for a micro-op (mainly for human reference; the sequencer uses relative offsets).
- **Micro-ops:** Each line following a label (or on its own) defines one 51-bit micro-word.

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
