# Penumbra Microcode Reference

The single authoritative reference for the Penumbra microcode system. Covers the micro-word format, all control fields, ROM organization, sequencer behavior, and every implemented micro-routine.

**Source of truth:** If this document disagrees with RTL, the RTL is correct and this document needs updating.

---

## Overview

Penumbra uses **horizontal microcode** — each micro-word directly drives datapath control signals with no decoding. The microcode ROM holds 256 entries of 51 bits each. The micro-sequencer fetches one micro-word per clock cycle and fans out its fields to the datapath.

Most ISA instructions execute in a single micro-op (one ROM entry). Multi-step instructions (loads, stores, RDSYS, ERET, BL) use 2-4 consecutive entries.

The **fetch unit** is hardwired (not microcoded). It handles instruction fetch, IR latching, dispatch address computation, and interrupt/exception detection at dispatch time. The microcode only runs during instruction execution.

**Tools:**
- Microcode assembler: `hw/tools/uasm.py` — symbolic source → `$readmemh` hex
- Microcode source: `hw/microcode/microcode.uasm`

---

## Micro-Word Format (51 bits)

Bits are numbered 50 (MSB) to 0 (LSB).

```
 50  49    47  46    43  42    39  38    35  34  33      29  28  27  26  25  24  23  22  21  20  19  18  17  16  15  14  13    11  10   9   8   7     5   4     2   1   0
┌───┬────────┬────────┬────────┬────────┬───┬──────────┬──────┬───┬──────┬───┬───┬───┬───┬───┬───┬───┬──────┬───┬────────┬───┬───┬───┬────────┬────────┬───┬───┐
│prv│ a_src  │ reg_a  │ reg_b  │ reg_w  │wEn│ alu_op   │ bmux │wmx│imm_m │flg│srL│mar│mdr│mdr│mRd│mWr│m_size│sEx│ pc_src │sys│sWE│aSt│ branch │fwd_off │ ei│ di│
│   │ [2:0]  │ [3:0]  │ [3:0]  │ [3:0]  │   │ [4:0]    │[1:0] │   │[1:0] │   │   │   │mem│ a │   │   │[1:0] │   │ [2:0]  │cyc│   │   │ [2:0]  │ [2:0]  │set│set│
└───┴────────┴────────┴────────┴────────┴───┴──────────┴──────┴───┴──────┴───┴───┴───┴───┴───┴───┴───┴──────┴───┴────────┴───┴───┴───┴────────┴────────┴───┴───┘
```

---

## Field Reference

### Privileged Instruction — `priv` [50] (1 bit)

If set, the micro-sequencer checks the `SR.S` bit during the first micro-op of the instruction's execution. If `SR.S == 0` (user mode), a privilege violation exception (vector 4) is triggered immediately. This allows for zero-overhead privilege checking on a per-instruction basis.

### A-Bus Source — `a_src` [49:47] (3 bits)

Selects what drives the A-bus input to the ALU.

| Value | Symbol | Source |
|-------|--------|--------|
| 0 | `REG` | Register file port A (addressed by `reg_a`) |
| 1 | `ESR` | Exception Status Register (saved SR at exception entry) |
| 2 | `EPC` | Exception Program Counter (saved PC at exception entry) |
| 3 | `VECTOR` | Vector address = `{26'b0, vector_num, 2'b00}` (hardware-computed) |
| 4 | `SPR` | Special-purpose register (ESR, EPC, USP, or SR) selected by `IR[15:12]` |

Values 5–7 are reserved.

### Register Address Fields — `reg_a` [46:43], `reg_b` [42:39], `reg_w` [38:35] (4 bits each)

Address the register file's two read ports (A, B) and one write port (W).

| Value | Symbol | Register |
|-------|--------|----------|
| 0 | `IR_RD` | Format-dependent destination: R→IR[24:21], L→IR[25:22], M→IR[25:22] |
| 1 | `IR_RS` | Format-dependent source/base: R→IR[20:17], M→IR[21:18] |
| 2-15 | `R2`–`R15` | Literal register number (direct address) |

`IR_RD` and `IR_RS` are resolved by the datapath's register address routing logic based on the current instruction format. Literal addresses (2-15) bypass this routing — useful for accessing specific registers like R13 (LR), R14 (SP), R15 (PC) from microcode.

**Special registers:**
- `R15` (PC): reads as current program counter value
- `R14` (SP): reads USP or SSP depending on supervisor mode (hardware-banked)
- `R0`: reads as zero, writes are discarded

