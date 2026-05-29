# Penumbra ISA — Architecture Overview

## Design Philosophy

Penumbra is a 32-bit load-store RISC-like architecture. While inspired
by the aesthetics and operational feel of 1970s/80s minicomputers like
the Data General Eclipse and DEC VAX, the ISA itself is a clean design
that avoids the accumulated complexity of those machines.

Key principles:

- **Load-store.** Only load and store instructions access memory; all
  computation operates on registers.
- **Fixed-width instructions.** All instructions are 32 bits wide.
- **2-operand format.** Arithmetic/logic instructions use a destructive
  destination (e.g., `ADD R1, R2` means `R1 = R1 + R2`).
- **Orthogonal design.** Minimise special cases and irregular encodings.
- **Consistent immediates.** All immediate fields are 16 bits wide.
- **Discrete-friendly.** All architectural choices must be feasible in
  a future discrete 74xx implementation.

---

## Registers

Penumbra has 16 registers addressed by a 4-bit field.

| Register | Name | Description                                             |
|:--------:|------|---------------------------------------------------------|
| R0       | ZERO | Hardwired to zero; writes are discarded                 |
| R1–R11   | GPR  | General-purpose                                         |
| R12      | TP   | Thread pointer (ABI convention; see below)              |
| R13      | LR   | Link register — hardware target of `BL`                 |
| R14      | SP   | Stack pointer, hardware-banked USP/SSP                  |
| R15      | PC   | Program counter — readable; writable only by control flow |

The assembler accepts `ZERO`, `TP`, `LR`, `SP`, `PC` as aliases for
the numeric registers.

### Zero Register (R0)

R0 always reads as zero. Any instruction that writes R0 completes
normally (including setting flags) but the result is discarded. This
provides useful pseudo-instructions:

| Pseudo-instruction | Actual encoding | Effect                         |
|--------------------|-----------------|--------------------------------|
| `NOP`              | `ADD R0, R0`    | No effect                      |
| `CMP Rs, #0`       | `CMP Rs, R0`    | Compare register to zero       |
| Clear Rd           | `MOV Rd, R0`    | `Rd = 0`                       |

### Status Register (SR)

The status register is **not** part of the 16-register GPR file. It is a
separate CPU-internal register, accessed as SPR 3 via `RDSPR`/`WRSPR`.
Condition flags (Z, N, C, V), supervisor (S), and interrupt-enable (I)
live here. The previous mode on exception entry is saved to `ESR`, not
to bits within SR itself.

```
 31  30  29                          4   3   2   1   0
┌───┬───┬──────── reserved (0) ──────┬───┬───┬───┬───┐
│ S │ I │         0 0 0 ... 0        │ V │ C │ Z │ N │
└───┴───┴────────────────────────────┴───┴───┴───┴───┘
```

| Bit   | Field | Description                                          |
|:-----:|:-----:|------------------------------------------------------|
| 31    | S     | Supervisor mode (1 = supervisor, 0 = user)           |
| 30    | I     | Interrupt enable (1 = enabled, 0 = masked)           |
| 29:4  | —     | Reserved; read as zero, ignored on write             |
| 3     | V     | Signed overflow                                      |
| 2     | C     | Carry — ARM convention: `C = NOT borrow` on `SUB`    |
| 1     | Z     | Zero                                                 |
| 0     | N     | Negative (= `result[31]`)                            |

Flags are updated by ALU arithmetic/logic, `ADD/SUB/CMP/AND/TEST` with
immediates, and `MUL/MULU/DIV/DIVU` (which compute NZ from the low
half / quotient — C and V are forced to zero). `MOV`, load immediates
(LLI/LLIS/LUI), loads, stores, branches, and system instructions do
**not** affect flags.

### Stack Pointer Banking (B1 model)

R14 (SP) is hardware-banked between user and supervisor modes. Two
physical registers exist:

- **USP** — user stack pointer, active when `S=0`.
- **SSP** — supervisor stack pointer, active when `S=1`.

On privilege transitions (interrupt, trap, `ERET`), the hardware swaps
which physical register is visible as R14. The inactive SP is
accessible via privileged `RDSPR USP` / `WRSPR USP` for context
save/restore. This is the **only** banked register; everything else is
shared across modes (the B1 banking model).

### Program Counter (R15)

PC is a separate hardware register with its own dedicated adder for
branch and increment — it is not part of the main register file or ALU
datapath.

