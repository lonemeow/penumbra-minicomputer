# Penumbra Microcode Reference

> **Applies to:** Penumbra/1 · microcoded core.

The single authoritative reference for the Penumbra microcode system.
Covers the micro-word format, all control fields, ROM organization,
sequencer behavior, and every implemented micro-routine.

**Source of truth:** If this document disagrees with RTL, the RTL is
correct and this document needs updating.

---

## Overview

Penumbra uses **horizontal microcode** — each micro-word directly
drives datapath control signals with no decoding. The microcode ROM
holds 256 entries of 52 bits each. The micro-sequencer fetches one
micro-word per clock cycle and fans out its fields to the datapath.

Most ISA instructions execute in a single micro-op (one ROM entry).
Multi-step instructions (loads, stores, RDSYS, ERET, BL, JALR,
MUL/DIV) use 2–4 consecutive entries.

The **fetch unit** is hardwired (not microcoded). It handles
instruction fetch, IR latching, dispatch address computation, and
interrupt/exception detection at dispatch time. The microcode only
runs during instruction execution.

**Tools:**
- Microcode assembler: `hw/tools/uasm.py` — symbolic source →
  `$readmemh` hex. Source-file syntax:
  [`uasm-syntax.md`](uasm-syntax.md). `python3 hw/tools/uasm.py
  --dump` prints the field table below straight from the assembler.
- Microcode source: `hw/microcode/microcode.uasm`

---

## Micro-Word Format (52 bits)

Bits are numbered 51 (MSB) to 0 (LSB). The sequencer
(`hw/rtl/penumbra1/sequencer.sv`) extracts these fields verbatim.

| Bits | Field | Width | Summary |
|------|-------|-------|---------|
| [51] | `priv` | 1 | Privileged-instruction marker |
| [50:48] | `a_src` | 3 | A-bus source select |
| [47:44] | `reg_a` | 4 | Register file read port A address |
| [43:40] | `reg_b` | 4 | Register file read port B address |
| [39:36] | `reg_w` | 4 | Register file write port address |
| [35] | `w_en` | 1 | Register write enable |
| [34:30] | `alu` | 5 | ALU / divmul operation select |
| [29:28] | `bmux` | 2 | B-bus source select |
| [27:26] | `wb_src` | 2 | Write-back (W-bus) source select |
| [25:24] | `imm_mode` | 2 | Immediate extension mode |
| [23] | `w_flags` | 1 | Latch NZCV flags |
| [22] | `sr_load` | 1 | Bulk-load SR from W-bus |
| [21] | `mar_load` | 1 | Load MAR from R-bus |
| [20] | `mdr_load_mem` | 1 | Load MDR from memory read data |
| [19] | `mdr_load_a` | 1 | Load MDR from A-bus |
| [18] | `mem_read` | 1 | Initiate memory read |
| [17] | `mem_write` | 1 | Initiate memory write |
| [16:15] | `mem_size` | 2 | Memory access size |
| [14] | `sign_ext` | 1 | Sign-extend sub-word load |
| [13:11] | `pc` | 3 | PC source select |
| [10:9] | `sys_op` | 2 | System/SPR operation |
| [8] | `divmul_start` | 1 | Start divmul operation (pulse) |
| [7:5] | `branch` | 3 | Micro-sequencer control |
| [4:2] | `fwd_offset` | 3 | Forward skip distance (with `branch=SKIP`) |
| [1] | `ei_set` | 1 | SR.I ← 1, delayed via `ei_shadow` |
| [0] | `di_set` | 1 | SR.I ← 0, immediate |

---

## Field Reference

### Privileged Instruction — `priv` [51] (1 bit)

Marks a privileged micro-op. The sequencer evaluates the bit as the
micro-op executes (`priv & !SR.S`); privileged routines set it on
their first micro-op, so the check fires before the instruction has
any side effects. On violation the sequencer suppresses every enable
(register writes, memory access, PC load), aborts to fetch, and
cpu_core latches `priv_pending` — the next dispatch is redirected to
`int_entry` with a privilege violation exception (vector 4,
`VEC_PRIV`). This gives zero-overhead privilege checking on a
per-instruction basis: in discrete logic it is one AND gate per
enable line.

### A-Bus Source — `a_src` [50:48] (3 bits)

Selects what drives the A-bus input to the ALU.

| Value | Symbol | Source |
|-------|--------|--------|
| 0 | `REG` | Register file port A (addressed by `reg_a`) |
| 1 | `ESR` | Exception Status Register (saved SR at exception entry) |
| 2 | `EPC` | Exception Program Counter (saved PC at exception entry) |
| 3 | `VECTOR` | Vector address = `{26'b0, vector_num, 2'b00}` (hardware-computed) |
| 4 | `SPR` | Special-purpose register selected by `IR[15:12]` (ESR, EPC, USP, SR, SCR0–SCR3) |

Values 5–7 are reserved.

### Register Address Fields — `reg_a` [47:44], `reg_b` [43:40], `reg_w` [39:36] (4 bits each)

Address the register file's two read ports (A, B) and one write port
(W).

| Value | Symbol | Register |
|-------|--------|----------|
| 0 | `IR_RD` | Format-dependent destination: R→IR[24:21], L→IR[25:22], M→IR[25:22] |
| 1 | `IR_RS` | Format-dependent source/base: R→IR[20:17], M→IR[21:18] |
| 2 | `IR_RDH` | The Rdh field (IR[15:12]) — MUL/DIV third operand |
| 3–15 | `R3`–`R15` | Literal register number (direct address) |