### Register Write Enable — `w_en` [34] (1 bit)

Enables the register file write port. Subject to **F-bit gating**: for Format R instructions, the actual write enable is `w_en & ~(format_R & IR[16])`. This allows CMP (SUB with F=1) and TEST (AND with F=1) to share the same micro-word as their destructive counterparts.

F-bit gating only applies when `reg_w = IR_RD`. Literal register addresses (R2-R15) are never gated.

### ALU Operation — `alu` [33:29] (5 bits)

| Value | Symbol | Operation | Cycles | Flags |
|-------|--------|-----------|--------|-------|
| 0 | `ADD` | A + B | 1 | NZCV |
| 1 | `SUB` | A − B | 1 | NZCV (C = NOT borrow, ARM-style) |
| 2 | `AND` | A & B | 1 | NZ (C,V unchanged) |
| 3 | `OR` | A \| B | 1 | NZ |
| 4 | `XOR` | A ^ B | 1 | NZ |
| 5 | `SHL` | A << B[4:0] | 1 | NZC (C = last bit shifted out) |
| 6 | `SHR` | A >> B[4:0] (logical) | 1 | NZC |
| 7 | `SAR` | A >> B[4:0] (arithmetic) | 1 | NZC |
| 8 | `PASS_A` | A (pass-through) | 1 | — |
| 9 | `PASS_B` | B (pass-through) | 1 | — |
| 10 | `NOT` | ~A | 1 | NZ |
| 11 | `ADC` | A + B + Cin | 1 | NZCV |
| 12 | `SBC` | A - B - ~Cin | 1 | NZCV |
| 13 | `MUL` | A × B (signed) | N | NZCV (stub: asserts busy forever) |
| 14 | `MULU` | A × B (unsigned) | N | NZCV (stub) |
| 15 | `DIV` | A / B (signed) | N | NZCV (stub) |
| 16 | `DIVU` | A / B (unsigned) | N | NZCV (stub) |
| 17 | `MOD` | A % B (signed) | N | NZCV (stub) |
| 18 | `MODU` | A % B (unsigned) | N | NZCV (stub) |
| 19-31 | — | Reserved for future FP ops | — | — |

Flags are only written to SR when `w_flags=1`. The flag column shows what the ALU *computes*, not what gets latched.

Multi-cycle operations require `alu_start=1` on the first micro-op and `branch=STALL` to wait for completion. Currently MUL/DIV/MOD are stubs (assert busy forever = illegal instruction trap via timeout).

### B-Bus Source — `bmux` [28:27] (2 bits)

| Value | Symbol | Source |
|-------|--------|--------|
| 0 | `REG` | Register file port B (addressed by `reg_b`) |
| 1 | `IMM` | Immediate from IR via immediate extractor (mode set by `imm_mode`) |
| 2 | `CONST4` | Hardwired constant 4 (for stack adjust, BL return address) |
| 3 | `CONST8` | Hardwired constant 8 (for stack adjust) |

### Write-Back Source — `wmux` [26] (1 bit)

Selects the source for the W-bus (register write data and SR load data).

| Value | Symbol | Source |
|-------|--------|--------|
| 0 | `RBUS` | R-bus (ALU result) |
| 1 | `MDR` | Memory Data Register (from cache/memory read or sysreg read) |

### Immediate Mode — `imm_mode` [25:24] (2 bits)

Controls how the 16-bit immediate from the IR is extended to 32 bits. Only used when `bmux=IMM`.

| Value | Symbol | Extension |
|-------|--------|-----------|
| 0 | `ZERO_EXT` | `{16'b0, imm16}` |
| 1 | `SIGN_EXT` | `{{16{imm16[15]}}, imm16}` |
| 2 | `SHIFT_L16` | `{imm16, 16'b0}` |

**Immediate source routing:** Format L uses IR[15:0], Format M uses IR[17:2] (the offset16 field). The datapath selects based on instruction format.

### Flag Write Enable — `w_flags` [23] (1 bit)

When set, latches the ALU's NZCV flag outputs into the status register. Used by arithmetic/logic instructions. Not used by MOV, loads, stores, branches, or system instructions.

### SR Load — `sr_load` [22] (1 bit)

Bulk-loads the entire status register from the W-bus. Used by ERET (restore SR from ESR). Overwrites N, Z, C, V, S, and I bits simultaneously.

### MAR Load — `mar_load` [21] (1 bit)

