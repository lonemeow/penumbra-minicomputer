# Penumbra CPU - Microcode Validation

## Purpose

Validate the micro-word format from `datapath.md` by writing bit-level micro-programs for representative instructions. Goal: catch missing signals, conflicting fields, and width errors before RTL.

## Finding: Micro-Word Width Discrepancy

The spec claims 52 bits, but the fields sum to **55 bits**:

| Field              | Width | Running total |
|--------------------|-------|---------------|
| `reg_a_sel[3:0]`  | 4     | 4             |
| `reg_b_sel[3:0]`  | 4     | 8             |
| `reg_w_sel[3:0]`  | 4     | 12            |
| `reg_w_en`         | 1     | 13            |
| `alu_op[3:0]`     | 4     | 17            |
| `b_mux_sel`        | 1     | 18            |
| `w_mux_sel`        | 1     | 19            |
| `imm_mode[1:0]`   | 2     | 21            |
| `flag_w_en`        | 1     | 22            |
| `mar_load`         | 1     | 23            |
| `mar_src`          | 1     | 24            |
| `mdr_load_mem`     | 1     | 25            |
| `mdr_load_a`       | 1     | 26            |
| `mem_read`         | 1     | 27            |
| `mem_write`        | 1     | 28            |
| `mem_size[1:0]`   | 2     | 30            |
| `sign_ext`         | 1     | 31            |
| `pc_src[1:0]`     | 2     | 33            |
| `pc_mdr_load`      | 1     | 34            |
| `stall_sel[1:0]`  | 2     | 36            |
| `sys_cycle`        | 1     | 37            |
| `sys_we`           | 1     | 38            |
| `lu_start`         | 1     | 39            |
| `lu_sel[1:0]`     | 2     | 41            |
| `lu_to_rbus`       | 1     | 42            |
| `next_addr[9:0]`  | 10    | 52            |
| `branch_cond[2:0]`| 3     | **55**        |

### Resolution

Through iterative validation (writing micro-programs, analyzing the sequencer, and redesigning the fetch cycle), the micro-word was revised to **49 bits** — 6 bits *smaller* than the original despite adding 4 new fields for exception support. See the **Revised Micro-Word Format** section at the end of this document for the final field table and full change log.

---

## ALU Op Encoding (from ISA)

| Code | Mnemonic | Description              |
|------|----------|--------------------------|
| 0000 | ADD      | A + B                    |
| 0001 | SUB      | A - B                    |
| 0010 | AND      | A & B                    |
| 0011 | OR       | A \| B                   |
| 0100 | XOR      | A ^ B                    |
| 0101 | SHL      | A << B[4:0]              |
| 0110 | SHR      | A >> B[4:0] (logical)    |
| 0111 | SAR      | A >> B[4:0] (arithmetic) |
| 1000 | PASS_A   | A (pass-through)         |
| 1001 | PASS_B   | B (pass-through)         |
| 1010 | NOT      | ~B                       |

---

## Micro-Sequencer (revised)

### Sequencer Fields

The original `next_addr[9:0]` (10 bits) + `branch_cond[2:0]` (3 bits) = 13 bits for sequencer control. Analysis of all micro-routines shows that no instruction requires absolute jumps, backward jumps, or micro-subroutines. Every micro-routine is a linear sequence with possible stall holds and a return to fetch.

**Revised fields:** `branch_cond[2:0]` (3 bits) + `fwd_offset[2:0]` (3 bits) = **6 bits** (saving 7 bits).