`IR_RD`, `IR_RS`, and `IR_RDH` are resolved by the datapath's register
address routing logic based on the current instruction format. Literal
addresses (3–15) bypass this routing — useful for accessing specific
registers like R13 (LR), R14 (SP), R15 (PC) from microcode. R2 is not
literal-addressable from microcode (encoding 2 means `IR_RDH`); no
implemented routine needs a literal R2.

**Special registers:**
- `R15` (PC): reads as current program counter value
- `R14` (SP): reads USP or SSP depending on supervisor mode
  (hardware-banked)
- `R0`: reads as zero, writes are discarded

### Register Write Enable — `w_en` [35] (1 bit)

Enables the register file write port. Subject to **F-bit gating**: for
Format R instructions, the actual write enable is `w_en & ~(format_R &
IR[16])`. This allows CMP (SUB with F=1) and TEST (AND with F=1) to
share the same micro-word as their destructive counterparts.

F-bit gating only applies when `reg_w = IR_RD`. Literal register
addresses are never gated.

### ALU Operation — `alu` [34:30] (5 bits)

| Value | Symbol | Operation | Cycles | Flags computed |
|-------|--------|-----------|--------|----------------|
| 0 | `ADD` | A + B | 1 | NZCV |
| 1 | `SUB` | A − B | 1 | NZCV (C = NOT borrow, ARM-style) |
| 2 | `AND` | A & B | 1 | NZ; C,V = 0 |
| 3 | `OR` | A \| B | 1 | NZ; C,V = 0 |
| 4 | `XOR` | A ^ B | 1 | NZ; C,V = 0 |
| 5 | `SHL` | A << B[4:0] | 1 | NZC (C = last bit shifted out); V = 0 |
| 6 | `SHR` | A >> B[4:0] (logical) | 1 | NZC; V = 0 |
| 7 | `SAR` | A >> B[4:0] (arithmetic) | 1 | NZC; V = 0 |
| 8 | `PASS_A` | A (pass-through) | 1 | NZ; C,V = 0 |
| 9 | `PASS_B` | B (pass-through) | 1 | NZ; C,V = 0 |
| 10 | `NOT` | ~B | 1 | NZ; C,V = 0 |
| 11 | `ADC` | A + B + C | 1 | NZCV |
| 12 | `SBC` | A + ~B + C (= A − B − !C, ARM convention) | 1 | NZCV |
| 13 | `MUL` | A × B (signed), 64-bit product | N | NZ from low half (divmul peer) |
| 14 | `MULU` | A × B (unsigned), 64-bit product | N | NZ from low half |
| 15 | `DIV` | A / B (signed), quotient + remainder | N | NZ from quotient |
| 16 | `DIVU` | A / B (unsigned), quotient + remainder | N | NZ from quotient |
| 17–31 | — | Reserved | — | — |

Flags are only written to SR when `w_flags=1`; a flag write latches
all four flags, so the "= 0" entries above mean C/V are cleared, not
preserved. The flag column shows what the unit *computes*, not what
gets latched.

MUL/MULU/DIV/DIVU are implemented by the divmul peer unit
(`hw/rtl/common/divmul.sv`, see [`../divmul.md`](../divmul.md)); for
those values the `alu` field selects the divmul operation and the
ALU's own R-bus output is unused. They require `divmul_start=1` and
`branch=STALL` in the start micro-op.

### B-Bus Source — `bmux` [29:28] (2 bits)

| Value | Symbol | Source |
|-------|--------|--------|
| 0 | `REG` | Register file port B (addressed by `reg_b`) |
| 1 | `IMM` | Immediate from IR via immediate extractor (mode set by `imm_mode`) |
| 2 | `CONST4` | Hardwired constant 4 (for stack adjust, BL return address) |
| 3 | `CONST8` | Hardwired constant 8 (for stack adjust) |

### Write-Back Source — `wb_src` [27:26] (2 bits)

Selects the source for the W-bus (register write data and SR load
data).

| Value | Symbol | Source |
|-------|--------|--------|
| 0 | `RBUS` | R-bus (ALU result) |
| 1 | `MDR` | Memory Data Register (cache/memory or sysreg read data, after sub-word extraction) |
| 2 | `DML_LO` | divmul result low half (product low / quotient) |
| 3 | `DML_HI` | divmul result high half (product high / remainder) |

divmul results never touch the R-bus — they enter the register file
only through this mux. When a divmul source is selected
(`wb_src[1]=1`) and `w_flags=1`, the latched flags are the divmul's
Z/N (computed from the low half) with C and V cleared.

### Immediate Mode — `imm_mode` [25:24] (2 bits)

Controls how the 16-bit immediate from the IR is extended to 32 bits.
Only used when `bmux=IMM`.

| Value | Symbol | Extension |
|-------|--------|-----------|
| 0 | `ZERO_EXT` | `{16'b0, imm16}` |
| 1 | `SIGN_EXT` | `{{16{imm16[15]}}, imm16}` |
| 2 | `SHIFT_L16` | `{imm16, 16'b0}` |

**Immediate source routing:** Format L uses IR[15:0], Format M uses
IR[17:2] (the offset16 field). The datapath selects based on
instruction format.

### Flag Write Enable — `w_flags` [23] (1 bit)

When set, latches the NZCV flag outputs (ALU or divmul, following
`wb_src`) into the status register. Used by arithmetic/logic
instructions. Not used by MOV, loads, stores, branches, or system
instructions.

### SR Load — `sr_load` [22] (1 bit)

Bulk-loads the entire status register from the W-bus. Used by ERET
(restore SR from ESR). Overwrites N, Z, C, V, S, and I bits
simultaneously. (WRSPR SR reaches the same bulk-load path through the
hardware SPR decode rather than this bit.)