Loads the Memory Address Register from the R-bus (ALU output). Sets the address for subsequent memory read/write operations.

### MDR Load from Memory — `mdr_load_mem` [20] (1 bit)

Loads the Memory Data Register from the memory/cache read data bus. Used during memory reads and sysreg reads (cpu_top muxes sysreg data onto the same bus).

### MDR Load from A-Bus — `mdr_load_a` [19] (1 bit)

Loads MDR from the A-bus. Used to stage store data: the register to be stored is driven onto the A-bus, captured in MDR, then MDR drives the memory write data bus.

### Memory Read / Write — `mem_read` [18], `mem_write` [17] (1 bit each)

Initiate a cache/memory read or write at the address in MAR. Typically paired with `branch=STALL` to wait for completion. The cache/memory asserts `busy` for one or more cycles; the STALL branch holds the micro-PC until busy clears.

### Memory Size — `mem_size` [16:15] (2 bits)

| Value | Symbol | Size |
|-------|--------|------|
| 0 | `BYTE` | 8-bit |
| 1 | `HALF` | 16-bit |
| 2 | `WORD` | 32-bit |

### Sign Extend — `sign_ext` [14] (1 bit)

For sub-word loads: when set, sign-extends the loaded byte/halfword to 32 bits. When clear, zero-extends. Ignored for word loads and all stores.

### PC Source — `pc` [13:11] (3 bits)

Selects the next PC value. The selected value loads into the PC register at the clock edge when `pc_load` is asserted (which equals `executing` — always true during S_EXEC).

| Value | Symbol | Source | Use |
|-------|--------|--------|-----|
| 0 | `HOLD` | Current PC (no change) | Multi-step instructions, stalls |
| 1 | `NEXT` / `PLUS4` | PC + 4 | Sequential execution (default) |
| 2 | `OFFSET` | PC + sign_extend(offset22 << 2) | Branch target |
| 3 | `ABUS` | A-bus value | Indirect jump (JMP Rs), vector fetch |
| 4 | `MDR` | MDR value | (Reserved, not currently used) |

**Conditional override:** When `branch=BRT` and the ISA condition is false, the sequencer forces `pc` to `NEXT` regardless of the micro-word value. Same for `branch=BRF` when condition is true. This implements conditional branches with a single micro-word.

### System/SPR Operation — `sys_op` [10:9] (2 bits)

Controls the sysreg sideband bus and SPR write path.

| Value | Symbol | Meaning |
|-------|--------|---------|
| 0 | `NONE` | No system operation (default) |
| 1 | `SPR_WRITE` | Write R-bus to SPR selected by IR[15:12] |
| 2 | `SYS_READ` | Sysreg bus read cycle |
| 3 | `SYS_WRITE` | Sysreg bus write cycle |

For sysreg operations (`SYS_READ`/`SYS_WRITE`), the device ID and register index come from IR spare fields (routed by the field extractor). During a sysreg read, cpu_core muxes the sysreg data onto the memory read data bus, so `mdr_load_mem=1` captures it into MDR.

For `SPR_WRITE` (WRSPR), hardware decodes IR[15:12] to select the target: ESR (0), EPC (1), USP (2), or SR (3). The R-bus carries the write data.

### ALU Start — `alu_start` [8] (1 bit)

Starts a multi-cycle ALU operation (MUL/DIV/MOD). The ALU latches operands and begins iterating. Ignored for single-cycle operations. Pair with `branch=STALL` on the following micro-op.

### Branch Condition — `branch` [7:5] (3 bits)

Controls micro-sequencer flow. Determines whether the micro-PC advances, holds, or returns to the fetch state.

| Value | Symbol | Micro-PC Action | Notes |
|-------|--------|-----------------|-------|
| 0 | `SEQ` | micro-PC++ | Continue to next micro-op |
| 1 | `FETCH` | → S_FETCH | Instruction complete, fetch next |
| 2 | `STALL` | fault → S_FETCH; !busy → micro-PC++; else hold | Wait for memory/ALU |
| 3 | `BRT` | → S_FETCH (pc overridden if cond false) | Branch if True |
| 4 | `BRF` | → S_FETCH (pc overridden if cond true) | Branch if False |
| 5 | — | (reserved) | — |
| 6 | `SKIP` | micro-PC += 1 + fwd_offset | Forward skip |
| 7 | — | Detected as BR_ILLEGAL | Traps to vector 7 |