The `fwd_offset` is a 3-bit unsigned forward skip (0-7 micro-ops) used only when `branch_cond = 110`. For all other branch_cond values, `fwd_offset` is ignored by the sequencer (available as don't-care / future use).

### Branch Conditions

| Code | Mnemonic | micro-PC action | `pc_src` behavior | `fwd_offset` |
|------|----------|-----------------|-------------------|---------------|
| 000  | SEQ      | micro-PC++      | unconditional     | ignored       |
| 001  | FETCH    | hand off to fetch unit (see below) | unconditional | ignored |
| 010  | STALL    | busy ? hold : (fault ? exception via fetch unit : micro-PC++) | unconditional | ignored |
| 011  | BRT      | hand off to fetch unit | applied only if ISA cond true, else forced to PC+4 | ignored |
| 100  | BRF      | hand off to fetch unit | applied only if ISA cond false, else forced to PC+4 | ignored |
| 101  | PRIV     | SR.S=0 ? exception (vec 3) via fetch unit : micro-PC++ | unconditional | ignored |
| 110  | SKIP     | micro-PC += 1 + fwd_offset | unconditional | **used** (0-7) |
| 111  | (reserved) | — | — | — |

FETCH, BRT, and BRF all signal "instruction complete" and hand control to the **fetch unit** (see next section). The fetch unit handles IR latching, interrupt checking, and dispatch — the micro-sequencer never explicitly dispatches. This separation allows the fetch mechanism to be upgraded from on-demand (phase 1) to prefetch-during-execution (phase 2) without changing any micro-words.

### Hardwired Dispatch Addresses

The micro-sequencer receives its next micro-PC from the fetch unit when a new instruction is dispatched:
- **IR-mapped:** computed from instruction bits by the fetch unit's decode logic.
- **Interrupt entry:** hardwired start address of the interrupt micro-routine, selected by the fetch unit when a pending IRQ is detected (instead of normal dispatch).

SYSCALL/BREAK entry points trigger hardware pre-actions (shadow latch, mode switch) and then sequence (micro-PC++) into the interrupt entry micro-routine placed consecutively in ROM.

---

## Instruction Fetch — Hardwired Fetch Unit (revised)

The original design used a 4-micro-op fetch routine in ROM. This has been replaced by a **hardware fetch unit** that operates outside of microcode. This eliminates fetch micro-ops from ROM, resolves the `ir_load` issue (#2), removes the need for `mar_src` (saving 1 bit), and provides a clean upgrade path to prefetched (0-cycle) instruction delivery.

### Architecture

The I-cache address input is **permanently wired to PC** (split I/D cache makes this free — no mux needed). The fetch unit is a small state machine with the following interface:

| Signal | Direction | Purpose |
|--------|-----------|---------|
| `fetch_go` | sequencer → fetch | Triggered when branch_cond ∈ {FETCH, BRT, BRF} |
| `ir_valid` | fetch → sequencer | Instruction latched in IR, ready for dispatch |
| `dispatch_addr` | fetch → sequencer | Micro-PC start address (computed from IR bits) |
| `fetch_invalidate` | datapath → fetch | Branch taken — discard any prefetch (phase 2 only) |

### Fetch Unit Behavior

When `fetch_go` is asserted (instruction complete):

```
1. Check pending interrupts:
   - If (IRQ pending AND SR.I=1 AND NOT ei_shadow):
       → trigger hardware pre-actions (shadow latch, mode switch)
       → dispatch_addr ← interrupt_entry_start (hardwired)
       → ir_valid ← 1
       → done (micro-sequencer loads dispatch_addr)
   - Else: proceed to instruction fetch

2. Instruction fetch:
   - Read I-cache at current PC
   - If I-cache miss: stall until ready
   - If I-cache hit:
       → IR ← I-cache data output
       → dispatch_addr ← decode(I-cache data)  [combinational, reads data lines directly]
       → ir_valid ← 1
       → done (micro-sequencer loads dispatch_addr)
```

Note: **PC advancement (PC += 4) is handled by `pc_src` in the micro-word**, not by the fetch unit. Non-branch instructions set `pc_src = 001` (PC+4) in their last micro-op. Branch instructions set `pc_src = 010` (PC+offset) or `011` (A-bus). The fetch unit reads I-cache at whatever PC currently holds.

### Dispatch Address Computation

The fetch unit computes the dispatch address from I-cache data lines (not IR — IR is latching simultaneously):

```
Format R: dispatch = {00, data[29:25]}         → entries 0-31
Format L: dispatch = {01, data[29:27], 00}     → entries 32-63 (×4 spacing)
Format M: dispatch = {10, data[29:26], 0}      → entries 64-95 (×2 spacing)
Format B: dispatch = {11, 0000000}             → entry 96
```

### Phase 1 → Phase 2 Upgrade Path

The fetch unit interface is designed so that upgrading to prefetched execution requires **no microcode changes**:

| | Phase 1 (initial) | Phase 2 (future) |
|---|---|---|
| When does fetch start? | On `fetch_go` | Autonomously when PC changes |
| `ir_valid` timing | 1+ cycles after `fetch_go` | May already be 1 when `fetch_go` fires |
| Branch penalty | Same as I-cache latency | Prefetch restart latency (miss-like) |
| Microcode changes | — | None |

### Original Fetch Micro-Routine (superseded)

The original 4-micro-op fetch sequence is preserved here for reference. It is no longer used — all its functionality is handled by the fetch unit.

```
fetch-0: MAR ← PC (mar_src=1)
fetch-1: mem_read, stall until ready
fetch-2: MDR → IR, PC ← PC+4
fetch-3: dispatch from IR
```

**Original issue #2 (ir_load signal) — resolved.** The fetch unit controls IR latching directly, so no micro-word signal is needed. The issue of `mdr_load_mem` conflicting between fetch and data loads no longer exists.

---

## 1. ADD Rd, Rs — Single micro-op

Format R: `[00 | op=00000 | Rd(4) | Rs(4) | F | spare(15)]`

The micro-routine for ADD uses IR fields: Rd = IR[24:21], Rs = IR[20:17], F = IR[16].

| Step  | reg_a    | reg_b    | reg_w    | rWE  | alu_op | bMux | wMux | imm | fWE | marLd | marSrc | mdrMem | mdrA | mRd | mWr | mSz | sExt | pcSrc | sysCyc | sysWE | luSt | luSel | luRB | next_addr | bCond |
|-------|----------|----------|----------|------|--------|------|------|-----|-----|-------|--------|--------|------|-----|-----|-----|------|-------|--------|-------|------|-------|------|-----------|-------|
| add-0 | IR[24:21]| IR[20:17]| IR[24:21]| ~F   | 0000   | 0    | 0    | —   | 1   | 0     | —      | 0      | 0    | 0   | 0   | —   | —    | 001   | 0      | 0     | 0    | —     | 0    | fetch-0   | 001   |

**Signal trace:**
1. `reg_a_sel = IR[24:21]` (Rd) → A-bus carries Rd value
2. `reg_b_sel = IR[20:17]` (Rs), `b_mux_sel = 0` (register) → B-bus carries Rs value
3. `alu_op = 0000` (ADD) → R-bus = A + B
4. `w_mux_sel = 0` (R-bus) → write data = ALU result
5. `reg_w_sel = IR[24:21]` (Rd), `reg_w_en = ~IR[16]` → Rd updated (unless F=1 for CMP)
6. `flag_w_en = 1` → SR flags updated from ALU (Z, N, C, V)
7. `pc_src = 001` (PC+4) → advance to next instruction
8. `branch_cond = 001` (always), `next_addr = fetch-0` → return to fetch

**Issue found:** `reg_w_en` depends on the **F bit** from IR[16]. The micro-word has a static `reg_w_en` field, but the same micro-routine must handle both `ADD Rd, Rs` (F=0, write Rd) and `CMP Rd, Rs` (F=1, don't write). **Options:**
- (a) Separate micro-routines for F=0 and F=1 variants (wastes ROM space)
- (b) Hardware AND: actual write enable = `reg_w_en & ~IR[16]` when in Format R. The micro-word sets `reg_w_en=1`, and the F bit gates it.

Option (b) is clearly better — single micro-word handles both variants. This requires a small AND gate between the micro-word's `reg_w_en` and `~IR[16]`, gated on Format R decode. The same pattern applies to all Format R ALU ops.

---

## 2. LDW Rd, [Rb + offset] — Multi-step memory load

Format M: `[10 | L=1 | sz=10 | SE=0 | Rd(4) | Rb(4) | offset16(16)]`

IR fields: Rd = IR[25:22], Rb = IR[21:18], offset16 = IR[17:2].

| Step  | reg_a    | reg_b | reg_w    | rWE | alu_op | bMux | wMux | imm  | fWE | marLd | marSrc | mdrMem | mdrA | mRd | mWr | mSz | sExt | pcSrc | sysCyc | sysWE | luSt | luSel | luRB | next_addr | bCond |
|-------|----------|-------|----------|-----|--------|------|------|------|-----|-------|--------|--------|------|-----|-----|-----|------|-------|--------|-------|------|-------|------|-----------|-------|
| ldw-0 | IR[21:18]| —     | —        | 0   | 0000   | 1    | —    | 01   | 0   | 1     | 0      | 0      | 0    | 0   | 0   | —   | —    | 000   | 0      | 0     | 0    | —     | 0    | ldw-1     | 000   |
| ldw-1 | —        | —     | —        | 0   | —      | —    | —    | —    | 0   | 0     | —      | 1      | 0    | 1   | 0   | 10  | 0    | 000   | 0      | 0     | 0    | —     | 0    | ldw-1     | 010   |
| ldw-2 | —        | —     | IR[25:22]| 1   | —      | —    | 1    | —    | 0   | 0     | —      | 0      | 0    | 0   | 0   | —   | —    | 001   | 0      | 0     | 0    | —     | 0    | fetch-0   | 001   |

**Signal trace:**

*ldw-0 — compute effective address:*
1. `reg_a_sel = IR[21:18]` (Rb) → A-bus carries base register value
2. `b_mux_sel = 1` (immediate), `imm_mode = 01` (sign-extend) → B-bus = sign_extend(offset16)
3. `alu_op = 0000` (ADD) → R-bus = Rb + sign_extend(offset16) = effective address
4. `mar_load = 1`, `mar_src = 0` (R-bus) → MAR ← effective address
5. `reg_w_en = 0`, `flag_w_en = 0` → no register or flag side effects
6. `pc_src = 000` (hold) → PC unchanged
7. `branch_cond = 000` (sequential) → micro-PC increments to ldw-1

*ldw-1 — memory read (stall loop):*
1. `mem_read = 1`, `mem_size = 10` (word) → initiate cache/bus read at MAR address
2. `mdr_load_mem = 1` → MDR will latch data when ready
3. `branch_cond = 010` (if stalled), `next_addr = ldw-1` → loop while cache busy
4. When cache signals ready, stall clears → micro-PC advances to ldw-2

*ldw-2 — write back:*
1. `w_mux_sel = 1` (MDR) → write data = memory value from MDR
2. `reg_w_sel = IR[25:22]` (Rd), `reg_w_en = 1` → Rd ← MDR
3. `pc_src = 001` (PC+4) → advance to next instruction
4. `branch_cond = 001` (always), `next_addr = fetch-0` → return to fetch

**Issue found:** The stall mechanism in ldw-1 reuses `branch_cond = 010` (if stalled), but the "stalled" condition was defined for long-latency units (MUL/DIV/FPU). Memory stalls are a different source. **Options:**
- (a) Add a memory-busy stall source. If we merged `stall_sel` into the branch condition, we need a way to distinguish memory stalls from LU stalls.
- (b) The cache/bus interface asserts a generic `busy` signal that the sequencer already checks. The `branch_cond = 010` condition checks a unified busy line that OR's together: `cache_busy | lu_busy[lu_sel]`. This is the simplest approach.

Option (b) works if the microcode never issues a memory op and an LU op simultaneously (it doesn't — they use different micro-ops). The unified busy line means "whatever you asked for isn't done yet."

---

## 3. BEQ offset — Conditional branch

Format B: `[11 | cond=0001 | offset22(22)]`

Branch target: `PC + 4 + sign_extend(offset22 << 2)`

| Step  | reg_a    | reg_b | reg_w    | rWE | alu_op | bMux | wMux | imm  | fWE | marLd | marSrc | mdrMem | mdrA | mRd | mWr | mSz | sExt | pcSrc | sysCyc | sysWE | luSt | luSel | luRB | next_addr | bCond |
|-------|----------|-------|----------|-----|--------|------|------|------|-----|-------|--------|--------|------|-----|-----|-----|------|-------|--------|-------|------|-------|------|-----------|-------|
| beq-0 | -        | -     | -        | 0   | -      | -    | -    | -    | 0   | 0     | -      | 0      | 0    | -   | -   | -   | -    | 010   | 0      | 0     | 0    | -     | 0    | fetch-0   | 011   |

**Signal trace:**

*Beq-0 — branch if equal:*
1. `pc_src = 010` (PC+offset) → proceed to next instruction (offset depends on `branch_cond`, +4 or immediate)
2. `branch_cond = 011` (if ISA condition true), `next_addr = fetch-0` → return to fetch

---

## 4. Interrupt Entry — Exception micro-sequence

The datapath spec defines this 6-step sequence:
```
micro-op 0: Save SR   → MDR = SR, MAR = KSP - 4, mem_write
micro-op 1: Save PC   → MDR = PC, MAR = KSP - 8, mem_write
micro-op 2: Update SP  → KSP = KSP - 8
micro-op 3: Set mode   → SR.S = 1, SR.I = 0, swap to KSP
micro-op 4: Load vector → MAR = vector_table_base + (vector_num × 4), mem_read
micro-op 5: Jump       → PC = MDR (vector address)
```

Attempting to implement this against the actual micro-word reveals **five missing datapath capabilities**. The sequence below is a corrected version that accounts for these gaps.

### Hardware Pre-Actions (not microcode — triggers atomically when interrupt is recognized)

The following must happen in hardware before the micro-routine runs:

1. **Latch shadow registers:** `shadow_SR ← SR`, `shadow_PC ← PC` (return address; faulting PC for exceptions)
2. **Mode switch:** `SR.S ← 1`, `SR.I ← 0`
3. **SP bank swap:** R14 now reads/writes KSP (user SP banked away)
4. **Latch vector number:** `vec_num ← source` (from priority encoder for IRQs, hardwired per exception type)

**Why hardware, not microcode:** The original spec's micro-op 3 sets S=1/I=0, but micro-ops 0-1 already need KSP. If the mode switch happens in microcode, we'd need a separate path to access KSP while still in user mode. Doing it atomically in hardware (like the 68000 and PDP-11) eliminates this ordering problem and is simpler for discrete — it's just a few flip-flops clocked by the "enter exception" signal.

### Missing Datapath Capabilities

Writing the micro-ops reveals that the current micro-word cannot express this sequence. The following are needed:

**Issue 6 — No A-bus source mux for internal registers.** The micro-word's `reg_a_sel` reads from the register file, but interrupt entry needs `shadow_SR`, `shadow_PC`, and `vec_num` on the A-bus. **Proposed fix:** Add `a_src[1:0]` (2 bits): `00`=register file, `01`=shadow_SR, `10`=shadow_PC, `11`=vector_addr (vec_num << 2, pre-shifted by hardware). This replaces the implicit "A-bus always comes from register file" assumption.

**Issue 7 — No microcode-accessible constants.** The immediate extractor reads from IR, but during interrupt entry IR holds the interrupted instruction (or is stale). The microcode needs the constants 4 and 8 for stack pointer adjustment. **Proposed fix:** Add `const_sel[1:0]` (2 bits) as an alternative B-bus source: `00`=normal (reg/imm from IR), `01`=4, `10`=8, `11`=reserved. Gated by a new `b_mux_sel` encoding: expand to 2 bits (`00`=register, `01`=IR immediate, `10`=micro-constant).

**Issue 8 — No SR modification signals.** The micro-word has `flag_w_en` (update NZCV from ALU) but no way to set individual SR bits (S, I). The hardware pre-action handles this for interrupt entry, but RTI (return from interrupt) will need to restore SR from the stack — which means loading a full SR value from MDR. **Proposed fix:** Add `sr_load` (1 bit): load SR from the W-mux output (same path as register writeback). RTI pops SR from the stack into MDR, then `w_mux_sel=1` (MDR) with `sr_load=1` writes it to SR.

**Issue 9 — Original 6-micro-op count assumes parallel MDR+MAR load.** The spec's micro-op 0 says "MDR = SR, MAR = KSP - 4" in a single step. But MDR loads from A-bus (`mdr_load_a`), and computing KSP-4 also needs A-bus (for `reg_a_sel=R14`). Only one value can be on A-bus per cycle. The sequence must be split into separate address-compute and data-drive micro-ops.

**Issue 10 — Stall loop placement.** Each memory write may stall if the cache/bus isn't ready. The original spec shows `mem_write` as instantaneous, but we need stall loops (like in LDW) after each write.

### Corrected Micro-Routine (7 micro-ops + 2 stall loops)

Using the proposed new signals: `a_src[1:0]`, `b_mux_sel` expanded to 2 bits, `sr_load`. The `pc_src` encoding uses 3 bits per the proposed fix from issue 1 (`100`=MDR).

| Step  | a_src | reg_a | reg_b | reg_w | rWE | alu_op | bMux | wMux | imm  | fWE | marLd | marSrc | mdrMem | mdrA | mRd | mWr | mSz | sExt | pcSrc | sysCyc | sysWE | luSt | luSel | luRB | srLd | next_addr | bCond |
|-------|-------|-------|-------|-------|-----|--------|------|------|------|-----|-------|--------|--------|------|-----|-----|-----|------|-------|--------|-------|------|-------|------|------|-----------|-------|
| int-0 | 00    | R14   | —     | —     | 0   | 0001   | 10   | —    | —    | 0   | 1     | 0      | 0      | 0    | 0   | 0   | —   | —    | 000   | 0      | 0     | 0    | —     | 0    | 0    | int-1     | 000   |
| int-1 | 01    | —     | —     | —     | 0   | —      | —    | —    | —    | 0   | 0     | —      | 0      | 1    | 0   | 1   | 10  | —    | 000   | 0      | 0     | 0    | —     | 0    | 0    | int-1     | 010   |
| int-2 | 00    | R14   | —     | R14   | 1   | 0001   | 10   | 0    | —    | 0   | 1     | 0      | 0      | 0    | 0   | 0   | —   | —    | 000   | 0      | 0     | 0    | —     | 0    | 0    | int-3     | 000   |
| int-3 | 10    | —     | —     | —     | 0   | —      | —    | —    | —    | 0   | 0     | —      | 0      | 1    | 0   | 1   | 10  | —    | 000   | 0      | 0     | 0    | —     | 0    | 0    | int-3     | 010   |
| int-4 | 11    | —     | —     | —     | 0   | 1000   | —    | —    | —    | 0   | 1     | 0      | 0      | 0    | 0   | 0   | —   | —    | 000   | 0      | 0     | 0    | —     | 0    | 0    | int-5     | 000   |
| int-5 | —     | —     | —     | —     | 0   | —      | —    | —    | —    | 0   | 0     | —      | 1      | 0    | 1   | 0   | 10  | —    | 000   | 0      | 0     | 0    | —     | 0    | 0    | int-5     | 010   |
| int-6 | —     | —     | —     | —     | 0   | —      | —    | —    | —    | 0   | 0     | —      | 0      | 0    | 0   | 0   | —   | —    | 100   | 0      | 0     | 0    | —     | 0    | 0    | fetch-0   | 001   |

**Signal trace:**

*int-0 — compute KSP - 4 → MAR:*
1. `a_src = 00` (register file), `reg_a_sel = R14` (KSP, post-bank-swap) → A-bus = KSP
2. `b_mux_sel = 10` (micro-constant = 4), `alu_op = 0001` (SUB) → R-bus = KSP - 4
3. `mar_load = 1`, `mar_src = 0` (R-bus) → MAR ← KSP - 4
4. Sequential → int-1

*int-1 — drive shadow_SR to MDR, write to memory (stall loop):*
1. `a_src = 01` (shadow_SR) → A-bus = old SR value
2. `mdr_load_a = 1` → MDR ← old SR
3. `mem_write = 1`, `mem_size = 10` (word) → write MDR to [MAR] = [KSP - 4]
4. `branch_cond = 010` (if stalled), `next_addr = int-1` → loop until write completes
5. When done → int-2

*int-2 — compute KSP - 8 → MAR, also update KSP:*
1. `a_src = 00`, `reg_a_sel = R14` (KSP) → A-bus = KSP (still original value)
2. `b_mux_sel = 10` (micro-constant = 8), `alu_op = 0001` (SUB) → R-bus = KSP - 8
3. `mar_load = 1` → MAR ← KSP - 8
4. `reg_w_sel = R14`, `reg_w_en = 1`, `w_mux_sel = 0` (R-bus) → KSP ← KSP - 8
5. Sequential → int-3

*int-3 — drive shadow_PC to MDR, write to memory (stall loop):*
1. `a_src = 10` (shadow_PC) → A-bus = old PC (return address)
2. `mdr_load_a = 1` → MDR ← old PC
3. `mem_write = 1`, `mem_size = 10` (word) → write MDR to [MAR] = [KSP - 8]
4. `branch_cond = 010` (if stalled) → loop until write completes
5. When done → int-4

*int-4 — load vector address → MAR:*
1. `a_src = 11` (vector_addr = vec_num << 2, hardware pre-shifted) → A-bus = vector table offset
2. `alu_op = 1000` (PASS_A) → R-bus = vector address (vector table base is 0x0000_0000, so offset = address)
3. `mar_load = 1` → MAR ← vector address
4. Sequential → int-5

*int-5 — memory read (stall loop):*
1. `mem_read = 1`, `mem_size = 10` (word) → read handler address from vector table
2. `mdr_load_mem = 1` → MDR will latch vector entry
3. `branch_cond = 010` → loop while busy
4. When done → int-6

*int-6 — jump to handler:*
1. `pc_src = 100` (MDR) → PC ← handler address from vector table
2. `branch_cond = 001` (always), `next_addr = fetch-0` → begin executing handler

### Note on int-2: KSP update timing

In int-2, we read KSP (R14) on the A-bus and write the decremented value back to R14 in the same micro-op. This is safe because register file reads happen at the start of the cycle (combinational) and writes happen at the end (clocked). This is standard register-file timing and works in both FPGA and discrete (the read port is async, the write port is edge-triggered).

However, this means int-0 must execute before int-2 — both read the *original* KSP value, and int-2 overwrites it. The ordering is correct as written.

### Note on micro-constant encoding

The `b_mux_sel = 10` (micro-constant) approach proposed above uses a 2-bit `b_mux_sel` and encodes the constant in a separate `const_sel` field. An alternative: since we only need the constants 4 and 8 for interrupt entry (and RTI for the reverse), we could encode the constant directly in the expanded `b_mux_sel`: `00`=register, `01`=IR immediate, `10`=constant 4, `11`=constant 8. This avoids adding a separate `const_sel` field at the cost of only two hardwired constants. For Penumbra this is probably sufficient — the only microcode-level constants needed are for stack frame adjustment.

---

## 5. RTI — Return from Interrupt

RTI is privileged (R-format, op=10110). It reverses the interrupt entry sequence: pops saved_PC and saved_SR from the kernel stack, restores full SR (privilege level, interrupt enable, condition flags), and resumes execution. If the restored SR.S=0, SP banking swaps R14 from KSP back to USP.

### Stack Frame Layout

Interrupt entry pushed in this order (KSP decrements):
```
[KSP + 0] → saved_PC    (pushed second, lower address)
[KSP + 4] → saved_SR    (pushed first, higher address)
```

### Ordering Constraint

`sr_load` (step 6) may change SR.S from 1→0, triggering SP bank swap (R14 switches from KSP to USP). All reads from KSP must complete **before** `sr_load`. Additionally, KSP must be adjusted (+=8) before the swap, so the kernel stack is clean.

### Key Trick: Parallel PC Load + MAR Compute (rti-3)

`pc_src=100` (load PC from MDR) uses the PC unit's MDR input path. Simultaneously, the ALU can compute `KSP + 4` on the A/B/R-bus path for MAR. These are independent datapath resources — no conflict. This saves one micro-op.

### Micro-Routine (8 micro-ops including PRIV check)

| Step  | a_src | reg_a | reg_b | reg_w | rWE | alu_op | bMux | wMux | imm | fWE | srLd | marLd | mdrMem | mdrA | mRd | mWr | mSz | sExt | pcSrc | sysCyc | sysWE | luOp | bCond | fwdOff |
|-------|-------|-------|-------|-------|-----|--------|------|------|-----|-----|------|-------|--------|------|-----|-----|-----|------|-------|--------|-------|------|-------|--------|
| rti-0 | —     | —     | —     | —     | 0   | —      | —    | —    | —   | 0   | 0    | 0     | 0      | 0    | 0   | 0   | —   | —    | 000   | 0      | 0     | 00   | 101   | —      |
| rti-1 | 00    | R14   | —     | —     | 0   | 1000   | —    | —    | —   | 0   | 0    | 1     | 0      | 0    | 0   | 0   | —   | —    | 000   | 0      | 0     | 00   | 000   | —      |
| rti-2 | —     | —     | —     | —     | 0   | —      | —    | —    | —   | 0   | 0    | 0     | 1      | 0    | 1   | 0   | 10  | —    | 000   | 0      | 0     | 00   | 010   | —      |
| rti-3 | 00    | R14   | —     | —     | 0   | 0000   | 10   | —    | —   | 0   | 0    | 1     | 0      | 0    | 0   | 0   | —   | —    | 100   | 0      | 0     | 00   | 000   | —      |
| rti-4 | —     | —     | —     | —     | 0   | —      | —    | —    | —   | 0   | 0    | 0     | 1      | 0    | 1   | 0   | 10  | —    | 000   | 0      | 0     | 00   | 010   | —      |
| rti-5 | 00    | R14   | —     | R14   | 1   | 0000   | 11   | 0    | —   | 0   | 0    | 0     | 0      | 0    | 0   | 0   | —   | —    | 000   | 0      | 0     | 00   | 000   | —      |
| rti-6 | —     | —     | —     | —     | 0   | —      | 0    | 1    | —   | 0   | 1    | 0     | 0      | 0    | 0   | 0   | —   | —    | 000   | 0      | 0     | 00   | 000   | —      |
| rti-7 | —     | —     | —     | —     | 0   | —      | —    | —    | —   | 0   | 0    | 0     | 0      | 0    | 0   | 0   | —   | —    | 000   | 0      | 0     | 00   | 001   | —      |

### Signal Trace

*rti-0 — privilege check:*
1. `branch_cond = PRIV` (101): if SR.S=0 → privilege violation (vector 3); if SR.S=1 → SEQ
2. All other signals inactive (don't-care / zero for enables)

*rti-1 — compute address of saved_PC → MAR:*
1. `a_src = 00`, `reg_a_sel = R14` (KSP) → A-bus = KSP
2. `alu_op = 1000` (PASS_A) → R-bus = KSP
3. `mar_load = 1` → MAR ← KSP (address of saved_PC)
4. `branch_cond = SEQ` → micro-PC++

*rti-2 — read saved_PC from memory (stall loop):*
1. `mem_read = 1`, `mem_size = 10` (word) → read from [KSP]
2. `mdr_load_mem = 1` → MDR will latch saved_PC when ready
3. `branch_cond = STALL` → loop while busy (or fault → exception)
4. When done → MDR = saved_PC

*rti-3 — load PC from MDR, compute address of saved_SR → MAR (parallel):*
1. `pc_src = 100` (MDR) → PC ← saved_PC (loaded from MDR via PC unit)
2. `a_src = 00`, `reg_a_sel = R14` (KSP) → A-bus = KSP
3. `b_mux_sel = 10` (const 4), `alu_op = 0000` (ADD) → R-bus = KSP + 4
4. `mar_load = 1` → MAR ← KSP + 4 (address of saved_SR)
5. Both operations use independent datapath resources — no conflict
6. `branch_cond = SEQ` → micro-PC++

*rti-4 — read saved_SR from memory (stall loop):*
1. `mem_read = 1`, `mem_size = 10` (word) → read from [KSP + 4]
2. `mdr_load_mem = 1` → MDR will latch saved_SR when ready
3. `branch_cond = STALL` → loop while busy (or fault → exception)
4. When done → MDR = saved_SR

*rti-5 — pop stack frame (KSP += 8):*
1. `a_src = 00`, `reg_a_sel = R14` (KSP) → A-bus = KSP
2. `b_mux_sel = 11` (const 8), `alu_op = 0000` (ADD) → R-bus = KSP + 8
3. `reg_w_sel = R14`, `reg_w_en = 1`, `w_mux_sel = 0` (R-bus) → KSP ← KSP + 8
4. **Must happen before rti-6:** if sr_load changes S=1→0, R14 becomes USP
5. `branch_cond = SEQ` → micro-PC++

*rti-6 — restore SR from saved value:*
1. `w_mux_sel = 1` (MDR) → W-mux output = saved_SR from MDR
2. `sr_load = 1` → SR ← saved_SR (restores S, I, NZCV flags)
3. If restored SR.S=0: hardware swaps SP banking (R14 → USP). KSP is already adjusted (rti-5), so the kernel stack is clean.
4. `branch_cond = SEQ` → micro-PC++

*rti-7 — resume execution:*
1. `branch_cond = FETCH` → hand off to fetch unit
2. PC was already loaded in rti-3 with saved_PC
3. Fetch unit reads I-cache at restored PC, dispatches next instruction
4. If restored SR.I=1, interrupts are now enabled for the next instruction

### Verification: FPU Emulation Return Path

Walkthrough of a complete FPU emulation cycle:

```
1. User code: FADD R1, R2              (PC = 0x1000)
2. No FPU hardware → illegal instruction exception (vector 2)
3. Hardware pre-actions: shadow_SR ← SR (S=0,I=1,...), shadow_PC ← 0x1000
   SR.S ← 1, SR.I ← 0, swap to KSP
4. Exception entry microcode: push SR at [KSP-4], push PC(0x1000) at [KSP-8], KSP -= 8
5. Fetch vector[2], PC ← handler address

--- Emulation handler (software) ---
6. Save registers, read instruction at [saved_PC] = [KSP+0] → decodes FADD R1, R2
7. Read R1, R2 from saved context on stack
8. Software FP add, write result to saved R1 on stack
9. Advance saved_PC: LDW R3, [KSP+0]; INC R3, #4; STW R3, [KSP+0]
   (saved_PC now = 0x1004, the instruction AFTER FADD)
10. Restore registers, execute RTI

--- RTI microcode ---
11. rti-0: PRIV check (we're in supervisor mode) → pass
12. rti-1: MAR = KSP
13. rti-2: mem_read [KSP+0] → MDR = 0x1004 (advanced saved_PC)
14. rti-3: PC ← 0x1004, MAR = KSP + 4
15. rti-4: mem_read [KSP+4] → MDR = saved_SR (S=0, I=1, user flags)
16. rti-5: KSP += 8 (pop frame)
17. rti-6: sr_load → SR restored (S=0, I=1), SP swaps to USP
18. rti-7: FETCH → fetch unit reads I-cache at PC=0x1004
    → executes the instruction after FADD ✓
```

**Result:** User code continues after the emulated FP instruction as if the FPU were present. The return address was advanced by the handler software, not the hardware. RTI blindly restores whatever PC and SR are on the stack.

### Verification: Page Fault + Demand Paging Return Path

```
1. User code: LDW R1, [R2 + #0]        (PC = 0x2000, page not resident)
2. LDW micro-routine: ldw-0 computes EA, ldw-1 issues mem_read
3. D-cache/MMU: TLB miss → fault=1, fault_vector=4
4. STALL resolves with fault → exception via fetch unit
5. Hardware pre-actions: shadow_PC ← 0x2000 (PC not yet advanced)
6. Exception entry: push SR, push PC(0x2000), load vector[4], jump to page fault handler

--- Page fault handler (software) ---
7. Read faulting address from MMU fault register (via MFSYS)
8. Page in from disk, update page table, load TLB entry
9. RTI (saved_PC = 0x2000, unchanged — we want to retry)

--- RTI microcode ---
10. rti-3: PC ← 0x2000 (the LDW instruction itself)
11. rti-6: sr_load restores user mode
12. rti-7: FETCH → re-executes LDW at 0x2000
    → TLB now has the entry, mem_read succeeds ✓
```

**Result:** The faulting instruction is transparently retried after the page is loaded.

---

## Revised Micro-Word Format (48 bits)

All issues resolved. Fields listed from MSB to LSB with exact bit positions.

### Field Table

| Bits | Field | Width | Description |
|------|-------|-------|-------------|
| 48:47 | `a_src[1:0]` | 2 | A-bus source: 00=register file, 01=shadow_SR, 10=shadow_PC, 11=vector_addr |
| 46:43 | `reg_a_sel[3:0]` | 4 | Register file read port A address (used when a_src=00) |
| 42:39 | `reg_b_sel[3:0]` | 4 | Register file read port B address |
| 38:35 | `reg_w_sel[3:0]` | 4 | Register file write port address |
| 34 | `reg_w_en` | 1 | Register file write enable (hardware-gated by Format R F bit) |
| 33:30 | `alu_op[3:0]` | 4 | ALU operation |
| 29:28 | `b_mux_sel[1:0]` | 2 | B-bus source: 00=register port B, 01=IR immediate, 10=const 4, 11=const 8 |
| 27 | `w_mux_sel` | 1 | Write-back source: 0=R-bus, 1=MDR |
| 26:25 | `imm_mode[1:0]` | 2 | IR immediate handling: 00=zero-extend, 01=sign-extend, 10=shift-left-16 |
| 24 | `flag_w_en` | 1 | Update SR condition flags (NZCV) from ALU |
| 23 | `sr_load` | 1 | Load full SR from W-mux output (for RTI) |
| 22 | `mar_load` | 1 | Load MAR from R-bus (D-cache/bus address) |
| 21 | `mdr_load_mem` | 1 | Load MDR from D-cache/memory (read data) |
| 20 | `mdr_load_a` | 1 | Load MDR from A-bus (for stores) |
| 19 | `mem_read` | 1 | Initiate D-cache/memory read |
| 18 | `mem_write` | 1 | Initiate D-cache/memory write |
| 17:16 | `mem_size[1:0]` | 2 | Access size: 00=byte, 01=half, 10=word |
| 15 | `sign_ext` | 1 | Sign-extend sub-word load result |
| 14:12 | `pc_src[2:0]` | 3 | PC source: 000=hold, 001=PC+4, 010=PC+offset, 011=A-bus, 100=MDR |
| 11 | `sys_cycle` | 1 | System register bus cycle |
| 10 | `sys_we` | 1 | System register write enable |
| 9:8 | `lu_op[1:0]` | 2 | Long-latency unit: 00=none, 01=start, 10=read result to R-bus, 11=(reserved). Unit selected by IR decode. |
| 7:5 | `branch_cond[2:0]` | 3 | Micro-sequencer control (see sequencer section) |
| 4:2 | `fwd_offset[2:0]` | 3 | Forward skip offset (used only when branch_cond=SKIP) |
| 1:0 | (spare) | 2 | Reserved for future use |

**Total: 48 bits**

### Changes from Original (55-bit) Format

| Change | Bits saved | Bits added | Rationale |
|--------|-----------|------------|-----------|
| Remove `mar_src` | 1 | — | I-cache permanently wired to PC; MAR is D-cache only, always loads from R-bus |
| Remove `stall_sel[1:0]` | 2 | — | Merged; stall checks unified busy line |
| Remove `pc_mdr_load` | 1 | — | Folded into expanded `pc_src` (encoding 100=MDR) |
| Replace `next_addr[9:0]` with `fwd_offset[2:0]` | 7 | — | No absolute/backward jumps needed; linear micro-routines with stall-hold and fetch-handoff |
| Expand `pc_src` 2→3 bits | — | 1 | Absorbs `pc_mdr_load`, adds MDR source for exception vector jump |
| Expand `b_mux_sel` 1→2 bits | — | 1 | Micro-constants (4, 8) for stack adjustment without IR |
| Add `a_src[1:0]` | — | 2 | A-bus source mux for shadow_SR, shadow_PC, vector_addr in exception entry |
| Add `sr_load` | — | 1 | Load full SR from datapath for RTI |
| Replace `lu_start`+`lu_sel`+`lu_to_rbus` (5 bits) with `lu_op[1:0]` (2 bits) | 3 | — | Combined MUL/DIV unit; unit selection by IR decode, not micro-word |
| Add PRIV branch_cond | — | 0 | Uses previously available slot 101; no new bits |
| **Net** | **-14** | **+5** | **55 → 48 bits (+ 2 spare)** |

### Discrete Implementation

48 bits = 6 byte-wide ROMs × 8 bits exactly. Clean fit — no spare bits, no waste. If the 2 spare bits are eventually used (reaching 50), a 7th ROM provides room to grow to 56 bits.

On ECP5: 256 entries × 48 bits = 12 Kbit (1 EBR, well within a single 18 Kbit block).

---

## Summary of Issues Found

| # | Issue | Status | Resolution |
|---|-------|--------|------------|
| 1 | Micro-word fields sum to 55, not 52 | **Resolved** | Revised format is 48 bits (+ 2 spare) after merges, sequencer redesign, and LU consolidation |
| 2 | No `ir_load` signal for instruction latch | **Resolved** | Hardwired fetch unit controls IR directly; no micro-word signal needed |
| 3 | `reg_w_en` must be gated by Format R `F` bit | **Accepted** | Hardware AND: `actual_w_en = reg_w_en & ~(format_R & IR[16])` |
| 4 | Memory stalls vs LU stalls share STALL condition | **Accepted** | Unified `busy` line: `cache_busy \| lu_busy[lu_sel]`; never overlap |
| 5 | `branch_cond` 011/100 redefined for conditional pc_src | **Accepted** | BRT/BRF gate pc_src and hand off to fetch unit; single-micro-op Bcc |
| 6 | No A-bus source for shadow_SR, shadow_PC, vector_addr | **Resolved** | Added `a_src[1:0]` field |
| 7 | No microcode-accessible constants | **Resolved** | Expanded `b_mux_sel` to 2 bits with hardwired 4 and 8 |
| 8 | No SR load-from-datapath for RTI | **Resolved** | Added `sr_load` field |
| 9 | Original 6-step interrupt sequence infeasible | **Resolved** | Corrected to 7 micro-ops + stall loops; hardware pre-actions for mode switch |
| 10 | Memory write stalls not accounted for | **Resolved** | Stall loops added (same STALL mechanism as reads) |
| 11 | Fetch cycle was 4 micro-ops in ROM | **Resolved** | Hardwired fetch unit; 0 micro-ops in ROM; upgradeable to prefetch |
| 12 | `mar_src` unnecessary with split I/D cache | **Resolved** | Removed; MAR serves D-cache only, always loads from R-bus |
| 13 | Separate MUL/DIV/FPU units waste micro-word bits | **Resolved** | Combined MUL/DIV into single integer unit; `lu_op[1:0]` replaces 5 bits of LU control; unit selection by IR decode |
| 14 | Privilege check mechanism undefined | **Resolved** | `branch_cond=PRIV` (101): checks SR.S, triggers exception via fetch unit on violation |
| 15 | FPU-absent trap mechanism | **Resolved** | Different ROM image fills FP opcode entries with illegal instruction exception code; zero runtime overhead |
| 16 | FP condition branches and GPR↔FPR moves | **Resolved** | FP-in-GPRs: FPU uses A/B/R buses like integer LU; FP compare sets NZCV via `flag_w_en`; normal Bcc works; no separate FP register file or move instructions |
| 17 | Page faults during instruction execution have no trigger path | **Resolved** | Extended STALL: 3-way resolution (busy/done/fault). D-cache/MMU asserts `mem_fault` + `fault_vector[3:0]` on TLB miss, protection violation, alignment fault, or bus error. Sequencer triggers exception via fetch unit, same as PRIV. `shadow_PC` = faulting instruction (PC not yet advanced). No new micro-word bits needed. |
