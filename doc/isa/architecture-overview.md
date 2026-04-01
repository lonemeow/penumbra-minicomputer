# Penumbra ISA - Architecture Overview

## Design Philosophy

Penumbra is a 32-bit load-store RISC-like architecture. While inspired by the aesthetics and operational feel of 1970s/80s minicomputers like the Data General Eclipse and DEC VAX, the ISA itself is a clean design that avoids the accumulated complexity of those machines.

Key principles:
- **Load-store:** Only load and store instructions access memory; all computation operates on registers
- **Fixed-width instructions:** All instructions are 32 bits wide
- **2-operand format:** Arithmetic/logic instructions use a destructive destination (e.g., `ADD R1, R2` means `R1 = R1 + R2`)
- **Orthogonal design:** Minimize special cases and irregular encodings
- **Consistent immediates:** All immediate fields are 16 bits wide
- **Discrete-friendly:** All architectural choices must be feasible in a future discrete 74xx chip implementation

## Registers

Penumbra has 16 registers addressed by a 4-bit field.

| Register | Name | Description |
|----------|------|-------------|
| R0       | ZERO | Hardwired to zero; writes are discarded |
| R1-R13   | GPR  | General-purpose (13 registers) |
| R14      | SP   | Stack pointer, hardware-banked (see below) |
| R15      | PC   | Program counter (read-only via register file; see below) |

### Zero Register (R0)

R0 always reads as zero. Any instruction that writes to R0 completes normally (including setting flags if applicable) but the result is discarded. This provides several useful pseudo-instructions:

| Pseudo-instruction | Actual encoding | Effect |
|-------------------|-----------------|--------|
| `NOP` | `ADD R0, R0` | No effect |
| `CMP Rs, #0` | `CMP Rs, R0` | Compare register to zero |
| Clear Rd | `MOV Rd, R0` | Rd = 0 |

### Status Register (SR)

The status register is **not** part of the 16-register GPR file. It is a separate hardware register internal to the CPU, accessed via dedicated privileged instructions (`GETSR`, `SETSR`).

SR contains:
- **Condition flags:** Z (zero), N (negative), C (carry), V (overflow)
- **S (supervisor):** Current privilege level (0 = user, 1 = supervisor)
- **I (interrupt enable):** Global interrupt mask

Previous mode is preserved via exception registers (EPC, ESR) at exception entry, not bits within SR. See the datapath specification for the exception model.

Carry convention is **ARM-style** (C = NOT borrow on subtraction). Flags are updated by arithmetic/logic ALU operations, INC, DEC, CMPI, ANDI, TESTI, and MUL/DIV/MOD. MOV, load immediates (LLI, LLIS, LUI), loads, stores, branches, and system instructions do **not** affect flags. See the datapath specification for full flag generation details.

#### SR Bit Layout (32-bit)

```
 31  30  29                          4   3   2   1   0
┌───┬───┬──────── reserved (0) ──────┬───┬───┬───┬───┐
│ S │ I │         0 0 0 ... 0        │ V │ C │ Z │ N │
└───┴───┴────────────────────────────┴───┴───┴───┴───┘
```

| Bit | Field | Description |
|-----|-------|-------------|
| 31 | S | Supervisor mode (1 = supervisor, 0 = user) |
| 30 | I | Interrupt enable (1 = enabled, 0 = masked) |
| 29:4 | — | Reserved, read as zero, ignored on write |
| 3 | V | Overflow flag |
| 2 | C | Carry flag (ARM-style: C = NOT borrow on SUB) |
| 1 | Z | Zero flag |
| 0 | N | Negative flag (= result[31]) |

System bits are in the upper word, condition flags in the lower nibble. Bits [29:4] are reserved for future use and should be written as zero for forward compatibility. This layout is used by GETSR, SETSR, and the exception entry save (EPC/ESR).