**STALL details:** Three-way resolution:
1. `mem_fault` asserted → abort instruction (`go_fetch`), cpu_top generates exception
2. `busy` deasserted → advance to next micro-op
3. `busy` still asserted → hold micro-PC (repeat this micro-word)

The unified busy signal is `cache_busy | alu_busy`. Memory and ALU ops never overlap in the same micro-op.

**BRT/BRF details:** Both always return to fetch (`go_fetch=1`). The difference is in `pc` override:
- BRT: if ISA condition **false**, force `pc = NEXT` (fall through)
- BRF: if ISA condition **true**, force `pc = NEXT` (fall through)

The ISA condition is evaluated by `cond_eval` from IR[29:26] (Format B condition field) against SR flags.

### Forward Skip Offset — `fwd_offset` [4:2] (3 bits)

Only used when `branch=SKIP`. The micro-PC advances by `1 + fwd_offset` (skip 1-8 micro-ops forward). Used for conditional microcode paths (not currently used by any implemented instruction).

### EI/DI Control — `ei_set` [1], `di_set` [0] (1 bit each)

| Field | Effect |
|-------|--------|
| `ei_set` | Sets SR.I=1 (enable interrupts). **Delayed** by one instruction via `ei_shadow` |
| `di_set` | Sets SR.I=0 (disable interrupts). **Immediate** effect |

The `ei_shadow` mechanism: when EI executes, a flip-flop is set that suppresses interrupt recognition until the next instruction completes. This allows atomic `EI; ERET` sequences where ERET executes in the "shadow" before any pending interrupt fires. The sequencer clears `ei_shadow` after the next instruction's `go_fetch`.

---

## Assembler Defaults

The microcode assembler (`uasm.py`) applies these defaults when a field is not specified:

| Field | Default | Meaning |
|-------|---------|---------|
| `pc` | `NEXT` (1) | Advance PC by 4 |
| `branch` | `FETCH` (1) | Return to fetch state |
| All others | 0 | No operation / hold / register 0 / ALU ADD |

These defaults mean a single-field-assignment line like `ei_set=1` is a complete micro-op: it sets EI, advances PC, and returns to fetch. Multi-step instructions must explicitly override both `pc=HOLD` and `branch=SEQ` on all steps except the last.

**Common mistake:** Forgetting `branch=SEQ` on intermediate steps. Without it, the default `branch=FETCH` returns to fetch immediately, and subsequent micro-ops never execute.

### Assembler Syntax

```
# Comments start with #
.org 0x00           # Set ROM address

label:              # Label (documentation, not referenced by micro-ops)
  field=VALUE field=VALUE ...   # One micro-op per line
  field=VALUE ...               # Next micro-op at ROM address + 1
```

Fields are `key=value` pairs separated by spaces. Values are symbolic names (uppercase) or integer literals (decimal, `0x` hex, `0b` binary).

---

## ROM Organization

### Address Space

256 entries (8-bit micro-PC), divided into zones by instruction format:

| Zone | Addresses | Slot Size | Count | Purpose |
|------|-----------|-----------|-------|---------|
| R-ALU | 0x00–0x1F | ×2 | 16 | Format R ALU ops (op[4]=0) |
| Format L | 0x20–0x3F | ×2 | 16 | Immediate operations (13 used, 3 reserved) |
| R-SYS | 0x40–0x5F | ×2 | 16 | Format R system ops (op[4]=1; ops 23–31 used) |
| Format B | 0x60–0x63 | ×2 | 2 | Conditional branch (0x60), BL (0x62) |
| (gap) | 0x64–0x6F | — | — | Unused |
| Exception | 0x70 | ×4 | 1 | `int_entry` (3 micro-ops) |
| (gap) | 0x74–0x7F | — | — | Unused |
| Format M | 0x80–0xBF | ×4 | 16 | Load/store operations |
| (unused) | 0xC0–0xFF | — | — | Available for future expansion |

### Dispatch Address Computation

Computed by the fetch unit from `mem_rdata` (instruction bits, same cycle as IR latch):

| Format | Formula | Range |
|--------|---------|-------|
| R (prefix 00) | `{0, op[4], 0, op[3:0], 0}` | 0x00–0x1E (ALU), 0x40–0x5E (SYS) |
| L (prefix 01) | `{01, op[3:0], 0}` | 0x20–0x3E |
| M (prefix 10) | `{10, L, sz[1:0], SE, 00}` | 0x80–0xBC |
| B (prefix 11) | `cond==1111 ? 0x62 : 0x60` | 0x60 or 0x62 |
| Exception | Hardwired | 0x70 |

