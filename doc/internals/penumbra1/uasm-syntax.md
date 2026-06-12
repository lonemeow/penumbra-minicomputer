# Microcode Assembler (uasm.py) Syntax

> **Applies to:** Penumbra/1 · microcoded core.

The Penumbra microcode is written in a symbolic assembly format and
translated into a `$readmemh` hex file for the 256-entry × 52-bit
microcode ROM by `hw/tools/uasm.py`. The single microcode source is
`hw/microcode/microcode.uasm`.

Field semantics (what each bit does in the datapath) are documented in
the [microcode reference](microcode.md); this document covers the
source format and the assembler's behavior. `python3 hw/tools/uasm.py
--dump` prints the field table (bit positions, widths, symbolic
values) straight from the assembler — always current.

## File Structure

- **Comments:** everything after `#` is ignored.
- **Directives:** `.org <address>` sets the ROM address (hex `0x..` or
  decimal) for the following micro-ops.
- **Labels:** `label:` names the current ROM address. Labels are
  documentation (they appear as comments in the generated hex);
  micro-ops never reference them — sequencing is purely relative.
- **Micro-ops:** each non-blank line of `field=VALUE` pairs assembles
  one 52-bit micro-word. Consecutive lines occupy consecutive ROM
  addresses until the next `.org` or label.

## Micro-Word Syntax

A micro-op is a space-separated list of `field=VALUE` assignments.
Values are symbolic names (case-insensitive) or integer literals
(decimal, `0x` hex, `0b` binary). Fields not specified take the
defaults below.

### Field Reference

| Field | Bits | Values | Description |
|-------|------|--------|-------------|
| `priv` | [51] | `0`, `1` | Privileged micro-op (sequencer traps `VEC_PRIV` in user mode) |
| `a_src` | [50:48] | `REG`, `ESR`, `EPC`, `VECTOR`, `SPR` | A-bus source selection |
| `reg_a` | [47:44] | `IR_RD`, `IR_RS`, `IR_RDH`, `R3`–`R15` | Register file read port A address |
| `reg_b` | [43:40] | `IR_RD`, `IR_RS`, `IR_RDH`, `R3`–`R15` | Register file read port B address |
| `reg_w` | [39:36] | `IR_RD`, `IR_RS`, `IR_RDH`, `R3`–`R15` | Register file write port address |
| `w_en` | [35] | `0`, `1` | Register file write enable |
| `alu` | [34:30] | `ADD`, `SUB`, `AND`, `OR`, `XOR`, `SHL`, `SHR`, `SAR`, `PASS_A`, `PASS_B`, `NOT`, `ADC`, `SBC`, `MUL`, `MULU`, `DIV`, `DIVU` | ALU / divmul operation select |
| `bmux` | [29:28] | `REG`, `IMM`, `CONST4`, `CONST8` | B-mux source selection |
| `wb_src` | [27:26] | `RBUS`, `MDR`, `DML_LO`, `DML_HI` | Write-back source (W-mux): ALU result, load data, divmul low/high half |
| `imm_mode` | [25:24] | `ZERO_EXT`, `SIGN_EXT`, `SHIFT_L16` | Immediate extension mode |
| `w_flags` | [23] | `0`, `1` | Latch NZCV condition flags |
| `sr_load` | [22] | `0`, `1` | Bulk-load SR from W-bus (ERET) |
| `mar_load` | [21] | `0`, `1` | Load MAR from R-bus |
| `mdr_load_mem` | [20] | `0`, `1` | Load MDR from memory read data |
| `mdr_load_a` | [19] | `0`, `1` | Load MDR from A-bus (store data staging) |
| `mem_read` | [18] | `0`, `1` | Initiate memory read |
| `mem_write` | [17] | `0`, `1` | Initiate memory write |
| `mem_size` | [16:15] | `BYTE`, `HALF`, `WORD` | Memory access size |
| `sign_ext` | [14] | `0`, `1` | Sign-extend sub-word load |
| `pc` | [13:11] | `HOLD`, `NEXT` (alias `PLUS4`), `OFFSET`, `ABUS`, `MDR` | PC source selection |
| `sys_op` | [10:9] | `NONE`, `SPR_WRITE`, `SYS_READ`, `SYS_WRITE` | System/SPR operation |
| `divmul_start` | [8] | `0`, `1` | Pulse the divmul peer unit's start (operation from `alu`) |
| `branch` | [7:5] | `SEQ`, `FETCH`, `STALL`, `BRT`, `BRF`, `SKIP` | Micro-sequencer control |
| `fwd_offset` | [4:2] | `0`–`7` | Forward skip distance (used with `branch=SKIP`; micro-PC += 1 + fwd_offset) |
| `ei_set` | [1] | `0`, `1` | SR.I ← 1, delayed one instruction via ei_shadow |
| `di_set` | [0] | `0`, `1` | SR.I ← 0, immediate |

