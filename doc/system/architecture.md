# Penumbra ISA - Architecture Overview

## Design Philosophy

Penumbra is a 32-bit load-store RISC-like architecture. While inspired by the aesthetics and operational feel of 1970s/80s minicomputers like the Data General Eclipse and DEC VAX, the ISA itself is a clean design that avoids the accumulated complexity of those machines.

Key principles:
- **Load-store:** Only load and store instructions access memory; all computation operates on registers.
- **Fixed-width instructions:** All instructions are 32 bits wide.
- **2-operand format:** Arithmetic/logic instructions use a destructive destination (e.g., `ADD R1, R2` means `R1 = R1 + R2`).
- **Orthogonal design:** Minimize special cases and irregular encodings.
- **Consistent immediates:** All immediate fields are 16 bits wide.

## Registers

Penumbra has 16 registers addressed by a 4-bit field.

| Register | Name | Description |
|----------|------|-------------|
| R0       | ZERO | Hardwired to zero; writes are discarded |
| R1-R13   | GPR  | General-purpose (13 registers) |
| R14      | SP   | Stack pointer, hardware-banked (USP/SSP) |
| R15      | PC   | Program counter (read-only via register file) |

### Zero Register (R0)

R0 always reads as zero. Any instruction that writes to R0 completes normally (including setting flags if applicable) but the result is discarded. This provides several useful pseudo-instructions:

| Pseudo-instruction | Actual encoding | Effect |
|-------------------|-----------------|--------|
| `NOP` | `ADD R0, R0` | No effect |
| `CMP Rs, #0` | `CMP Rs, R0` | Compare register to zero |
| Clear Rd | `MOV Rd, R0` | Rd = 0 |

### Status Register (SR)

The status register is a separate hardware register, accessible as SPR 3 via `RDSPR Rd, SR` and `WRSPR SR, Rd`.

SR contains:
- **Condition flags:** Z (zero), N (negative), C (carry), V (overflow)
- **S (supervisor):** Current privilege level (0 = user, 1 = supervisor)
- **I (interrupt enable):** Global interrupt mask

Carry convention is **ARM-style** (C = NOT borrow on subtraction). Flags are updated by arithmetic/logic ALU operations, INC, DEC, CMPI, ANDI, TESTI, and MUL/DIV/MOD. MOV, load immediates (LLI, LLIS, LUI), loads, stores, branches, and system instructions do **not** affect flags.

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

### Stack Pointer Banking

R14 (SP) is hardware-banked between user and supervisor modes. The hardware maintains two physical registers:
- **USP:** User stack pointer, active when S=0
- **SSP:** Supervisor stack pointer, active when S=1

On privilege transitions (interrupt, trap, ERET), the hardware swaps which physical register is visible as R14. The inactive SP is accessible via privileged instructions (`RDSPR`/`WRSPR USP`) for context save/restore.

### Program Counter (R15)

**Reads:** When any instruction reads R15 (e.g., as a base register in a load/store), the register file returns the current PC value. This enables PC-relative addressing.

**Writes:** R15 cannot be written through the ALU or register file write port. PC is modified only by dedicated control flow instructions: branches (B/Bcc/BL), indirect jumps (JMP), exception return (ERET), and exception entry.

## Instruction Formats

Penumbra instructions use four 32-bit formats, selected by a 2-bit prefix.

- **Format R [00]:** Register-register ALU and system operations.
- **Format L [01]:** Immediate operations (load immediate, inc/dec, compare).
- **Format M [10]:** Memory load/store with register + offset.
- **Format B [11]:** Branch (conditional, unconditional, branch-and-link).

## Privilege Levels

- **User mode (S=0):** Restricted access, cannot execute privileged instructions.
- **Supervisor mode (S=1):** Full access to all instructions and hardware resources.

Privileged instructions: `DI`, `WRSYS`, `RDSYS`, `ERET`, `WRSPR`, `RDSPR`. Executing a privileged instruction in user mode raises a privilege violation exception (vector 4).

## Memory Model

- 32-bit virtual address space (4 GB).
- **Little-endian** byte order.
- Page-based virtual memory (4 KB pages).
- Memory protection (read/write/execute per page, user/supervisor).
- Per-page cacheability control (C bit in page table entry).

### Byte Ordering

Penumbra is **little-endian**: the least-significant byte of a word occupies the lowest address.

```
Word at address A:
  addr A+0  →  bits [ 7: 0]   (least significant byte)
  addr A+1  →  bits [15: 8]
  addr A+2  →  bits [23:16]
  addr A+3  →  bits [31:24]   (most significant byte)
```