Format R uses ×2 spacing split by `op[4]`: ALU zone (`op[4]=0`, 0x00–0x1E) and SYS zone (`op[4]=1`, 0x40–0x5E). The formula `{0, op[4], 0, op[3:0], 0}` is pure wiring — zero gates.

Format B dispatches all conditional/unconditional branches to 0x60 and BL (cond=1111) to 0x62. The BL detection is a 4-input AND gate on `mem_rdata[29:26]`.

### Slot Boundaries

Multi-step micro-routines must not cross slot boundaries. The microcode assembler validates this automatically. If a routine at 0x42 (R-SYS, slot size 2) has 3 micro-ops, assembly fails with an overflow error.

### Dispatch-Time Interception

Some instructions are intercepted at dispatch time in cpu_top and never reach their ROM entry:

| Instruction | Dispatch Addr | Interception | Vector |
|-------------|--------------|--------------|--------|
| SYSCALL | 0x52 | `dispatch_addr == 0x52` | VEC_SYSCALL (5) |
| BREAK | 0x54 | `dispatch_addr == 0x54` | VEC_BREAK (6) |
| Privileged in user mode | 0x40–0x5E | SYS zone & !exempt & !SR.S | VEC_PRIV (4) |

BREAK and privilege violations trigger `except_entry` (saves EPC/ESR, sets S=1/I=0) and redirect dispatch to 0x70 (int_entry).

External IRQs are also checked at dispatch time: `irq_taken = i_irq & sr_i & !ei_shadow`.

MMU faults are detected during STALL (not at dispatch). The sequencer aborts the instruction, and `fault_pending` overrides the next dispatch to 0x70.

---

## Exception Integration

### except_entry Pulse

When any exception fires, cpu_top asserts `except_entry` for one cycle. This triggers hardware pre-actions in the datapath:

1. **EPC ← PC** — saves the current program counter
2. **ESR ← SR** — saves the current status register
3. **SR.S ← 1, SR.I ← 0** — enter supervisor mode, disable interrupts
4. **SP bank swap** — R14 now accesses SSP

These happen atomically in hardware, before the int_entry micro-op executes.

### int_entry Micro-Op (0x70)

After hardware pre-actions, the sequencer dispatches to 0x70:

```
a_src=VECTOR pc=ABUS
```

This loads the vector table entry address into PC. The vector address is `{26'b0, vector_num, 2'b00}` — word-aligned entries at physical address 0x00.

### Vector Fetch Bypass

After int_entry loads the vector address into PC, the next instruction fetch **bypasses the MMU**. This ensures the vector table is always reachable without a TLB mapping — eliminating nested TLB miss on exception entry. The `vector_fetch` flag is set when int_entry completes and cleared when `ir_valid` fires.

### Vector Numbers

| Vector | Address | Source |
|--------|---------|--------|
| 0 | 0x00 | Bus fault (no device at address) |
| 1 | 0x04 | Timer interrupt |
| 2 | 0x08 | TLB miss |
| 3 | 0x0C | TLB protection fault |
| 4 | 0x10 | Privilege violation |
| 5 | 0x14 | SYSCALL |
| 6 | 0x18 | BREAK |
| 7 | 0x1C | Illegal instruction |
| 8 | 0x20 | Alignment fault |

### Priority

`fault_pending > illegal_pending > priv_pending > BREAK > SYSCALL > IRQ`

Fault sets SR.I=0, so `irq_taken` is false at the next dispatch — faults always take priority.

---

## Complete Micro-Routine Catalog

### Notation

- Fields not listed use assembler defaults (`pc=NEXT branch=FETCH` on last step, all others 0)
- Multi-step routines show step numbers; step 0 is at the dispatch address
- `→` indicates the datapath flow for that step

### Format R — ALU Operations (0x00–0x1E)

All single-cycle, single micro-op. Pattern: read Rd and Rs, compute, write result to Rd, update flags.

**ADD** (op=0, dispatch=0x00)
```
reg_a=IR_RD reg_b=IR_RS reg_w=IR_RD w_en=1 alu=ADD bmux=REG wmux=RBUS w_flags=1
```
→ Rd = Rd + Rs, flags updated. CMP (F=1) variant: flags only, Rd unchanged.

**SUB** (op=1, dispatch=0x02) — same pattern, `alu=SUB`. CMP = SUB with F=1.