- **Reads:** reading R15 returns the current PC value, enabling
  PC-relative addressing for literal-pool constants.
- **Writes:** R15 cannot be written through the ALU or register-file
  write port. PC is modified only by dedicated control-flow
  instructions: `B`/`Bcc`/`BL`, `JMP`, `ERET`, and exception entry.
  This eliminates accidental PC writes and simplifies the datapath.

### Link Register (R13)

`BL` is the only instruction that writes a return address, and it
**always** writes to R13 — the target register is hard-coded in the
microcode, not chosen by a field in the instruction. There is no
separate hardware LR register; R13 is the single GPR that serves as
the link register.

Return uses `JMP R13` (assembler alias `RET`). Nested calls must save
R13 to the stack explicitly before calling further, since the next
`BL` will overwrite it.

### Thread Pointer (R12)

R12 is reserved by ABI convention as the **thread pointer** for
thread-local storage. The hardware does not use R12 specially — any
instruction can read or write it. The ABI convention exists so that
compilers can generate TLS accesses as simple offsets from R12 without
having to spill/reload it on every access. See
[abi.md](./abi.md) for full calling-convention details.

---

## Instruction Formats

All instructions are 32 bits wide; bits `[31:30]` select the format.

| Prefix | Format | Purpose                                                       |
|:------:|:------:|---------------------------------------------------------------|
| `00`   | R      | Register-register ALU and system operations                   |
| `01`   | L      | Immediate operations (load immediate, add/sub/cmp/and/test #imm, imm-shifts) |
| `10`   | M      | Memory load/store with register + signed 16-bit offset         |
| `11`   | B      | Branch (conditional, unconditional, branch-and-link)           |

Full bit-level encoding and opcode tables live in
[instruction-set.md](./instruction-set.md#appendix-a--instruction-encoding).

---

## Addressing Modes

As a load-store architecture, addressing modes apply only to load/store
instructions:

- **Register + signed 16-bit offset** — `LDW Rd, [Rb + #off]`. The
  primary addressing mode. Covers struct fields, stack variables, and
  array elements within ±32 KB of a base pointer.
- **PC-relative via register** — load PC into a GPR (`MOV Rd, PC`),
  then use register + offset. Compilers can also emit `BL` trampolines
  for position-independent code.

Register + register addressing (`LDW Rd, [Rb + Ri]`) is **not** in the
encoding. Array indexing requires a computed address in a register:
`ADD Raddr, Ri` then `LDW Rd, [Raddr + #0]`.

Word and halfword accesses must be naturally aligned (word to 4-byte,
halfword to 2-byte). Misaligned accesses trap to `VEC_ALIGN` (vector 8).

---

## Privilege Levels

- **User (S=0).** Restricted; cannot execute privileged instructions.
- **Supervisor (S=1).** Full access.

Privileged instructions: `EI`, `DI`, `WRSYS`, `RDSYS`, `ERET`,
`WRSPR`, `RDSPR`. Executing a privileged instruction in user mode
raises `VEC_PRIV` (vector 4). User code cannot enable interrupts; the
kernel owns `SR.I` and restores it via `ERET` from `ESR`.

Controlled transitions:

- **User → supervisor:** `SYSCALL` or any hardware interrupt/exception.
- **Supervisor → user:** `ERET`.

---

## Special-Purpose Registers (SPRs)

Unified privileged instructions for reading/writing the CPU's
special-purpose registers:

```
RDSPR Rd, {SPR}            ; Rd = SPR
WRSPR {SPR}, Rd            ; SPR = Rd
```

SPR encoding in IR[15:12] (same field position as `dev` for WRSYS/RDSYS):

| SPR    | Number | Description                                       |
|:------:|:------:|---------------------------------------------------|
| ESR    | 0      | Exception SR — saved at exception entry           |
| EPC    | 1      | Exception PC — saved at exception entry           |
| USP    | 2      | User stack pointer — banked-away R14              |
| SR     | 3      | Current status register (flags + mode bits)       |
| SCR0   | 4      | Scratch SPR (supervisor-only, 32-bit storage)     |
| SCR1   | 5      | Scratch SPR (supervisor-only, 32-bit storage)     |
| SCR2   | 6      | Scratch SPR (supervisor-only, 32-bit storage)     |
| SCR3   | 7      | Scratch SPR (supervisor-only, 32-bit storage)     |
| 8–15   | —      | Reserved (future debug/performance SPRs)          |

**Skip-faulting-instruction idiom.** Trap handlers can modify the
return state before `ERET`:

```asm
; Advance EPC past the faulting instruction
RDSPR R2, EPC
ADD   R2, #4
WRSPR EPC, R2
ERET
```

`USP` access is essential for saving/restoring the full user context on
interrupt entry and process switches.

### Scratch SPRs (SCR0–SCR3)

Four 32-bit storage registers exposed to supervisor code via
`RDSPR`/`WRSPR`. The hardware contract is **storage cells, nothing
more**: a write to SCRn stores the value; a read returns the most
recent value written. Reset values are **undefined** — software must
write before it reads.

These exist to eliminate RAM accesses from the trap-entry prologue.
The hottest trap (TLB miss) can park its working set of GPRs without
touching memory, even before the kernel has saved enough context to
safely fault.

```asm
trap_entry:
    WRSPR SCR0, R1            ; park caller GPRs in CPU-internal storage
    WRSPR SCR1, R2
    WRSPR SCR2, R3
    ; ... use R1-R3 freely (PTE walk, vector dispatch, ...) ...
    RDSPR R3, SCR2
    RDSPR R2, SCR1
    RDSPR R1, SCR0
    ERET
```

Like the other SPRs, `RDSPR`/`WRSPR` against SCR0–SCR3 are privileged;
user-mode access faults with `VEC_PRIV`. The hardware does not assign
roles to specific SCRn registers — software chooses how to use them.

---

## Interrupt Control

### EI — Enable Interrupts

`EI` sets `SR.I = 1` with a **one-instruction delay**: pending
interrupts are not recognised until after the instruction following
`EI` completes. This enables the atomic enable-and-return pattern:

```asm
EI          ; SR.I = 1, but interrupts not yet recognised
ERET        ; executes in the "shadow" — completes before any IRQ fires
```

Internally a flip-flop (`ei_shadow`) is set when `EI` executes, causing
the next instruction fetch to skip the pending-interrupt check. The
flip-flop clears after one instruction cycle. `EI` is privileged; user
mode attempts trap to `VEC_PRIV`.

### DI — Disable Interrupts

`DI` sets `SR.I = 0` with **immediate effect**. The next interrupt
check (after `DI`'s micro-routine completes) sees `I=0`. `DI` is
privileged.

### Why Dedicated EI/DI

Interrupt enable/disable must be atomic single instructions to avoid
race conditions:

- **Read-modify-write race.** `RDSPR SR` → `OR` → `WRSPR SR` to set `I=1`
  can be interrupted between read and write; a handler's SR changes
  would be overwritten by the stale WRSPR value.
- **Pending-interrupt timing.** The one-instruction delay on `EI` is
  specific to the `I` bit and cannot be expressed through a general
  `WRSPR SR`.

`RDSPR/WRSPR SR` remain useful for saving/restoring the full SR state
(e.g., atomic sections that need to preserve the caller's interrupt
state) but **must not** substitute for `EI`/`DI`.

---

## Exception Model

Penumbra uses a **unified vector table** for all exceptions, traps,
and external interrupts. The same entry/exit mechanism handles every
case, simplifying microcode and handler structure.

### Entry Sequence

On any interrupt, exception, or trap the hardware:

1. Saves `PC → EPC`, saves `SR → ESR`.
2. Sets `SR.S = 1` (supervisor), `SR.I = 0` (IRQs disabled).
3. Swaps `SP` to the supervisor stack pointer (SSP).
4. Reads the handler address from `vector_table[vector_number]` at
   physical memory (MMU bypassed).
5. Loads `PC` from that handler address.

- **Exceptions** (page fault, illegal instruction): saved PC is the
  faulting instruction (handler can retry after fixing the cause).
- **External interrupts:** saved PC is the next instruction
  (the interrupted one completed).
- **Software traps** (`SYSCALL`, `BREAK`): saved PC is the next
  instruction.

The kernel handler then saves remaining registers (`R1`–`R13`) and
`USP` in software. `R0` need not be saved — it is always zero.

### Exit Sequence

`ERET` restores `ESR` then `EPC`, reversing the entry sequence. For
context switches (return to a **different** process), the kernel uses
`WRSPR` to set `EPC`/`ESR` to the new process's saved state, then
`ERET`.

### Vector Table

Located at fixed physical addresses starting at `0x0000_0000`. Each
entry is a **32-bit handler address** (MIPS/68k-style, not an
instruction like ARM). Vector fetch bypasses the MMU — no TLB mapping
needed for the vector page. `vector_addr = vector_number × 4`.

| Vector | Addr | Name            | Source                              |
|:------:|:----:|-----------------|-------------------------------------|
| 0      | 0x00 | `VEC_BUS_FAULT` | Bus fault (no device at address)    |
| 1      | 0x04 | `VEC_TIMER`     | Timer interrupt                     |
| 2      | 0x08 | `VEC_TLB_MISS`  | TLB miss                            |
| 3      | 0x0C | `VEC_TLB_PROT`  | TLB protection fault                |
| 4      | 0x10 | `VEC_PRIV`      | Privilege violation                 |
| 5      | 0x14 | `VEC_SYSCALL`   | `SYSCALL` software trap             |
| 6      | 0x18 | `VEC_BREAK`     | `BREAK` (debug)                     |
| 7      | 0x1C | `VEC_ILLEGAL`   | Illegal instruction                 |
| 8      | 0x20 | `VEC_ALIGN`     | Alignment fault (fetch + data)      |
| 9      | 0x24 | `VEC_EXT_IRQ`   | External device IRQ (wired-OR)      |
| 10     | 0x28 | `VEC_ARITH`     | Arithmetic fault (DIV0, narrowing-DIV overflow — defensive) |
| 11–15  | —    | —               | Reserved (future NMI, etc.)         |

**Reset does not use the vector table.** The CPU boots at `RESET_PC`
(default `0xFFFF_0000`), a hardwired PC reset value pointing to boot ROM.

### Two-IRQ Architecture

The CPU has two asynchronous interrupt inputs, each with its own vector:

- **Timer IRQ** (`VEC_TIMER`): dedicated vector for the programmable
  interval timer (sysreg device 7). Its own vector lets the OS
  scheduler tick avoid the polling cost of shared-IRQ dispatch.
- **External device IRQ** (`VEC_EXT_IRQ`): shared wired-OR line for all
  bus devices — no interrupt controller. Software polls each device's
  status register to identify the source (PCI-style shared interrupt).

Both are masked by `SR.I` and gated by `ei_shadow`. Timer has strict
priority: if both are pending simultaneously, the timer is taken first.

### Software Trap Instructions

- **`SYSCALL`** → `VEC_SYSCALL`. The architecturally-defined entry for
  user → kernel calls. The OS defines its own conventions for
  register-based argument passing, syscall-number location, and
  error reporting — the hardware only delivers the trap. See
  [abi.md](./abi.md) for the conventions used by the current userland.
- **`BREAK`** → `VEC_BREAK`. Debug breakpoints. A debugger can patch
  instructions with `BREAK` to implement single-stepping, watchpoints,
  or assertion failures.

---

## Memory Model

- 32-bit virtual address space (4 GB).
- **Little-endian** byte order.
- Page-based virtual memory (4 KB pages) — see [mmu.md](./mmu.md).
- Memory protection (R/W/X per page, user/supervisor).
- Per-page cacheability control (`C` bit in TLB entry) — essential for
  memory-mapped I/O.
- Software-managed TLB (64-entry 2-way SA + 8-entry FA pinned).

### Byte Ordering

```
Word at address A:
  addr A+0  →  bits [ 7: 0]   (least-significant byte)
  addr A+1  →  bits [15: 8]
  addr A+2  →  bits [23:16]
  addr A+3  →  bits [31:24]   (most-significant byte)
```

The 32-bit value `0x44434241` stored at `0x1000`:

| Address | Byte | ASCII |
|:-------:|:----:|:-----:|
| 0x1000  | 0x41 | 'A'   |
| 0x1001  | 0x42 | 'B'   |
| 0x1002  | 0x43 | 'C'   |
| 0x1003  | 0x44 | 'D'   |

`LDB R1, [R0 + #0x1000]` reads `'A'`; `LDW R1, [R0 + #0x1000]` reads
`0x44434241`. The assembler's `.asciz` and `.byte` directives pack
data in this order, so sequential byte loads read characters in string
order. This matches x86, RISC-V, and ARM in LE mode.