### MAR Load — `mar_load` [21] (1 bit)

Loads the Memory Address Register from the R-bus (ALU output). Sets
the address for subsequent memory read/write operations.

### MDR Load from Memory — `mdr_load_mem` [20] (1 bit)

Loads the Memory Data Register from the memory/cache read data bus.
Used during memory reads and sysreg reads (cpu_core muxes sysreg data
onto the same bus).

### MDR Load from A-Bus — `mdr_load_a` [19] (1 bit)

Loads MDR from the A-bus. Used to stage store data: the register to be
stored is driven onto the A-bus, captured in MDR, then MDR drives the
memory write data bus.

### Memory Read / Write — `mem_read` [18], `mem_write` [17] (1 bit each)

Initiate a cache/memory read or write at the address in MAR. Typically
paired with `branch=STALL` to wait for completion. The cache/memory
asserts `busy` for one or more cycles; the STALL branch holds the
micro-PC until busy clears.

### Memory Size — `mem_size` [16:15] (2 bits)

| Value | Symbol | Size |
|-------|--------|------|
| 0 | `BYTE` | 8-bit |
| 1 | `HALF` | 16-bit |
| 2 | `WORD` | 32-bit |

### Sign Extend — `sign_ext` [14] (1 bit)

For sub-word loads: when set, sign-extends the loaded byte/halfword to
32 bits. When clear, zero-extends. Ignored for word loads and all
stores.

### PC Source — `pc` [13:11] (3 bits)

Selects the next PC value. The selected value loads into the PC
register at the clock edge when `pc_load` is asserted (asserted
throughout S_EXEC unless a privilege violation suppresses it).

| Value | Symbol | Source | Use |
|-------|--------|--------|-----|
| 0 | `HOLD` | Current PC (no change) | Multi-step instructions, stalls |
| 1 | `NEXT` / `PLUS4` | PC + 4 | Sequential execution (default) |
| 2 | `OFFSET` | PC + sign_extend(offset22 << 2) | Branch target |
| 3 | `ABUS` | A-bus value | Indirect jump (JMP), ERET (EPC → PC) |
| 4 | `MDR` | MDR value | JALR (target staged in MDR), int_entry (handler address from vector table) |

Values 5–7 are reserved.

**Conditional override:** When `branch=BRT` and the ISA condition is
false, the sequencer forces `pc` to `NEXT` regardless of the
micro-word value. Same for `branch=BRF` when condition is true. This
implements conditional branches with a single micro-word.

### System/SPR Operation — `sys_op` [10:9] (2 bits)

Controls the sysreg sideband bus and SPR write path.

| Value | Symbol | Meaning |
|-------|--------|---------|
| 0 | `NONE` | No system operation (default) |
| 1 | `SPR_WRITE` | Write R-bus to SPR selected by IR[15:12] |
| 2 | `SYS_READ` | Sysreg bus read cycle |
| 3 | `SYS_WRITE` | Sysreg bus write cycle |

For sysreg operations (`SYS_READ`/`SYS_WRITE`), the device ID and
register index come from IR spare fields (routed by the field
extractor). During a sysreg read, cpu_core muxes the sysreg data onto
the memory read data bus, so `mdr_load_mem=1` captures it into MDR.

For `SPR_WRITE` (WRSPR), hardware decodes IR[15:12] to select the
target: ESR (0), EPC (1), USP (2, cross-bank R14 write), SR (3,
bulk-load of the entire SR), or SCR0–SCR3 (4–7, scratch SPRs). The
R-bus carries the write data (the W-mux passes it through with
`wb_src=RBUS`).

### Divmul Start — `divmul_start` [8] (1 bit)

Pulses the divmul peer unit's start input. The unit latches the A/B
operands and the operation (selected by the `alu` field:
MUL/MULU/DIV/DIVU) and begins iterating. The same micro-op uses
`branch=STALL`: divmul asserts `busy` combinationally on the start
cycle and holds it through the iteration, so the STALL takes effect
immediately and releases when busy falls.

### Branch Condition — `branch` [7:5] (3 bits)

Controls micro-sequencer flow. Determines whether the micro-PC
advances, holds, or returns to the fetch state.

| Value | Symbol | Micro-PC Action | Notes |
|-------|--------|-----------------|-------|
| 0 | `SEQ` | micro-PC++ | Continue to next micro-op |
| 1 | `FETCH` | → S_FETCH | Instruction complete, fetch next |
| 2 | `STALL` | fault → S_FETCH; !busy → micro-PC++; else hold | Wait for memory/divmul |
| 3 | `BRT` | → S_FETCH (pc overridden if cond false) | Branch if True |
| 4 | `BRF` | → S_FETCH (pc overridden if cond true) | Branch if False |
| 5 | — | (reserved) | — |
| 6 | `SKIP` | micro-PC += 1 + fwd_offset | Forward skip |
| 7 | — | Detected as BR_ILLEGAL | Illegal sentinel; traps to vector 7 |

**STALL details:** Three-way resolution, fault checked first:
1. `mem_fault` or `arith_fault` asserted → abort instruction
   (`go_fetch`), even if busy is still high; cpu_core generates the
   exception
2. `busy` deasserted → advance to next micro-op
3. `busy` still asserted → hold micro-PC (repeat this micro-word)

The unified busy signal is `divmul_busy | mem_busy`. Memory and divmul
ops never overlap in the same micro-op.

**BRT/BRF details:** Both always return to fetch (`go_fetch=1`). The
difference is in `pc` override:
- BRT: if ISA condition **false**, force `pc = NEXT` (fall through)
- BRF: if ISA condition **true**, force `pc = NEXT` (fall through)