Register-address notes: encodings 0–2 are IR-indirect (`IR_RD` and
`IR_RS` resolve per instruction format; `IR_RDH` is the Rdh field,
IR[15:12]); literal registers are `R3`–`R15` only — R2 has no literal
encoding.

Branch-value note: encoding 7 is the **illegal sentinel**. It is not a
symbolic value; the assembler fills every unused ROM entry with a word
whose only set field is `branch=7`, and the sequencer traps it to
`VEC_ILLEGAL`.

### Defaults (applied if a field is omitted)

- `pc=NEXT` — advance PC by 4
- `branch=FETCH` — instruction complete, return to fetch
- `mem_size=WORD`
- All other fields `0` (e.g. `a_src=REG`, `alu=ADD`, `wb_src=RBUS`,
  all enables off)

The defaults make a one-line micro-op a complete instruction.
Multi-step routines must override `pc=HOLD branch=SEQ` (or
`branch=STALL`) on every step except the last.

## Examples

(From `hw/microcode/microcode.uasm`.)

### 1. Simple ALU Operation (ADD)
```uasm
.org 0x00
add:
  reg_a=IR_RD reg_b=IR_RS reg_w=IR_RD w_en=1 alu=ADD bmux=REG wb_src=RBUS w_flags=1
```

### 2. Multi-step Load (LDW)
```uasm
.org 0xB0
ldw:
  # Step 0: compute address into MAR
  reg_a=IR_RS bmux=IMM imm_mode=SIGN_EXT alu=ADD mar_load=1 pc=HOLD branch=SEQ
  # Step 1: read memory (STALL until busy deasserts)
  mem_read=1 mem_size=WORD mdr_load_mem=1 pc=HOLD branch=STALL
  # Step 2: write back to register (defaults: pc=NEXT branch=FETCH)
  reg_w=IR_RD w_en=1 wb_src=MDR
```

### 3. Conditional Branch (Bcc)
```uasm
.org 0x60
branch:
  # Hardware evaluates the ISA condition; BRT gates pc=OFFSET
  pc=OFFSET branch=BRT
```

## Slot Zones

ROM addresses are dispatch targets computed from instruction bits, so
every routine lives in a fixed slot (`SLOT_ZONES` in `uasm.py`). A
multi-step routine must not run past the end of its slot — the next
slot is another instruction's dispatch target. Zones with slot size 1
have no internal boundaries: a routine there may use consecutive
entries up to the end of the zone.

| Zone | Addresses | Slot size | Contents |
|------|-----------|-----------|----------|
| R-ALU | 0x00–0x1F | 2 | Format R ALU ops (op[4]=0) |
| Format L | 0x20–0x3F | 2 | Immediate ops, JMP, JALR |
| R-SYS | 0x40–0x5F | 2 | divmul dispatch + SYS ops (op[4]=1) |
| Format B | 0x60–0x63 | 2 | Bcc (0x60), BL (0x62) |
| Exception | 0x70–0x7F | whole zone | `int_entry` (3 micro-ops) |
| Format M | 0x80–0xBF | 4 | Loads/stores |

Addresses outside any zone (0x64–0x6F, 0xC0–0xFF) are unassigned; the
assembler performs no slot checking there.

The assembler validates slot boundaries at assembly time. A routine
that overflows its slot fails the build with:

```
Error line N: micro-op at 0xXX overflows <zone> slot 0xYY–0xZZ (routine '<label>')
```

If a routine legitimately needs more micro-ops than its slot holds,
use `branch=SKIP` with `fwd_offset` to jump into free entries (the
MUL/MULU/DIV/DIVU routines share a writeback tail at 0x49 this way),
or rearrange the zone — see
[Adding a New Instruction](microcode.md#adding-a-new-instruction).

## Building

The root Makefile assembles the microcode as part of every simulation
and test target (`make sim`, `make test`, `make simulate-rtl`, …):

```sh
python3 hw/tools/uasm.py hw/microcode/microcode.uasm -o microcode.hex
```

The output lands at the repository root as `microcode.hex`, where the
RTL's `$readmemh` (in `hw/rtl/penumbra1/ucode_rom.sv`) expects it. The
same command works standalone for a quick syntax/slot check. Note that
`make -C hw/rom` builds only the boot ROM — it does **not** assemble
microcode.