**AND** (op=2, dispatch=0x04) — same pattern, `alu=AND`. TEST = AND with F=1.

**OR** (op=3, dispatch=0x06) — same pattern, `alu=OR`.

**XOR** (op=4, dispatch=0x08) — same pattern, `alu=XOR`.

**SHL** (op=5, dispatch=0x0A) — same pattern, `alu=SHL`.

**SHR** (op=6, dispatch=0x0C) — same pattern, `alu=SHR`.

**SAR** (op=7, dispatch=0x0E) — same pattern, `alu=SAR`.

**MOV** (op=8, dispatch=0x10)
```
reg_a=IR_RS reg_w=IR_RD w_en=1 alu=PASS_A wmux=RBUS
```
→ Rd = Rs. Does **not** update flags.

**NOT** (op=9, dispatch=0x12)
```
reg_a=IR_RS reg_w=IR_RD w_en=1 alu=NOT wmux=RBUS w_flags=1
```
→ Rd = ~Rs, flags updated.

### Format L — Immediate Operations (0x20–0x38)

All single-cycle, single micro-op except JALR (2 micro-ops).

**LLI** (op=0, dispatch=0x20)
```
reg_w=IR_RD w_en=1 alu=PASS_B bmux=IMM wmux=RBUS imm_mode=ZERO_EXT
```
→ Rd = zero_extend(imm16).

**LLIS** (op=1, dispatch=0x22)
```
reg_w=IR_RD w_en=1 alu=PASS_B bmux=IMM wmux=RBUS imm_mode=SIGN_EXT
```
→ Rd = sign_extend(imm16).

**LUI** (op=2, dispatch=0x24)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=OR bmux=IMM wmux=RBUS imm_mode=SHIFT_L16
```
→ Rd = Rd | (imm16 << 16). Typically preceded by LLI to build a 32-bit constant.

**ADDi (alias `INC`)** (op=3, dispatch=0x26)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=ADD bmux=IMM wmux=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd + zero_extend(imm16), flags updated. Assembler accepts `ADD Rd, #imm`.

**SUBi (alias `DEC`)** (op=4, dispatch=0x28)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=SUB bmux=IMM wmux=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd − zero_extend(imm16), flags updated. Assembler accepts `SUB Rd, #imm`.

**CMPi** (op=5, dispatch=0x2A)
```
reg_a=IR_RD alu=SUB bmux=IMM wmux=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Flags = Rd − zero_extend(imm16). Rd **not** written (no `w_en`).

**ANDi** (op=6, dispatch=0x2C)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=AND bmux=IMM wmux=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd & zero_extend(imm16), flags updated.

**TESTi** (op=7, dispatch=0x2E)
```
reg_a=IR_RD alu=AND bmux=IMM wmux=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Flags = Rd & zero_extend(imm16). Rd **not** written (no `w_en`).

**SHLi** (op=8, dispatch=0x30)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=SHL bmux=IMM wmux=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd << imm[4:0], flags updated.

**SHRi** (op=9, dispatch=0x32)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=SHR bmux=IMM wmux=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd >> imm[4:0] (logical), flags updated.

**SARi** (op=10, dispatch=0x34)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=SAR bmux=IMM wmux=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd >> imm[4:0] (arithmetic), flags updated.

**JMP Rd** (op=11, dispatch=0x36)
```
reg_a=IR_RD pc=ABUS
```
→ PC = Rd field. (Format L uses the Rd slot, not Rs.) Assembler alias: `RET` = `JMP R13`.

**JALR Rd** (op=12, dispatch=0x38) — 2 micro-ops
```
Step 0: reg_a=R15 bmux=CONST4 alu=ADD reg_w=R13 w_en=1 wmux=RBUS pc=HOLD branch=SEQ
Step 1: reg_a=IR_RD pc=ABUS
```
→ R13 = PC + 4 (link), then PC = Rd field. Used for indirect calls (function pointers, vtables).

### Format R — System Operations (op 23–31, 0x4E–0x5E)

Format R SYS ops sit at the high half of the R-zone (dispatch
`{0, 1, op[3:0], 0}` per `cpu_core.sv:326`). Opcodes 18–22 (0x44–0x4C)
are reserved and trap as illegal.

**WRSYS Rd, #dev, #reg** (op=23, dispatch=0x4E) — Privileged
```
priv=1 reg_a=IR_RD sys_op=SYS_WRITE
```
→ A-bus = Rd → sysreg write bus. Device/register from IR spare fields.