The SR contains only CPU-internal state. Registers belonging to other system devices (MMU, interrupt controller, etc.) are accessed via the system register bus — see [System Register Access](#system-register-access) below.

### Stack Pointer Banking

R14 (SP) is hardware-banked between user and supervisor modes. The hardware maintains two physical registers:
- **USP:** User stack pointer, active when S=0
- **SSP:** Supervisor stack pointer, active when S=1

On privilege transitions (interrupt, trap, ERET), the hardware swaps which physical register is visible as R14. The inactive SP is accessible via privileged instructions for context save/restore.

This is the only banked register (B1 model). All other registers are shared across modes.

### Program Counter (R15)

The PC is a **separate hardware register** with its own dedicated adder for branches and increment. It is not part of the main register file or ALU datapath.

**Reads:** When any instruction reads R15 (e.g., as a base register in a load/store), the register file returns the current PC value. This enables PC-relative addressing for loading constants from literal pools.

**Writes:** R15 cannot be written through the ALU or register file write port. PC is modified only by dedicated control flow instructions: branches (B/Bcc/BL), indirect jumps (JMP), exception return (ERET), and exception entry. This eliminates accidental PC writes and simplifies the datapath.

### Link Register

There is no hardware link register. The branch-and-link instruction (`BL`) writes the return address (PC+4) to a designated GPR by software convention (R13). Function return uses `JMP R13`.

## Instruction Encoding

All instructions are 32 bits wide with a 2-bit format prefix selecting one of four formats.

### Format Overview

```
Format R  [00]: Register-register ALU and system operations
Format L  [01]: Immediate operations (load immediate, inc/dec, compare)
Format M  [10]: Memory load/store with register + offset
Format B  [11]: Branch (conditional, unconditional, branch-and-link)
```

### Format R — Register-Register (prefix `00`)

```
 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
[  0  0 |    op (5 bits)    |   Rd (4 bits)  |   Rs (4 bits)  | F|         spare (15 bits)         ]
```

| Field | Bits | Description |
|-------|------|-------------|
| prefix | 31:30 | `00` — Format R |
| op | 29:25 | Operation (5 bits, 32 opcodes) |
| Rd | 24:21 | Destination / first source register |
| Rs | 20:17 | Second source register |
| F | 16 | Flag control: 0 = write result + update flags, 1 = update flags only (no register write) |
| spare | 15:0 | Reserved. Used by WRSYS/RDSYS for device/register fields. |

#### ALU Operations

When F=0, the result is written to Rd and flags are updated. When F=1, flags are updated but Rd is not modified (flag-only mode for CMP/TEST).

| op | Mnemonic | Operation | F=1 equivalent |
|----|----------|-----------|----------------|
| 00000 | `ADD Rd, Rs` | Rd = Rd + Rs | — |
| 00001 | `SUB Rd, Rs` | Rd = Rd - Rs | `CMP Rd, Rs` |
| 00010 | `AND Rd, Rs` | Rd = Rd & Rs | `TEST Rd, Rs` |
| 00011 | `OR Rd, Rs` | Rd = Rd \| Rs | — |
| 00100 | `XOR Rd, Rs` | Rd = Rd ^ Rs | — |
| 00101 | `SHL Rd, Rs` | Rd = Rd << Rs[4:0] | — |
| 00110 | `SHR Rd, Rs` | Rd = Rd >> Rs[4:0] (logical) | — |
| 00111 | `SAR Rd, Rs` | Rd = Rd >> Rs[4:0] (arithmetic) | — |
| 01000 | `MOV Rd, Rs` | Rd = Rs | — |
| 01001 | `NOT Rd, Rs` | Rd = ~Rs | — |
| 01010 | `MUL Rd, Rs` | Rd = Rd × Rs (signed, stalls) | — |
| 01011 | `MULU Rd, Rs` | Rd = Rd × Rs (unsigned, stalls) | — |
| 01100 | `DIV Rd, Rs` | Rd = Rd / Rs (signed, stalls) | — |
| 01101 | `DIVU Rd, Rs` | Rd = Rd / Rs (unsigned, stalls) | — |
| 01110 | `MOD Rd, Rs` | Rd = Rd % Rs (signed, stalls) | — |
| 01111 | `MODU Rd, Rs` | Rd = Rd % Rs (unsigned, stalls) | — |

#### System Operations

System operations use the Format R encoding with the following opcodes. The `Rd`, `Rs`, and `spare` fields are repurposed as needed.

| op | Mnemonic | Description | Field usage |
|----|----------|-------------|-------------|
| 10000 | `WRSYS Rd, #dev, #reg` | Write Rd to system device register | spare[15:12]=dev, spare[11:8]=reg |
| 10001 | `RDSYS Rd, #dev, #reg` | Read system device register to Rd | spare[15:12]=dev, spare[11:8]=reg |
| 10010 | `GETSR Rd` | Read SR to Rd | |
| 10011 | `SETSR Rd` | Write Rd to SR (privileged) | |
| 10100 | `SYSCALL` | System call trap (vector 7) | |
| 10101 | `BREAK` | Debug breakpoint (vector 8) | |
| 10110 | `ERET` | Exception return: restore SR from ESR, PC from EPC (privileged) | |
| 10111 | `ICACHE_INV` | Invalidate entire I-cache (privileged) | |
| 11000 | `JMP Rs` | PC = Rs (indirect jump) | Rs field; assembler alias: `RET` = `JMP R13` |
| 11001 | `EI` | Enable interrupts: SR.I = 1 (delayed — see below) | |
| 11010 | `DI` | Disable interrupts: SR.I = 0 (immediate, privileged) | |
| 11011 | `WRSPR {SPR}, Rd` | Write Rd to SPR (privileged) | SPR in spare[15:12]: 0=ESR, 1=EPC, 2=USP |
| 11100 | `RDSPR Rd, {SPR}` | Read SPR into Rd (privileged) | SPR in spare[15:12]: 0=ESR, 1=EPC, 2=USP |
| 11101-11111 | (reserved) | Future expansion (3 slots) | |

### Format L — Immediate (prefix `01`)

```
 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
[  0  1 |  op (4 bits)  |   Rd (4 bits)  | spare |           imm16 (16 bits)                      ]
```

| Field | Bits | Description |
|-------|------|-------------|
| prefix | 31:30 | `01` — Format L |
| op | 29:26 | Immediate operation (4 bits, 16 opcodes) |
| Rd | 25:22 | Destination register |
| spare | 21:16 | Reserved (6 bits) |
| imm16 | 15:0 | 16-bit immediate value |

All immediate fields are consistently 16 bits wide.

| op | Mnemonic | Operation | Description |
|----|----------|-----------|-------------|
| 0000 | `LLI Rd, #imm16` | Rd = zero_extend(imm16) | Load lower immediate (zero-extended) |
| 0001 | `LLIS Rd, #imm16` | Rd = sign_extend(imm16) | Load lower immediate (sign-extended) |
| 0010 | `LUI Rd, #imm16` | Rd = Rd OR (imm16 << 16) | Load upper immediate (ORs into upper half) |
| 0011 | `INC Rd, #imm16` | Rd = Rd + zero_extend(imm16) | Increment by immediate |
| 0100 | `DEC Rd, #imm16` | Rd = Rd - zero_extend(imm16) | Decrement by immediate |
| 0101 | `CMPI Rd, #imm16` | flags = Rd - sign_extend(imm16) | Compare register to immediate (no write) |
| 0110 | `ANDI Rd, #imm16` | Rd = Rd & zero_extend(imm16) | Bitwise AND with immediate |
| 0111 | `TESTI Rd, #imm16` | flags = Rd & zero_extend(imm16) | Test bits with immediate (no write) |
| 1000 | `SHL Rd, #imm5` | Rd = Rd << imm[4:0] | Shift left by immediate (0–31) |
| 1001 | `SHR Rd, #imm5` | Rd = Rd >> imm[4:0] (logical) | Shift right by immediate (0–31) |
| 1010 | `SAR Rd, #imm5` | Rd = Rd >> imm[4:0] (arithmetic) | Arithmetic shift right by immediate (0–31) |
| 1011–1111 | (reserved) | Future expansion (5 slots) | |

#### Loading 32-bit Constants

```asm
; Small positive constant (0 to 65535) — 1 instruction:
LLI   R1, #42              ; R1 = 0x0000002A

; Small negative constant (-32768 to -1) — 1 instruction:
LLIS  R1, #-1              ; R1 = 0xFFFFFFFF

; Full 32-bit constant — 2 instructions:
LLI   R1, #0x5678          ; R1 = 0x00005678
LUI   R1, #0x1234          ; R1 = 0x12345678

; Upper-half-only constant — 2 instructions:
MOV   R1, R0               ; R1 = 0 (clear, since LUI ORs)
LUI   R1, #0xFF00          ; R1 = 0xFF000000
```

### Format M — Memory Load/Store (prefix `10`)

```
 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
[  1  0 | L |  sz  | SE|   Rd (4 bits)  |   Rb (4 bits)  |           offset16 (16 bits)             ]
```

| Field | Bits | Description |
|-------|------|-------------|
| prefix | 31:30 | `10` — Format M |
| L | 29 | Load/store: 1 = load, 0 = store |
| sz | 28:27 | Size: 00 = byte, 01 = halfword, 10 = word |
| SE | 26 | Sign-extend on load: 1 = sign-extend, 0 = zero-extend (ignored for stores and word loads) |
| Rd | 25:22 | Register to load into / store from |
| Rb | 21:18 | Base address register |
| offset16 | 17:2 | 16-bit signed offset (byte-addressed) |

Wait — 2+1+2+1+4+4+16 = 30 bits. With 32-bit instructions, bits 1:0 are spare.

Effective address: `EA = Rb + sign_extend(offset16)`

Offset range: -32768 to +32767 bytes. Covers any struct field offset, stack frame variable, or array element within ±32 KB of the base.

#### Load/Store Encoding Summary

| L | sz | SE | Mnemonic | Operation |
|---|----|----|----------|-----------|
| 1 | 10 | 0 | `LDW Rd, [Rb + off]` | Rd = mem32[EA] |
| 1 | 01 | 0 | `LDH Rd, [Rb + off]` | Rd = zero_extend(mem16[EA]) |
| 1 | 01 | 1 | `LDHS Rd, [Rb + off]` | Rd = sign_extend(mem16[EA]) |
| 1 | 00 | 0 | `LDB Rd, [Rb + off]` | Rd = zero_extend(mem8[EA]) |
| 1 | 00 | 1 | `LDBS Rd, [Rb + off]` | Rd = sign_extend(mem8[EA]) |
| 0 | 10 | x | `STW Rd, [Rb + off]` | mem32[EA] = Rd |
| 0 | 01 | x | `STH Rd, [Rb + off]` | mem16[EA] = Rd[15:0] |
| 0 | 00 | x | `STB Rd, [Rb + off]` | mem8[EA] = Rd[7:0] |

### Format B — Branch (prefix `11`)

```
 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
[  1  1 |   cond (4 bits)  |                  offset22 (22 bits, signed)                            ]
```

| Field | Bits | Description |
|-------|------|-------------|
| prefix | 31:30 | `11` — Format B |
| cond | 29:26 | Branch condition (4 bits, 16 conditions) |
| offset22 | 25:4 | 22-bit signed offset in words (shifted left 2 for byte address) |

Branch target: `PC + sign_extend(offset22 << 2)`

The offset is relative to the branch instruction itself (not PC+4). The assembler encodes `offset22 = (target - PC) >> 2`. This avoids an extra adder stage in hardware — important for the discrete build.

Range: ±8 MB from the branch instruction. Sufficient to reach anywhere in the 32 MB physical RAM.

#### Condition Codes

| cond | Mnemonic | Meaning | Flags tested |
|------|----------|---------|-------------|
| 0000 | `B` (AL) | Always (unconditional) | — |
| 0001 | `BEQ` | Equal / zero | Z=1 |
| 0010 | `BNE` | Not equal | Z=0 |
| 0011 | `BCS` / `BHS` | Carry set / unsigned ≥ | C=1 |
| 0100 | `BCC` / `BLO` | Carry clear / unsigned < | C=0 |
| 0101 | `BMI` | Minus / negative | N=1 |
| 0110 | `BPL` | Plus / positive or zero | N=0 |
| 0111 | `BVS` | Overflow set | V=1 |
| 1000 | `BVC` | Overflow clear | V=0 |
| 1001 | `BHI` | Unsigned greater than | C=1 & Z=0 |
| 1010 | `BLS` | Unsigned less or equal | C=0 \| Z=1 |
| 1011 | `BGE` | Signed greater or equal | N=V |
| 1100 | `BLT` | Signed less than | N≠V |
| 1101 | `BGT` | Signed greater than | Z=0 & N=V |
| 1110 | `BLE` | Signed less or equal | Z=1 \| N≠V |
| 1111 | `BL` | Branch and link (always) | — (saves PC+4 to link register) |

Conditions 0001-1110 are paired: each condition and its inverse differ only in bit 0, allowing simple inversion in the condition evaluation logic.

`BL` (cond=1111) is always taken. The microcode saves PC+4 (return address) to the conventional link register (R13) before branching. Function return is `JMP R13` (assembler alias: `RET`).

## Addressing Modes

As a load-store architecture, addressing modes apply only to load/store instructions:
- **Register + signed offset:** `LDW Rd, [Rb + #offset16]` — the primary addressing mode, covers struct fields, stack variables, array access from a base pointer
- **PC-relative via register:** Load PC into a register, then use register + offset. Alternatively, a compiler can use `BL` to a trampoline for position-independent code.

Register + register addressing (e.g., `LDW Rd, [Rb + Ri]`) is not directly supported in the encoding. Array indexing uses a computed address in a register: `ADD Raddr, Ri; LDW Rd, [Raddr + #0]`.

## Privilege Levels

Two privilege levels:
- **User mode (S=0):** Restricted access, cannot execute privileged instructions
- **Supervisor mode (S=1):** Full access to all instructions and hardware resources

Controlled transitions:
- **User to supervisor:** Via SYSCALL instruction or hardware interrupt/exception
- **Supervisor to user:** Via exception return (ERET) instruction

Privileged instructions: SETSR, DI, WRSYS, RDSYS, ERET, ICACHE_INV, WRSPR, RDSPR. Executing a privileged instruction in user mode raises a privilege violation exception (vector 3).

Note: EI (enable interrupts) and GETSR (read SR) are **unprivileged** — user code can enable interrupts (they may have been temporarily disabled by the kernel before returning) and can read its own flags.

## Interrupt Control

### EI — Enable Interrupts

`EI` sets SR.I = 1. It has a **one-instruction delay**: pending interrupts are not recognized until after the instruction following EI completes. This allows atomic enable-and-return patterns:

```asm
EI          ; SR.I = 1, but interrupts not yet recognized
ERET        ; executes in the "shadow" — completes before any pending interrupt fires
            ; NOW pending interrupts are checked
```

The delay is implemented in the microcode sequencer: a flip-flop is set when EI executes, causing the next instruction fetch to skip the pending-interrupt check. The flip-flop clears after one instruction cycle.

EI is **unprivileged** — user code may execute it (the kernel may have disabled interrupts before returning to user mode via an unusual path).

### DI — Disable Interrupts

`DI` sets SR.I = 0 with **immediate effect**. The very next interrupt check (after DI's micro-routine completes) sees I=0 and ignores pending interrupts.

DI is **privileged** — only the kernel may disable interrupts.

### Why Dedicated Instructions

Interrupt enable/disable must be atomic single instructions to avoid race conditions:

- **Read-modify-write race:** A GETSR/OR/SETSR sequence to set I=1 can be interrupted by NMI between GETSR and SETSR. The NMI handler's SR modifications would be overwritten by the stale value in SETSR.
- **Pending interrupt timing:** The one-instruction delay on EI cannot be implemented with a general SETSR — the delay is specific to the I bit.

GETSR/SETSR still exist for reading flags and kernel-level SR manipulation, but **must not be used for interrupt control**. Always use EI/DI.

### Typical Interrupt Handler Pattern

```asm
; ---- Handler entry (hardware has set S=1, I=0, pushed SR+PC) ----

; Save user registers
DEC   SP, #56
STW   R1,  [SP + #0]
STW   R2,  [SP + #4]
; ... save R3-R13 ...
STW   R13, [SP + #48]
RDSPR R1, USP               ; read banked-away user SP
STW   R1,  [SP + #52]       ; save it too

; Safe to re-enable interrupts (all critical state saved)
EI                           ; delayed: next instruction runs in shadow
BL    handle_interrupt       ; C handler — interrupts enabled during call

; Prepare to return (may switch to a different process)
DI                           ; disable before restoring context
BL    schedule               ; pick next process, returns proc pointer in R1

; Restore context for chosen process
LDW   R2,  [R1 + #PROC_USP]
WRSPR USP, R2               ; restore user SP
LDW   R2,  [R1 + #PROC_EPC]
WRSPR EPC, R2               ; set return address
LDW   R2,  [R1 + #PROC_ESR]
WRSPR ESR, R2               ; set return SR (user mode, flags)
LDW   R2,  [R1 + #PROC_R1]
; ... restore R3-R13 from process table ...
LDW   R13, [R1 + #PROC_R13]
LDW   R1,  [R1 + #PROC_R1]  ; restore R1 last (was used as pointer)

; Atomic return: enable interrupts, then ERET in the shadow
EI
ERET                         ; restores PC + SR from EPC/ESR, returns to user mode
```

## Stack Pointer Access

### RDSPR / WRSPR — Special-Purpose Register Access

Unified instructions for reading and writing the CPU's special-purpose registers. Both are privileged.

- **`RDSPR Rd, {ESR|EPC|USP}`** — Read SPR into Rd
- **`WRSPR {ESR|EPC|USP}, Rd`** — Write Rd to SPR

SPR encoding in IR[15:12] (same position as `sys_dev` for WRSYS/RDSYS):

| SPR | Number | Description |
|-----|--------|-------------|
| ESR | 0 | Exception SR — saved at exception entry |
| EPC | 1 | Exception PC — saved at exception entry |
| USP | 2 | User stack pointer — banked-away R14 |

Writing EPC/ESR allows trap handlers to modify the return state before `ERET`. For example, skipping a faulting instruction: `RDSPR R2, EPC; ADD R2, #4; WRSPR EPC, R2; ERET`. USP access is essential for saving/restoring the full user context on interrupt entry and process switches.

## Exception and Interrupt Model

Penumbra uses a **unified vector table** for all exceptions, traps, and external interrupts. The same entry/exit mechanism handles every case, simplifying the microcode and handler structure.

### Entry Sequence

On any interrupt, exception, or trap, the hardware performs:
1. Save PC → EPC, save SR → ESR (via `except_entry` pulse)
2. Set S=1 in SR (enter supervisor mode), set I=0 (disable interrupts)
3. Swap SP to supervisor stack pointer (SSP)
4. Read handler address from `vector_table[vector_number]` at physical memory (MMU bypassed)
5. Load PC from handler address (jump to handler)

For **exceptions** (page fault, illegal instruction, etc.), the saved PC is the address of the faulting instruction (so the handler can retry after fixing the cause). For **external interrupts**, the saved PC is the next instruction (since the current instruction completed). For **software traps**, the saved PC is the next instruction (the trap was intentional).

The kernel interrupt handler then saves remaining registers (R1-R13) and USP in software. (R0 need not be saved — it is always zero.)

### Exit Sequence

Return-from-interrupt (RTI/ERET) restores ESR then EPC, reversing the entry sequence. For context switches (return to a *different* process), the kernel uses WRSPR to set EPC/ESR to the new process's saved state, then ERET. RDSPR Rd, ESR/EPC lets the kernel read the exception registers to save them to the process table. All are privileged.

### Vector Table

The vector table is at **fixed physical addresses** starting at `0x0000_0000` in RAM. Each entry contains a **32-bit handler address** (MIPS/68k-style, not an instruction like ARM). On exception, `int_entry` reads the handler address from the vector table with MMU bypass, then loads it into PC. Software writes handler addresses at boot time. This eliminates nested TLB miss problems (no TLB entry needed for the vector page).

Note: Reset does not use the vector table. The CPU boots at `RESET_PC` (default `0xFFFF_E000`), a hardwired PC reset value pointing to boot ROM.

`vector_addr = vector_number × 4`

| Vector | Address | Source                | Status |
|--------|---------|-----------------------|--------|
| 0      | 0x00    | Bus fault (no device at address) | Implemented |
| 1      | 0x04    | External IRQ          | Implemented |
| 2      | 0x08    | TLB miss              | Implemented |
| 3      | 0x0C    | TLB protection fault  | Implemented |
| 4      | 0x10    | Privilege violation   | Implemented |
| 5      | 0x14    | SYSCALL               | Implemented |
| 6      | 0x18    | BREAK (debug)         | Implemented |
| 7      | 0x1C    | Illegal instruction   | Implemented |
| 8      | 0x20    | Alignment fault (fetch + data) | Implemented |
| 9-15   | 0x24–0x3C | (reserved for future: NMI, etc.) | — |

### How the CPU Gets the Vector Number

| Source | Mechanism |
|--------|-----------|
| External IRQ | `irq_taken = i_irq & sr_i & !ei_shadow`, hardwired VEC_IRQ=1 |
| TLB miss | `data_fault` or `fetch_fault` with `!mmu_hit && !mmu_align`, VEC_TLB_MISS=2 |
| TLB protection | `data_fault` or `fetch_fault` with `mmu_hit`, VEC_TLB_PROT=3 |
| Alignment | MMU checks `i_mem_size` vs `addr[1:0]`, sets `o_align`, VEC_ALIGN=8 |
| BREAK | Detected at dispatch (`dispatch_addr == 0x4A`), VEC_BREAK=6 |
| Priority | fault_pending (align > TLB) > illegal > priv > BREAK > SYSCALL > IRQ |

### External Interrupt Hardware

External interrupts use a simple **priority encoder** (one 74x148 chip in discrete):

```
Device IRQ lines ──→ Priority Encoder ──→ IRQ (active) + vector[2:0] ──→ CPU
                                                                          ↑
NMI (debug button, critical fault) ──────────────────────────────────────┘
```

- Priority is fixed by wiring order (timer = highest, as it drives the Minix 2 scheduler)
- The CPU checks `IRQ` between instructions; if asserted, it reads the 3-bit vector from the encoder and enters the interrupt sequence
- NMI bypasses the priority encoder and is non-maskable (ignores the I bit in SR)

### Software Trap Instructions

Two trap instructions with fixed vector assignments:

- **SYSCALL** — vector 7. Used for Minix 2 system calls. The syscall function number and arguments are passed in GPRs by software convention.
- **BREAK** — vector 8. Used for debug breakpoints. A debugger can patch instructions with BREAK and use the handler to implement single-stepping, watchpoints, etc.

## System Register Access

Penumbra uses a unified mechanism to access control registers on CPU-adjacent system devices (MMU, interrupt controller, timer, DMA controller). Two privileged instructions address a flat device:register space:

```
WRSYS Rsrc, #dev, #reg    ; Move To System register: Rsrc → device[dev].register[reg]
RDSYS Rdst, #dev, #reg    ; Move From System register: device[dev].register[reg] → Rdst
```

These are encoded as Format R instructions. The `spare[15:12]` field holds the 4-bit device ID, and `spare[11:8]` holds the 4-bit register index. This gives access to up to 16 devices × 16 registers = 256 system registers.

### System Register Bus

WRSYS/RDSYS do **not** use the main memory bus. Instead, they drive a lightweight sideband called the **system register bus**, which shares the existing data bus but uses dedicated control signals:

```
Shared with memory bus:
  data[31:0]          — read/write value (existing bus, reused)

Sideband control signals (new, accent bus):
  sys_cycle            — 1 = this is a sysreg bus access, not a memory access
  sys_dev[3:0]         — target device select
  sys_reg[3:0]         — register index within device
  sys_we               — write enable (1 = WRSYS, 0 = RDSYS)
```

Total new signals: **10 lines**. No additional data bus. The microcode ensures system register cycles and memory cycles never overlap.

Each system device has a 4-bit comparator on `sys_dev` to recognize its ID. When `sys_cycle` is asserted and the device ID matches, the device reads or drives `data[31:0]`.

### Device Map

| sys_dev | Device               | Example registers |
|---------|----------------------|-------------------|
| 0       | MMU                  | MMUCR (M bit), fault address, fault status, TLB control |
| 1       | Interrupt controller | Mask, pending, priority |
| 2       | Timer                | Count, compare, control |
| 3       | DMA controller       | Source addr, dest addr, length, control/status |
| 4-15    | Reserved             | Future expansion |

### External Peripherals

System register access is for CPU-adjacent devices only. External peripherals (UART, SPI, GPIO, Wiznet Ethernet) are accessed via regular load/store instructions to memory-mapped I/O addresses, mapped in uncacheable pages via the MMU C bit.

## Memory Model

- 32-bit virtual address space (4 GB)
- **Little-endian** byte order
- Page-based virtual memory managed by the MMU (4 KB pages)
- Memory protection (read/write/execute per page, user/supervisor)
- Per-page cacheability control (C bit in page table entry) for memory-mapped I/O
- Software-managed TLB (64-entry, 2-way set-associative)
- See the MMU overview for full details

### Byte Ordering

Penumbra is **little-endian**: the least-significant byte of a word occupies the
lowest address. This is defined by the `byte_ext` and `byte_rep` modules in
hardware, and matches the convention used by x86, RISC-V, and ARM in LE mode.

```
Word at address A:
  addr A+0  →  bits [ 7: 0]   (least significant byte)
  addr A+1  →  bits [15: 8]
  addr A+2  →  bits [23:16]
  addr A+3  →  bits [31:24]   (most significant byte)
```

For example, the 32-bit value `0x44434241` stored at address 0x1000:

| Address | Byte | ASCII |
|---------|------|-------|
| 0x1000 | 0x41 | 'A' |
| 0x1001 | 0x42 | 'B' |
| 0x1002 | 0x43 | 'C' |
| 0x1003 | 0x44 | 'D' |

`LDB R1, [R0 + #0x1000]` reads 0x41 ('A'); `LDW R1, [R0 + #0x1000]` reads
0x44434241. The assembler's `.asciz` and `.byte` directives pack data in this
order so that sequential byte loads read characters in string order.

## Microarchitecture Summary

The CPU is microcoded for flexibility and iterability.

- **Horizontal microcode:** 49-bit wide micro-words with direct control signal fields
- **Micro-PC sequencing:** Default increment; explicit branch via `next_addr` + `branch_cond` fields
- **Single micro-op ALU instructions:** Simple ALU operations (ADD, SUB, AND, etc.) execute in a single micro-op thanks to horizontal encoding
- **Flag-only variants:** The `reg_write_en` bit allows CMP (SUB without writeback) and TEST (AND without writeback) to share the same micro-word as their destructive counterparts, controlled by the F bit in Format R instructions
- **Long-latency stall:** Block-at-issue model. Multiply, divide, and future FPU operations stall the issuing instruction until complete. All long-latency units expose a uniform `busy`/`done` interface. Micro-PC hold is conditional on the selected unit's `busy` signal.
- **Branch handling:** No delay slots. Conditional branches stall instruction fetch until the branch resolves.
- **Sub-word access:** The `mem_size[1:0]` micro-word field encodes byte/halfword/word. The microcode generates `byte_en[3:0]` from `mem_size` and `addr[1:0]`, and checks alignment (misaligned access triggers alignment fault, vector 6). On sub-word loads, the CPU sign- or zero-extends the result based on a `sign_ext` micro-word bit.

### Byte Enable Generation

The cache and bus interface use `byte_en[3:0]` to select active byte lanes:

| `mem_size` | `addr[1:0]` | `byte_en` | Access |
|-----------|-------------|-----------|--------|
| `10` (word) | `00` | `1111` | Full word |
| `01` (half) | `00` | `0011` | Lower halfword |
| `01` (half) | `10` | `1100` | Upper halfword |
| `00` (byte) | `00` | `0001` | Byte 0 |
| `00` (byte) | `01` | `0010` | Byte 1 |
| `00` (byte) | `10` | `0100` | Byte 2 |
| `00` (byte) | `11` | `1000` | Byte 3 |

The D-cache data array supports per-byte writes (4 separate byte-wide SRAMs in discrete, native byte-write enables in ECP5 block RAM).

### Long-Latency Functional Units

| Unit       | Latency  | Notes |
|------------|----------|-------|
| Multiplier | Fixed    | Shift-and-add, deterministic cycle count |
| Divider    | Fixed    | Restoring/non-restoring, deterministic cycle count |
| FPU        | Variable | Early-out; `done` signal fires when result is ready |

All units use the same stall mechanism: microcode issues the operation, then holds on a "wait while busy" micro-op until `done`.

### Instruction Decode

The instruction decoder extracts the 2-bit format prefix and routes to the appropriate field extraction logic:

| Prefix | Format | Key fields to extract |
|--------|--------|-----------------------|
| `00` | R | op, Rd, Rs, F, spare (for WRSYS/RDSYS: dev, reg) |
| `01` | L | op, Rd, imm16 |
| `10` | M | L/S, sz, SE, Rd, Rb, offset16 |
| `11` | B | cond, offset22 |

The format prefix drives a micro-PC lookup to the start of the appropriate microcode routine. Within each format, the op/cond fields select the specific micro-routine.
