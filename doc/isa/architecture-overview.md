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
| R15      | PC   | Program counter |

### Zero Register (R0)

R0 always reads as zero. Any instruction that writes to R0 completes normally (including setting flags if applicable) but the result is discarded. This provides several useful pseudo-instructions:

| Pseudo-instruction | Actual encoding | Effect |
|-------------------|-----------------|--------|
| `NOP` | `ADD R0, R0` | No effect |
| `CMP Rs, #0` | `CMP Rs, R0` | Compare register to zero |
| Clear Rd | `MOV Rd, R0` | Rd = 0 |

### Status Register (SR)

The status register is **not** part of the 16-register GPR file. It is a separate hardware register internal to the CPU, accessed via dedicated privileged instructions (`GETSR`, `SETSR`).

SR contains at minimum:
- **Condition flags:** Z (zero), N (negative), C (carry), V (overflow)
- **S (supervisor):** Current privilege level (0 = user, 1 = supervisor)
- **I (interrupt enable):** Global interrupt mask
- **Previous mode bits:** Saved privilege state for return-from-interrupt

The SR contains only CPU-internal state. Registers belonging to other system devices (MMU, interrupt controller, etc.) are accessed via the system register bus — see [System Register Access](#system-register-access) below.

### Stack Pointer Banking

R14 (SP) is hardware-banked between user and supervisor modes. The hardware maintains two physical registers:
- **USP:** User stack pointer, active when S=0
- **KSP:** Kernel stack pointer, active when S=1

On privilege transitions (interrupt, trap, RTI), the hardware swaps which physical register is visible as R14. The inactive SP is accessible via privileged instructions for context save/restore.

This is the only banked register (B1 model). All other registers are shared across modes.

### Link Register

There is no hardware link register. The branch-and-link instruction (`BL`) writes the return address to a designated GPR by software convention (e.g., R13). This avoids requiring a dual-write-port register file.

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
| spare | 15:0 | Reserved. Used by MTSYS/MFSYS for device/register fields. |

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
| 10000 | `MTSYS Rd, #dev, #reg` | Write Rd to system device register | spare[15:12]=dev, spare[11:8]=reg |
| 10001 | `MFSYS Rd, #dev, #reg` | Read system device register to Rd | spare[15:12]=dev, spare[11:8]=reg |
| 10010 | `GETSR Rd` | Read SR to Rd | |
| 10011 | `SETSR Rd` | Write Rd to SR (privileged) | |
| 10100 | `SYSCALL` | System call trap (vector 7) | |
| 10101 | `BREAK` | Debug breakpoint (vector 8) | |
| 10110 | `RTI` | Return from interrupt (privileged) | |
| 10111 | `ICACHE_INV` | Invalidate entire I-cache (privileged) | |
| 11000-11111 | (reserved) | Future expansion (8 slots) | |

### Format L — Immediate (prefix `01`)

```
 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
[  0  1 |  op (3 bits) |   Rd (4 bits)  |  spare  |           imm16 (16 bits)                      ]
```

| Field | Bits | Description |
|-------|------|-------------|
| prefix | 31:30 | `01` — Format L |
| op | 29:27 | Immediate operation (3 bits, 8 opcodes) |
| Rd | 26:23 | Destination register |
| spare | 22:16 | Reserved (7 bits) |
| imm16 | 15:0 | 16-bit immediate value |

All immediate fields are consistently 16 bits wide.

| op | Mnemonic | Operation | Description |
|----|----------|-----------|-------------|
| 000 | `LLI Rd, #imm16` | Rd = zero_extend(imm16) | Load lower immediate (zero-extended) |
| 001 | `LLIS Rd, #imm16` | Rd = sign_extend(imm16) | Load lower immediate (sign-extended) |
| 010 | `LUI Rd, #imm16` | Rd = Rd OR (imm16 << 16) | Load upper immediate (ORs into upper half) |
| 011 | `INC Rd, #imm16` | Rd = Rd + zero_extend(imm16) | Increment by immediate |
| 100 | `DEC Rd, #imm16` | Rd = Rd - zero_extend(imm16) | Decrement by immediate |
| 101 | `CMPI Rd, #imm16` | flags = Rd - sign_extend(imm16) | Compare register to immediate (no write) |
| 110 | (reserved) | | |
| 111 | (reserved) | | |

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

Branch target: `PC + 4 + sign_extend(offset22 << 2)`

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

`BL` (cond=1111) is always taken. The microcode saves PC+4 (return address) to the conventional link register (R13) before branching. Function return is `MOV PC, R13` (or via whatever register holds the return address).

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
- **Supervisor to user:** Via return-from-interrupt (RTI) instruction

Privileged instructions: SETSR, MTSYS, MFSYS, RTI, ICACHE_INV. Executing a privileged instruction in user mode raises a privilege violation exception (vector 3).

## Exception and Interrupt Model

Penumbra uses a **unified vector table** for all exceptions, traps, and external interrupts. The same entry/exit mechanism handles every case, simplifying the microcode and handler structure.

### Entry Sequence

On any interrupt, exception, or trap, the hardware performs:
1. Set S=1 in SR (enter supervisor mode), set I=0 (disable interrupts)
2. Swap SP to kernel stack pointer (KSP)
3. Push old SR onto kernel stack
4. Push old PC onto kernel stack
5. Load PC from `vector_table[vector_number]`

For **exceptions** (page fault, illegal instruction, etc.), the saved PC is the address of the faulting instruction (so the handler can retry after fixing the cause). For **external interrupts**, the saved PC is the next instruction (since the current instruction completed). For **software traps**, the saved PC is the next instruction (the trap was intentional).

The kernel interrupt handler then saves remaining registers (R1-R13) in software. (R0 need not be saved — it is always zero.)

### Exit Sequence

Return-from-interrupt (RTI) reverses the entry sequence: pop PC and SR from kernel stack, restoring previous privilege level, interrupt enable state, and SP banking.

### Vector Table

The vector table is located at physical address `0x0000_0000` (base of RAM), set up by the kernel during boot. Each entry is one 32-bit handler address.

| Vector | Source                | Type           | Vector number source |
|--------|-----------------------|----------------|----------------------|
| 0      | Reset                 | —              | Hardwired            |
| 1      | NMI                   | External       | Hardwired            |
| 2      | Illegal instruction   | Exception      | Microcode            |
| 3      | Privilege violation   | Exception      | Microcode            |
| 4      | MMU fault (TLB miss / page fault) | Exception | Microcode     |
| 5      | Divide by zero        | Exception      | Microcode            |
| 6      | Alignment fault       | Exception      | Microcode            |
| 7      | SYSCALL               | Software trap  | Instruction          |
| 8      | BREAK (debug)         | Software trap  | Instruction          |
| 9      | Bus error             | Exception      | Microcode            |
| 10-15  | (reserved)            | —              | —                    |
| 16     | IRQ: Timer            | External       | Priority encoder + 16 |
| 17     | IRQ: UART             | External       | Priority encoder + 16 |
| 18     | IRQ: Wiznet Ethernet  | External       | Priority encoder + 16 |
| 19     | IRQ: DMA complete     | External       | Priority encoder + 16 |
| 20     | IRQ: SPI/SD           | External       | Priority encoder + 16 |
| 21-23  | IRQ: (reserved)       | External       | Priority encoder + 16 |

### How the CPU Gets the Vector Number

| Source | Mechanism |
|--------|-----------|
| Exceptions | Hardwired in microcode — each exception type branches to a micro-routine that loads the corresponding fixed vector number |
| SYSCALL | Fixed vector 7 |
| BREAK | Fixed vector 8 |
| NMI | Fixed vector 1, directly wired to CPU, non-maskable |
| External IRQ | Priority encoder outputs a 3-bit device number; microcode adds offset (16) to produce the vector table index |

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
MTSYS Rsrc, #dev, #reg    ; Move To System register: Rsrc → device[dev].register[reg]
MFSYS Rdst, #dev, #reg    ; Move From System register: device[dev].register[reg] → Rdst
```

These are encoded as Format R instructions. The `spare[15:12]` field holds the 4-bit device ID, and `spare[11:8]` holds the 4-bit register index. This gives access to up to 16 devices × 16 registers = 256 system registers.

### System Register Bus

MTSYS/MFSYS do **not** use the main memory bus. Instead, they drive a lightweight sideband called the **system register bus**, which shares the existing data bus but uses dedicated control signals:

```
Shared with memory bus:
  data[31:0]          — read/write value (existing bus, reused)

Sideband control signals (new, accent bus):
  sys_cycle            — 1 = this is a system register access, not a memory access
  sys_dev[3:0]         — target device select
  sys_reg[3:0]         — register index within device
  sys_we               — write enable (1 = MTSYS, 0 = MFSYS)
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
- Page-based virtual memory managed by the MMU (4 KB pages)
- Memory protection (read/write/execute per page, user/supervisor)
- Per-page cacheability control (C bit in page table entry) for memory-mapped I/O
- Software-managed TLB (64-entry, 2-way set-associative)
- See the MMU overview for full details

## Microarchitecture Summary

The CPU is microcoded for flexibility and iterability.

- **Horizontal microcode:** ~48-bit wide micro-words with direct control signal fields
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
| `00` | R | op, Rd, Rs, F, spare (for MTSYS/MFSYS: dev, reg) |
| `01` | L | op, Rd, imm16 |
| `10` | M | L/S, sz, SE, Rd, Rb, offset16 |
| `11` | B | cond, offset22 |

The format prefix drives a micro-PC lookup to the start of the appropriate microcode routine. Within each format, the op/cond fields select the specific micro-routine.