The ISA condition is evaluated by `cond_eval` from IR[29:26] (Format B
condition field) against SR flags.

**Illegal sentinel:** the assembler fills every unused ROM entry with
a word whose only set field is `branch=7`. The sequencer detects it on
the first micro-op of a dispatch and traps to `VEC_ILLEGAL` (7).

### Forward Skip Offset — `fwd_offset` [4:2] (3 bits)

Only used when `branch=SKIP`. The micro-PC advances by `1 +
fwd_offset` (skip 1–8 micro-ops forward). Used by all four
MUL/MULU/DIV/DIVU routines to converge on the shared high-half
writeback tail at 0x49.

### EI/DI Control — `ei_set` [1], `di_set` [0] (1 bit each)

| Field | Effect |
|-------|--------|
| `ei_set` | Sets SR.I=1 (enable interrupts). **Delayed** by one instruction via `ei_shadow` |
| `di_set` | Sets SR.I=0 (disable interrupts). **Immediate** effect |

The `ei_shadow` mechanism: when EI executes, a flip-flop is set that
suppresses interrupt recognition until the next instruction completes.
This allows atomic `EI; ERET` sequences where ERET executes in the
"shadow" before any pending interrupt fires. The sequencer clears
`ei_shadow` after the next instruction's `go_fetch`.

---

## Assembler Defaults

The microcode assembler (`uasm.py`) applies these defaults when a
field is not specified:

| Field | Default | Meaning |
|-------|---------|---------|
| `pc` | `NEXT` (1) | Advance PC by 4 |
| `branch` | `FETCH` (1) | Return to fetch state |
| `mem_size` | `WORD` (2) | Word-size memory access |
| All others | 0 | No operation / hold / register 0 / ALU ADD |

These defaults mean a single-field-assignment line like `ei_set=1` is
a complete micro-op: it sets EI, advances PC, and returns to fetch.
Multi-step instructions must explicitly override both `pc=HOLD` and
`branch=SEQ` on all steps except the last.

**Common mistake:** Forgetting `branch=SEQ` on intermediate steps.
Without it, the default `branch=FETCH` returns to fetch immediately,
and subsequent micro-ops never execute.

### Assembler Syntax

```
# Comments start with #
.org 0x00           # Set ROM address

label:              # Label (documentation, not referenced by micro-ops)
  field=VALUE field=VALUE ...   # One micro-op per line
  field=VALUE ...               # Next micro-op at ROM address + 1
```

Fields are `key=value` pairs separated by spaces. Values are symbolic
names (uppercase) or integer literals (decimal, `0x` hex, `0b`
binary). Full syntax, value lists, and build commands:
[`uasm-syntax.md`](uasm-syntax.md).

---

## ROM Organization

### Address Space

256 entries (8-bit micro-PC), divided into zones by instruction
format:

| Zone | Addresses | Slot Size | Slots | Purpose |
|------|-----------|-----------|-------|---------|
| R-ALU | 0x00–0x1F | ×2 | 16 | Format R ALU ops, op[4]=0 (ops 0–11 used; 12–15 reserved-illegal) |
| Format L | 0x20–0x3F | ×2 | 16 | Immediate operations (ops 0–12 used; 13–15 reserved) |
| R-SYS | 0x40–0x5F | ×2 | 16 | Format R op[4]=1: divmul dispatch (ops 16–19, 0x40–0x46, shared tail at 0x49), SYS ops 23–31 (0x4E–0x5E); ops 20–22 reserved-illegal |
| Format B | 0x60–0x63 | ×2 | 2 | Conditional branch (0x60), BL (0x62) |
| (gap) | 0x64–0x6F | — | — | Unused |
| Exception | 0x70–0x7F | whole zone | 1 | `int_entry` (3 micro-ops) |
| Format M | 0x80–0xBF | ×4 | 16 | Load/store operations |
| (unused) | 0xC0–0xFF | — | — | Available for future expansion |

The Exception zone has no internal slot boundaries — `int_entry`
consumes consecutive entries from 0x70 and may grow up to the zone
end.

### Dispatch Address Computation

Computed by the fetch logic in `hw/rtl/penumbra1/cpu_core.sv` from the
fetched instruction word (`icache_rdata`, the same cycle the IR
latches):

| Format | Formula | Range |
|--------|---------|-------|
| R (prefix 00) | `{0, op[4], 0, op[3:0], 0}` | 0x00–0x1E (ALU), 0x40–0x5E (divmul/SYS) |
| L (prefix 01) | `{01, op[3:0], 0}` | 0x20–0x3E |
| M (prefix 10) | `{10, L, sz[1:0], SE, 00}` | 0x80–0xBC |
| B (prefix 11) | `cond==1111 ? 0x62 : 0x60` | 0x60 or 0x62 |
| Exception | Hardwired | 0x70 |

Format R uses ×2 spacing split by `op[4]`: ALU zone (`op[4]=0`,
0x00–0x1E) and divmul/SYS zone (`op[4]=1`, 0x40–0x5E). The formula
`{0, op[4], 0, op[3:0], 0}` is pure wiring — zero gates.

Format B dispatches all conditional/unconditional branches to 0x60 and
BL (cond=1111) to 0x62. The BL detection is a 4-input AND gate on the
fetched word's bits [29:26].

### Slot Boundaries

Multi-step micro-routines must not cross slot boundaries. The
microcode assembler validates this automatically. If a routine at 0x42
(R-SYS, slot size 2) has 3 micro-ops, assembly fails with an overflow
error.