**RDSYS Rd, #dev, #reg** (op=24, dispatch=0x50) — 2 micro-ops, Privileged
```
Step 0: priv=1 sys_op=SYS_READ mdr_load_mem=1 pc=HOLD branch=SEQ
Step 1: reg_w=IR_RD w_en=1 wmux=MDR
```
→ Step 0: sysreg data → mem_rdata bus → MDR. Step 1: MDR → Rd.

**SYSCALL** (op=25, dispatch=0x52) — Intercepted at dispatch
Never reaches ROM. Detected by `dispatch_addr == 0x52`. Triggers `except_entry` → VEC_SYSCALL (5) → `int_entry` at 0x70. Unprivileged. EPC points at the SYSCALL; handler is responsible for advancing EPC+4 before `ERET`.

**BREAK** (op=26, dispatch=0x54) — Intercepted at dispatch
Never reaches ROM. Detected by `dispatch_addr == 0x54`. Triggers `except_entry` → VEC_BREAK (6) → `int_entry` at 0x70. Unprivileged. `o_halted` pulses for testbench halt.

**ERET** (op=27, dispatch=0x56) — 2 micro-ops, Privileged
```
Step 0: priv=1 a_src=ESR alu=PASS_A sr_load=1 pc=HOLD branch=SEQ
Step 1: a_src=EPC pc=ABUS
```
→ Step 0: ESR → A-bus → ALU → R-bus → W-bus → SR (restores flags, S, I bits, may trigger SP bank swap). Step 1: EPC → A-bus → PC (resume at saved address). Single-form only — there is no `ERET Rd, Rs` variant; context switches use `WRSPR EPC/ESR; ERET`.

**EI** (op=28, dispatch=0x58) — Privileged
```
priv=1 ei_set=1
```
→ SR.I = 1, with one-instruction delay (ei_shadow). User-mode `EI` traps to `VEC_PRIV`.

**DI** (op=29, dispatch=0x5A) — Privileged
```
priv=1 di_set=1
```
→ SR.I = 0, immediate effect.

**WRSPR {ESR|EPC|USP|SCR0–3}, Rd** (op=30, dispatch=0x5C) — Privileged
```
priv=1 reg_a=IR_RD alu=PASS_A sys_op=SPR_WRITE
```
→ R-bus = Rd value → SPR write target. Hardware decodes IR[15:12]: SPR 0 (ESR) → `esr_load`, SPR 1 (EPC) → `epc_load`, SPR 2 (USP) → R14 cross-bank write, SPR 4–7 (SCR0–SCR3) → respective scratch storage. SPR 3 (SR) is not writable via WRSPR (use `ERET` or `EI`/`DI`).

**RDSPR Rd, {ESR|EPC|USP|SR|SCR0–3}** (op=31, dispatch=0x5E) — Privileged
```
priv=1 a_src=SPR alu=PASS_A reg_w=IR_RD w_en=1 wmux=RBUS
```
→ Rd = SPR value. Hardware decodes IR[15:12]: SPR 0 → A-bus=ESR, SPR 1 → A-bus=EPC, SPR 2 → A-bus=R14 cross-bank read, SPR 3 → A-bus=SR (current status), SPR 4–7 → SCR0–SCR3 scratch storage.

### Format B — Branches (0x60, 0x62)

**Bcc** (conditional/unconditional, dispatch=0x60)
```
pc=OFFSET branch=BRT
```
→ If ISA condition true: PC = PC + offset. If false: PC = PC + 4 (fall through). All 15 conditions (AL, EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE) use this single entry.

**BL** (branch-and-link, cond=1111, dispatch=0x62) — 2 micro-ops
```
Step 0: reg_a=R15 bmux=CONST4 alu=ADD reg_w=R13 w_en=1 wmux=RBUS pc=HOLD branch=SEQ
Step 1: pc=OFFSET
```
→ Step 0: R13 (LR) = R15 (PC) + 4 = return address. Step 1: PC = PC + offset (always taken). Return via `RET` (JMP R13).

### Exception Entry (0x70)

**int_entry** (hardwired dispatch after except_entry pulse)
```
a_src=VECTOR pc=ABUS
```
→ PC = vector_addr. Shared by all exception sources (IRQ, TLB miss, TLB prot, privilege violation, BREAK). Hardware pre-actions (EPC/ESR save, mode switch) have already executed before this micro-op runs.

### Format M — Memory Operations (0x80–0xBF)