### Dispatch-Time Interception

SYSCALL and BREAK are intercepted at dispatch time in cpu_core and
never reach their ROM entries:

| Instruction | Dispatch Addr | Interception | Vector |
|-------------|--------------|--------------|--------|
| SYSCALL | 0x52 | `dispatch_addr == 0x52` | VEC_SYSCALL (5) |
| BREAK | 0x54 | `dispatch_addr == 0x54` | VEC_BREAK (6) |

Both trigger `except_entry` (saves EPC/ESR, sets S=1/I=0) and redirect
dispatch to 0x70 (int_entry).

Interrupts are also checked at dispatch time, timer first:

```
timer_irq_taken = i_timer_irq & SR.I & !ei_shadow            → VEC_TIMER (1)
ext_irq_taken   = i_irq & SR.I & !ei_shadow & !i_timer_irq   → VEC_EXT_IRQ (9)
```

A pending timer interrupt masks the external interrupt for that
dispatch; the external IRQ is taken on a later dispatch once the timer
line drops.

**Privilege violations are not dispatch-address based.** The sequencer
evaluates the per-micro-word `priv` bit when the instruction's first
micro-op executes (`priv & !SR.S` — see the
[`priv` field](#privileged-instruction--priv-51-1-bit)). A
dispatch-zone check on 0x40–0x5E would wrongly trap user-mode
MUL/MULU/DIV/DIVU, which live in the op[4]=1 region (0x40–0x46) but
are unprivileged.

MMU faults are detected during STALL (data) or during S_FETCH
(instruction fetch), not at dispatch. The sequencer aborts the
instruction, and `fault_pending` overrides the next dispatch to 0x70.

---

## Exception Integration

### except_entry Pulse

When any exception fires, cpu_core asserts `except_entry` for one
cycle. This triggers hardware pre-actions in the datapath:

1. **EPC ← PC** — saves the current program counter
2. **ESR ← SR** — saves the current status register
3. **SR.S ← 1, SR.I ← 0** — enter supervisor mode, disable interrupts
4. **SP bank swap** — R14 now accesses SSP (follows SR.S)

These happen atomically in hardware, before the int_entry micro-ops
execute.

### int_entry Micro-Routine (0x70, 3 micro-ops)

After hardware pre-actions, the sequencer dispatches to 0x70. The
vector table holds handler **addresses** (MIPS/68k-style), so entry is
an indirect fetch through memory:

```
Step 0: a_src=VECTOR alu=PASS_A mar_load=1 pc=HOLD branch=SEQ
Step 1: mem_read=1 mdr_load_mem=1 pc=HOLD branch=STALL
Step 2: pc=MDR
```

Step 0 computes MAR = `{26'b0, vector_num, 2'b00}` — word-aligned
entries at physical address 0x00. Step 1 reads the handler address
from the table into MDR (stalling for memory). Step 2 loads it into PC
and hands off to the fetch unit. Software fills the table with handler
addresses (plain STW) at boot time.

### Vector Table Read Bypass

The vector-table read must work without a TLB mapping. cpu_core sets a
`vector_read` flag when `except_entry` fires and clears it at the next
`fetch_go`; the MMU's force-bypass input is driven with `vector_read
&& !fetch_active`. The data read in int_entry step 1 therefore
bypasses translation (the table is physical), eliminating nested TLB
misses on exception entry. The bypass covers **only that data read** —
the subsequent instruction fetch of the handler goes through normal
translation, so handler code must be reachable via the TLB (e.g. a
pinned kernel mapping).

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
| 9 | 0x24 | External device interrupt |
| 10 | 0x28 | Arithmetic fault (DIV0, from divmul) |

### Priority

`fault_pending > illegal_pending > priv_pending > arith_pending >
dispatch-time group`

Within the dispatch-time group (recognized together at dispatch):
`BREAK > SYSCALL > timer IRQ > external IRQ`.

Fault entry sets SR.I=0, so `irq_taken` is false at the next dispatch
— faults always take priority over interrupts.

---

## Complete Micro-Routine Catalog

### Notation

- Fields not listed use assembler defaults (`pc=NEXT branch=FETCH
  mem_size=WORD` on last step, all others 0)
- Multi-step routines show step numbers; step 0 is at the dispatch
  address
- `→` indicates the datapath flow for that step

### Format R — ALU Operations (0x00–0x16)

All single-cycle, single micro-op. Pattern: read Rd and Rs, compute,
write result to Rd, update flags. Ops 12–15 (0x18–0x1E) are
reserved-illegal.

**ADD** (op=0, dispatch=0x00)
```
reg_a=IR_RD reg_b=IR_RS reg_w=IR_RD w_en=1 alu=ADD bmux=REG wb_src=RBUS w_flags=1
```
→ Rd = Rd + Rs, flags updated. CMP (F=1) variant: flags only, Rd
unchanged.

**SUB** (op=1, dispatch=0x02) — same pattern, `alu=SUB`. CMP = SUB
with F=1.

**AND** (op=2, dispatch=0x04) — same pattern, `alu=AND`. TEST = AND
with F=1.

**OR** (op=3, dispatch=0x06) — same pattern, `alu=OR`.

**XOR** (op=4, dispatch=0x08) — same pattern, `alu=XOR`.

**SHL** (op=5, dispatch=0x0A) — same pattern, `alu=SHL`.

**SHR** (op=6, dispatch=0x0C) — same pattern, `alu=SHR`.

**SAR** (op=7, dispatch=0x0E) — same pattern, `alu=SAR`.

**MOV** (op=8, dispatch=0x10)
```
reg_a=IR_RS reg_w=IR_RD w_en=1 alu=PASS_A wb_src=RBUS
```
→ Rd = Rs. Does **not** update flags.

**NOT** (op=9, dispatch=0x12)
```
reg_b=IR_RS reg_w=IR_RD w_en=1 alu=NOT wb_src=RBUS w_flags=1
```
→ Rd = ~Rs, flags updated. The operand is read on the **B** port —
the ALU's NOT computes `~B`.

**ADC** (op=10, dispatch=0x14)
```
reg_a=IR_RD reg_b=IR_RS reg_w=IR_RD w_en=1 alu=ADC bmux=REG wb_src=RBUS w_flags=1
```
→ Rd = Rd + Rs + C (add with carry from SR.C), flags updated.

**SBC** (op=11, dispatch=0x16)
```
reg_a=IR_RD reg_b=IR_RS reg_w=IR_RD w_en=1 alu=SBC bmux=REG wb_src=RBUS w_flags=1
```
→ Rd = Rd − Rs − !C (subtract with borrow, ARM convention), flags
updated.

### Format L — Immediate Operations (0x20–0x38)

All single-cycle, single micro-op except JALR (2 micro-ops). Ops
13–15 (0x3A–0x3E) are reserved.

**LLI** (op=0, dispatch=0x20)
```
reg_w=IR_RD w_en=1 alu=PASS_B bmux=IMM wb_src=RBUS imm_mode=ZERO_EXT
```
→ Rd = zero_extend(imm16).

**LLIS** (op=1, dispatch=0x22)
```
reg_w=IR_RD w_en=1 alu=PASS_B bmux=IMM wb_src=RBUS imm_mode=SIGN_EXT
```
→ Rd = sign_extend(imm16).

**LUI** (op=2, dispatch=0x24)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=OR bmux=IMM wb_src=RBUS imm_mode=SHIFT_L16
```
→ Rd = Rd | (imm16 << 16). Typically preceded by LLI to build a 32-bit
constant.

**ADDi (alias `INC`)** (op=3, dispatch=0x26)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=ADD bmux=IMM wb_src=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd + zero_extend(imm16), flags updated. Assembler accepts `ADD
Rd, #imm`.

**SUBi (alias `DEC`)** (op=4, dispatch=0x28)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=SUB bmux=IMM wb_src=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd − zero_extend(imm16), flags updated. Assembler accepts `SUB
Rd, #imm`.

**CMPi** (op=5, dispatch=0x2A)
```
reg_a=IR_RD alu=SUB bmux=IMM wb_src=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Flags = Rd − zero_extend(imm16). Rd **not** written (no `w_en`).

**ANDi** (op=6, dispatch=0x2C)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=AND bmux=IMM wb_src=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd & zero_extend(imm16), flags updated.

**TESTi** (op=7, dispatch=0x2E)
```
reg_a=IR_RD alu=AND bmux=IMM wb_src=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Flags = Rd & zero_extend(imm16). Rd **not** written (no `w_en`).

**SHLi** (op=8, dispatch=0x30)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=SHL bmux=IMM wb_src=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd << imm[4:0], flags updated.

**SHRi** (op=9, dispatch=0x32)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=SHR bmux=IMM wb_src=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd >> imm[4:0] (logical), flags updated.

**SARi** (op=10, dispatch=0x34)
```
reg_a=IR_RD reg_w=IR_RD w_en=1 alu=SAR bmux=IMM wb_src=RBUS imm_mode=ZERO_EXT w_flags=1
```
→ Rd = Rd >> imm[4:0] (arithmetic), flags updated.

**JMP Rd** (op=11, dispatch=0x36)
```
reg_a=IR_RD pc=ABUS
```
→ PC = Rd field. (Format L uses the Rd slot, not Rs.) Assembler alias:
`RET` = `JMP R13`.

**JALR Rd** (op=12, dispatch=0x38) — 2 micro-ops
```
Step 0: reg_a=IR_RD mdr_load_a=1 pc=HOLD branch=SEQ
Step 1: reg_a=R15 bmux=CONST4 alu=ADD reg_w=R13 w_en=1 wb_src=RBUS pc=MDR
```
→ MDR ← old Rd, then R13 = PC + 4 (link) and PC = MDR (= old Rd) in
the same µ-op (link writeback and PC-source are independent fields).
Used for indirect calls (function pointers, vtables).

**Why the Rd-first capture matters:** the compiler's canonical
indirect-call sequence is `ldw r13, [ptr]; jalr r13` — both target and
link alias the same register. Reading Rd *before* the link write
ensures `jalr r13` jumps to the original target rather than
self-clobbering to `PC+4`. A pipelined implementation gets this for
free by reading operands at issue and writing back later.

### Format R — Divmul Operations (op 16–19, 0x40–0x46)

MUL/MULU/DIV/DIVU run on the divmul peer unit (algorithm, handshake,
and datapath in [`../divmul.md`](../divmul.md)). Each is a 3-µop
routine. The ×2 dispatch spacing gives each opcode two ROM slots, so
the third µop is a **shared high-half writeback tail** at 0x49 (op
20's second slot, never a dispatch target), reached via `SKIP`. Total
footprint: 4 dispatch slots × 2 entries + 1 shared tail = 9 ROM
entries.

**MUL** (op=16, dispatch=0x40), **MULU** (op=17, 0x42), **DIV**
(op=18, 0x44), **DIVU** (op=19, 0x46)
```
Step 0: reg_a=IR_RD reg_b=IR_RS alu=<MUL|MULU|DIV|DIVU> divmul_start=1 bmux=REG pc=HOLD branch=STALL
Step 1: reg_w=IR_RD w_en=1 wb_src=DML_LO w_flags=1 pc=HOLD branch=SKIP fwd_offset=<7|5|3|1>
Tail (0x49): reg_w=IR_RDH w_en=1 wb_src=DML_HI
```
→ Step 0 latches Rd/Rs into divmul and selects the operation via the
`alu` field; divmul asserts busy combinationally on the start edge and
holds it through the iteration (roughly 33 cycles), so the same µ-op's
STALL waits for completion. A DIV0 asserts divmul's fault output,
which the STALL check turns into an abort → `VEC_ARITH` (10). Step 1
writes the low half (product low / quotient) to Rd and latches Z/N,
then SKIPs to 0x49 (the four `fwd_offset` values 7/5/3/1 all land
there). The tail writes the high half (product high / remainder) to
Rdh and advances PC. For the 2-operand form (Rdh=R0) the regfile
silently drops the high-half write — no microcode branch needed.

The divmul op is decoded from the `alu` field (which the sequencer
already emits); no separate divmul-op micro-word field exists. The
results enter the register file only through `wb_src`
(DML_LO/DML_HI) — never via the R-bus.

### Format R — System Operations (op 23–31, 0x4E–0x5E)

Format R SYS ops sit at the high half of the op[4]=1 zone (dispatch
`{0, 1, op[3:0], 0}` — see `hw/rtl/penumbra1/cpu_core.sv`). Within
that zone, ops 16–19 (0x40–0x46) are the divmul dispatch slots above
and 0x49 holds their shared tail; ops 20–22 (0x48, 0x4A, 0x4C) are
reserved and trap as illegal via the sentinel.

**WRSYS Rd, #dev, #reg** (op=23, dispatch=0x4E) — Privileged
```
priv=1 reg_a=IR_RD sys_op=SYS_WRITE
```
→ A-bus = Rd → sysreg write bus. Device/register from IR spare fields.

**RDSYS Rd, #dev, #reg** (op=24, dispatch=0x50) — 2 micro-ops,
Privileged
```
Step 0: priv=1 sys_op=SYS_READ mdr_load_mem=1 pc=HOLD branch=SEQ
Step 1: reg_w=IR_RD w_en=1 wb_src=MDR
```
→ Step 0: sysreg data → mem_rdata bus → MDR. Step 1: MDR → Rd.

**SYSCALL** (op=25, dispatch=0x52) — Intercepted at dispatch Never
reaches ROM. Detected by `dispatch_addr == 0x52`. Triggers
`except_entry` → VEC_SYSCALL (5) → `int_entry` at 0x70. Unprivileged.
EPC points at the SYSCALL; handler is responsible for advancing EPC+4
before `ERET`.

**BREAK** (op=26, dispatch=0x54) — Intercepted at dispatch Never
reaches ROM. Detected by `dispatch_addr == 0x54`. Triggers
`except_entry` → VEC_BREAK (6) → `int_entry` at 0x70. Unprivileged.
`o_halted` pulses for testbench halt.

**ERET** (op=27, dispatch=0x56) — 2 micro-ops, Privileged
```
Step 0: priv=1 a_src=ESR alu=PASS_A sr_load=1 pc=HOLD branch=SEQ
Step 1: a_src=EPC pc=ABUS
```
→ Step 0: ESR → A-bus → ALU → R-bus → W-bus → SR (restores flags, S, I
bits, may trigger SP bank swap). Step 1: EPC → A-bus → PC (resume at
saved address). Single-form only — there is no `ERET Rd, Rs` variant;
context switches use `WRSPR EPC/ESR; ERET`.

**EI** (op=28, dispatch=0x58) — Privileged
```
priv=1 ei_set=1
```
→ SR.I = 1, with one-instruction delay (ei_shadow). User-mode `EI`
traps to `VEC_PRIV`.

**DI** (op=29, dispatch=0x5A) — Privileged
```
priv=1 di_set=1
```
→ SR.I = 0, immediate effect.

**WRSPR {ESR|EPC|USP|SR|SCR0–3}, Rd** (op=30, dispatch=0x5C) —
Privileged
```
priv=1 reg_a=IR_RD alu=PASS_A sys_op=SPR_WRITE
```
→ R-bus = Rd value → SPR write target. Hardware decodes IR[15:12]: SPR
0 (ESR) → `esr_load`, SPR 1 (EPC) → `epc_load`, SPR 2 (USP) → R14
cross-bank write, SPR 3 (SR) → bulk-load of the entire SR (flags, S,
I — same path ERET's `sr_load` uses; this is how a context switch
installs a new SR), SPR 4–7 (SCR0–SCR3) → respective scratch storage.

**RDSPR Rd, {ESR|EPC|USP|SR|SCR0–3}** (op=31, dispatch=0x5E) —
Privileged
```
priv=1 a_src=SPR reg_a=R14 alu=PASS_A reg_w=IR_RD w_en=1 wb_src=RBUS
```
→ Rd = SPR value. Hardware decodes IR[15:12]: SPR 0 → A-bus=ESR, SPR 1
→ A-bus=EPC, SPR 2 → A-bus=R14 cross-bank read, SPR 3 → A-bus=SR
(current status), SPR 4–7 → SCR0–SCR3 scratch storage. The
`reg_a=R14` is load-bearing: for the USP case the SPR decode only
flips the bank select — the register address itself comes from the
micro-word, so it must point at R14 (the datapath asserts this).

### Format B — Branches (0x60, 0x62)

**Bcc** (conditional/unconditional, dispatch=0x60)
```
pc=OFFSET branch=BRT
```
→ If ISA condition true: PC = PC + offset. If false: PC = PC + 4 (fall
through). All 15 conditions (AL, EQ, NE, CS, CC, MI, PL, VS, VC, HI,
LS, GE, LT, GT, LE) use this single entry.

**BL** (branch-and-link, cond=1111, dispatch=0x62) — 2 micro-ops
```
Step 0: reg_a=R15 bmux=CONST4 alu=ADD reg_w=R13 w_en=1 wb_src=RBUS pc=HOLD branch=SEQ
Step 1: pc=OFFSET
```
→ Step 0: R13 (LR) = R15 (PC) + 4 = return address. Step 1: PC = PC +
offset (always taken). Return via `RET` (JMP R13).

### Exception Entry (0x70) — 3 micro-ops

**int_entry** (hardwired dispatch after except_entry pulse)
```
Step 0: a_src=VECTOR alu=PASS_A mar_load=1 pc=HOLD branch=SEQ
Step 1: mem_read=1 mdr_load_mem=1 pc=HOLD branch=STALL
Step 2: pc=MDR
```
→ MAR = vector address, read the handler address from the vector table
(MMU bypassed — see
[Vector Table Read Bypass](#vector-table-read-bypass)), PC = handler
address. Shared by all exception sources. Hardware pre-actions
(EPC/ESR save, mode switch) have already executed before this routine
runs.

### Format M — Memory Operations (0x80–0xBF)

**STW Rd, [Rb + #offset]** (L=0, sz=10, SE=0, dispatch=0x90) — 4
micro-ops
```
Step 0: reg_a=IR_RS bmux=IMM imm_mode=SIGN_EXT alu=ADD mar_load=1 pc=HOLD branch=SEQ
Step 1: reg_a=IR_RD mdr_load_a=1 pc=HOLD branch=SEQ
Step 2: mem_write=1 mem_size=WORD pc=HOLD branch=STALL
Step 3: pc=NEXT
```
→ Step 0: MAR = Rb + sign_extend(offset). Step 1: MDR = Rd (via
A-bus). Step 2: write MDR to [MAR], stall until accepted. Step 3:
advance PC, fetch next.

**STB** (dispatch=0x80) and **STH** (dispatch=0x88) — identical
4-µop pattern with `mem_size=BYTE` / `mem_size=HALF` on the write
step (the byte replicator copies the low byte/halfword across lanes;
byte enables select the position).

**LDW Rd, [Rb + #offset]** (L=1, sz=10, SE=0, dispatch=0xB0) — 3
micro-ops
```
Step 0: reg_a=IR_RS bmux=IMM imm_mode=SIGN_EXT alu=ADD mar_load=1 pc=HOLD branch=SEQ
Step 1: mem_read=1 mem_size=WORD mdr_load_mem=1 pc=HOLD branch=STALL
Step 2: reg_w=IR_RD w_en=1 wb_src=MDR
```
→ Step 0: MAR = Rb + sign_extend(offset). Step 1: read [MAR] into MDR,
stall until ready. Step 2: Rd = MDR, advance PC.

**LDB** (0xA0), **LDBS** (0xA4), **LDH** (0xA8), **LDHS** (0xAC) —
identical 3-µop pattern with `mem_size=BYTE`/`HALF` (and `sign_ext=1`
for the S variants) on **both** the read step and the writeback step:
the byte extractor sits between MDR and the W-mux, so the writeback
µ-op must repeat the size/sign fields for correct extraction.

---

## Adding a New Instruction

Step-by-step guide using BL as the example:

### 1. Identify the dispatch address

Check the instruction format and encoding. BL is Format B with
cond=1111. Regular branches dispatch to 0x60; BL needs its own entry.
The Format B zone spans 0x60–0x63 (two ×2 slots). BL dispatches to
0x62.

If the new instruction fits in an existing zone with unused slots,
just use the slot. If not, modify the dispatch logic in
`hw/rtl/penumbra1/cpu_core.sv`.

### 2. Write the microcode

Add the micro-routine to `hw/microcode/microcode.uasm`:

```
.org 0x62
bl:
  reg_a=R15 bmux=CONST4 alu=ADD reg_w=R13 w_en=1 wb_src=RBUS pc=HOLD branch=SEQ
  pc=OFFSET
```

Key rules:
- Intermediate steps need `pc=HOLD branch=SEQ`
- Last step uses defaults (`pc=NEXT branch=FETCH`) or explicit branch
  target
- Run `uasm.py` to verify slot boundaries

### 3. Update the dispatch (if needed)

In `hw/rtl/penumbra1/cpu_core.sv`, modify the dispatch address
computation:

```verilog
2'b11: dispatch_addr = (fetch_rdata[29:26] == 4'b1111) ? 8'h62 : 8'h60;
```

### 4. Update the assembler (if needed)

The ISA assembler (`pasm.py`) already had BL in `BRANCH_OPS` with
cond=15. If adding a wholly new instruction, add it to the appropriate
table (`FORMAT_R_OPS`, `FORMAT_L_OPS`, etc.).

### 5. Update slot zones (if needed)

In `uasm.py`, update `SLOT_ZONES` if the zone boundaries changed:

```python
(0x60, 0x64, 2, "Format B"),
```

### 6. Write a test

Add `hw/sim/programs/isa/test_<name>.s` (new instructions are
ISA-level — they must pass on every core generation) following the
pass/fail convention:
- R1 = 1 means PASS, R1 = 0 means FAIL
- End with `BREAK`
- Self-checking: test sets R1 and branches to `fail:` on assertion
  failure

### 7. Run the full test suite

```
make test
```

Verify no regressions.