**STW Rd, [Rb + #offset]** (L=0, sz=10, SE=0, dispatch=0x90) — 4 micro-ops
```
Step 0: reg_a=IR_RS bmux=IMM imm_mode=SIGN_EXT alu=ADD mar_load=1 pc=HOLD branch=SEQ
Step 1: reg_a=IR_RD mdr_load_a=1 pc=HOLD branch=SEQ
Step 2: mem_write=1 mem_size=WORD pc=HOLD branch=STALL
Step 3: pc=NEXT
```
→ Step 0: MAR = Rb + sign_extend(offset). Step 1: MDR = Rd (via A-bus). Step 2: write MDR to [MAR], stall until accepted. Step 3: advance PC, fetch next.

**LDW Rd, [Rb + #offset]** (L=1, sz=10, SE=0, dispatch=0xB0) — 3 micro-ops
```
Step 0: reg_a=IR_RS bmux=IMM imm_mode=SIGN_EXT alu=ADD mar_load=1 pc=HOLD branch=SEQ
Step 1: mem_read=1 mem_size=WORD mdr_load_mem=1 pc=HOLD branch=STALL
Step 2: reg_w=IR_RD w_en=1 wmux=MDR
```
→ Step 0: MAR = Rb + sign_extend(offset). Step 1: read [MAR] into MDR, stall until ready. Step 2: Rd = MDR, advance PC.

---

## Adding a New Instruction

Step-by-step guide using BL as the example:

### 1. Identify the dispatch address

Check the instruction format and encoding. BL is Format B with cond=1111. Regular branches dispatch to 0x60; BL needs its own entry. The Format B zone was expanded to 0x60–0x63 (two ×2 slots). BL dispatches to 0x62.

If the new instruction fits in an existing zone with unused slots, just use the slot. If not, modify the dispatch logic in `cpu_top.sv`.

### 2. Write the microcode

Add the micro-routine to `sw/microcode/microcode.uasm`:

```
.org 0x62
bl:
  reg_a=R15 bmux=CONST4 alu=ADD reg_w=R13 w_en=1 wmux=RBUS pc=HOLD branch=SEQ
  pc=OFFSET
```

Key rules:
- Intermediate steps need `pc=HOLD branch=SEQ`
- Last step uses defaults (`pc=NEXT branch=FETCH`) or explicit branch target
- Run `uasm.py` to verify slot boundaries

### 3. Update the dispatch (if needed)

In `cpu_top.sv`, modify the dispatch address computation:

```verilog
2'b11: dispatch_addr = (mem_rdata[29:26] == 4'b1111) ? 8'h62 : 8'h60;
```

### 4. Update the assembler (if needed)

The ISA assembler (`pasm.py`) already had BL in `BRANCH_OPS` with cond=15. If adding a wholly new instruction, add it to the appropriate table (`FORMAT_R_OPS`, `FORMAT_L_OPS`, etc.).

### 5. Update slot zones (if needed)

In `uasm.py`, update `SLOT_ZONES` if the zone boundaries changed:

```python
(0x60, 0x64, 2, "Format B"),  # was (0x60, 0x61, 1, ...)
```

### 6. Write a test

Add `sim/programs/test_<name>.s` following the pass/fail convention:
- R1 = 1 means PASS, R1 = 0 means FAIL
- End with `BREAK`
- Self-checking: test sets R1 and branches to `fail:` on assertion failure

### 7. Run the full test suite

```
make test
```

Verify no regressions.

---

## Design History

This document replaces `doc/internals/microcode-validation.md`, which was a design exploration document used during initial architecture development. That document tracked the evolution from a 55-bit micro-word to the final 51-bit format, identified 17 design issues, and validated the format through hand-written bit-level micro-programs for representative instructions.

Key design changes discovered during validation:
- Micro-word finalized at 51 bits
- `priv` bit added for hardware privilege checking
- `a_src` expanded to 3 bits to support `SPR` access
- Instruction fetch moved from a 4-micro-op ROM routine to a hardwired fetch unit
- Privilege checking moved from microcode (`BR_PRIV`) to hardware `priv` bit
- Exception entry reduced from a 7-micro-op stack-push sequence to a 1-micro-op vector jump, with hardware pre-actions handling EPC/ESR save and mode switch
- Interrupt entry uses the same unified path as MMU faults, BREAK, and privilege violations
- `ei_set`/`di_set` occupy bits [1:0] (originally listed as spare in the validation doc)
