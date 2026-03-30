# Penumbra Instruction Set Guide

This is the programmer's reference for the Penumbra ISA. It covers everything
needed to write assembly by hand or implement a compiler targeting Penumbra
assembler output. Instruction encoding details are in the appendices for those
implementing an assembler or binary tools.

## Registers

Penumbra has 16 general-purpose 32-bit registers.

| Register | Notes |
|----------|-------|
| R0 | Hardwired zero. Reads always return 0. Writes are discarded. |
| R1 -- R12 | General purpose. |
| R13 (LR) | Link register. BL saves the return address here. |
| R14 (SP) | Stack pointer. Hardware-banked: user code sees USP, supervisor code sees SSP. Swapped automatically on privilege transitions. |
| R15 (PC) | Program counter. Readable (useful for PC-relative addressing), but not writable through normal instructions. |

The assembler accepts `LR`, `SP`, and `PC` as aliases for R13, R14, and R15.

## Status Register

The status register (SR) is separate from the register file. It is modified
implicitly by flag-setting instructions and explicitly by SETSR, ERET, and
exception entry.

```
Bit 31   30   29..4      3    2    1    0
    S    I    (reserved)  V    C    Z    N
```

| Bit | Name | Meaning |
|-----|------|---------|
| S | Supervisor | 1 = supervisor mode, 0 = user mode |
| I | Interrupt enable | 1 = IRQs enabled, 0 = masked |
| V | Overflow | Signed overflow on last flag-setting operation |
| C | Carry | Unsigned carry/borrow (ARM convention: C = NOT borrow on SUB) |
| Z | Zero | Result was zero |
| N | Negative | Result bit 31 (sign bit) |

## Condition Codes

Used by all branch instructions. The 16 conditions cover unsigned, signed,
and flag-based tests. Each condition and its inverse are adjacent codes
(differing in bit 0), which simplifies inversion in hardware and compilers.

| Code | Mnemonic | Meaning | Test |
|------|----------|---------|------|
| 0 | B (AL) | Always | 1 |
| 1 | BEQ / BZ | Equal / zero | Z |
| 2 | BNE / BNZ | Not equal / not zero | !Z |
| 3 | BCS / BHS | Carry set / unsigned >= | C |
| 4 | BCC / BLO | Carry clear / unsigned < | !C |
| 5 | BMI | Minus (negative) | N |
| 6 | BPL | Plus (positive or zero) | !N |
| 7 | BVS | Overflow set | V |
| 8 | BVC | Overflow clear | !V |
| 9 | BHI | Unsigned > | C and !Z |
| 10 | BLS | Unsigned <= | !C or Z |
| 11 | BGE | Signed >= | N == V |
| 12 | BLT | Signed < | N != V |
| 13 | BGT | Signed > | !Z and (N == V) |
| 14 | BLE | Signed <= | Z or (N != V) |
| 15 | BL | Branch and link (call) | Always (saves return address to LR) |

**After unsigned comparison** (`CMP Ra, Rb` where both are unsigned):
use BHI/BHS/BLO/BLS.

**After signed comparison** (`CMP Ra, Rb` where both are signed):
use BGT/BGE/BLT/BLE.

**After decrement / increment** (testing for zero):
use BZ/BNZ, which are aliases for BEQ/BNE but read more naturally in this
context.

---

## Instructions

### Data Movement

| Instruction | Syntax | Operation | Flags |
|-------------|--------|-----------|-------|
| MOV | `MOV Rd, Rs` | Rd = Rs | -- |
| LLI | `LLI Rd, #imm16` | Rd = zero_extend(imm16) | -- |
| LLIS | `LLIS Rd, #imm16` | Rd = sign_extend(imm16) | -- |
| LUI | `LUI Rd, #imm16` | Rd = Rd \| (imm16 << 16) | -- |

LLI clears the upper 16 bits. LLIS sign-extends bit 15 into the upper half.
LUI ORs into the upper half, leaving the lower half intact -- so the standard
pattern for loading a full 32-bit constant is:

```asm
    LLI  R1, #0x5678       ; R1 = 0x00005678
    LUI  R1, #0x1234       ; R1 = 0x12345678
```

For small values (0--65535), LLI alone suffices. For small negative values
(-32768 to -1), use LLIS.

LLI also accepts labels as immediates:

```asm
    LLI  LR, #my_label     ; R13 = address of my_label
```

### Arithmetic

| Instruction | Syntax | Operation | Flags |
|-------------|--------|-----------|-------|
| ADD | `ADD Rd, Rs` | Rd = Rd + Rs | NZCV |
| ADD | `ADD Rd, #imm16` | Rd = Rd + zero_extend(imm16) | NZCV |
| SUB | `SUB Rd, Rs` | Rd = Rd - Rs | NZCV |
| SUB | `SUB Rd, #imm16` | Rd = Rd - zero_extend(imm16) | NZCV |
| MUL | `MUL Rd, Rs` | Rd = Rd * Rs (signed) | NZCV |
| MULU | `MULU Rd, Rs` | Rd = Rd * Rs (unsigned) | NZCV |
| DIV | `DIV Rd, Rs` | Rd = Rd / Rs (signed) | NZCV |
| DIVU | `DIVU Rd, Rs` | Rd = Rd / Rs (unsigned) | NZCV |
| MOD | `MOD Rd, Rs` | Rd = Rd % Rs (signed) | NZCV |
| MODU | `MODU Rd, Rs` | Rd = Rd % Rs (unsigned) | NZCV |

All arithmetic is 2-operand destructive: the first operand is both a source
and the destination. Plan register usage accordingly -- save values you still
need before overwriting them.

The assembler automatically selects the correct encoding (Format R for
register-register, Format L for register-immediate) based on the operand type.

MUL/DIV/MOD are multi-cycle and stall the pipeline. They are initially
trapped as illegal instructions for software emulation; hardware support will
be added incrementally.

### Logic

| Instruction | Syntax | Operation | Flags |
|-------------|--------|-----------|-------|
| AND | `AND Rd, Rs` | Rd = Rd & Rs | NZCV |
| OR | `OR Rd, Rs` | Rd = Rd \| Rs | NZCV |
| XOR | `XOR Rd, Rs` | Rd = Rd ^ Rs | NZCV |
| NOT | `NOT Rd, Rs` | Rd = ~Rs | NZCV |

NOT is the only unary ALU operation. The source is Rs, not Rd.

### Shifts

| Instruction | Syntax | Operation | Flags |
|-------------|--------|-----------|-------|
| SHL | `SHL Rd, Rs` | Rd = Rd << Rs[4:0] | NZCV |
| SHR | `SHR Rd, Rs` | Rd = Rd >> Rs[4:0] (logical, zero-fill) | NZCV |
| SAR | `SAR Rd, Rs` | Rd = Rd >> Rs[4:0] (arithmetic, sign-fill) | NZCV |

Shift amount is taken from the low 5 bits of Rs (0--31). There are no
immediate shift instructions; load the shift count into a register first,
or use ADD/SUB with an immediate for multiply/divide by powers of two.

### Comparison

| Instruction | Syntax | Operation | Flags |
|-------------|--------|-----------|-------|
| CMP | `CMP Rd, Rs` | flags = Rd - Rs (Rd unchanged) | NZCV |
| CMP | `CMP Rd, #imm16` | flags = Rd - zero_extend(imm16) (Rd unchanged) | NZCV |
| TEST | `TEST Rd, Rs` | flags = Rd & Rs (Rd unchanged) | NZCV |

CMP and TEST are encoded as SUB and AND with the F-bit set, which suppresses
the register write. The ALU computes the result and updates flags normally,
but the destination register is not modified.

The assembler automatically selects the correct encoding for CMP based on
whether the second operand is a register or immediate.

`CMP Rd, R0` is the idiomatic way to test whether Rd is zero, since R0 is
always 0.

### Memory Access

All memory operations use base-register + signed 16-bit offset addressing.

| Instruction | Syntax | Operation | Flags |
|-------------|--------|-----------|-------|
| LDW | `LDW Rd, [Rb + #off]` | Rd = mem32[Rb + sext(off)] | -- |
| LDH | `LDH Rd, [Rb + #off]` | Rd = zero_extend(mem16[Rb + sext(off)]) | -- |
| LDHS | `LDHS Rd, [Rb + #off]` | Rd = sign_extend(mem16[Rb + sext(off)]) | -- |
| LDB | `LDB Rd, [Rb + #off]` | Rd = zero_extend(mem8[Rb + sext(off)]) | -- |
| LDBS | `LDBS Rd, [Rb + #off]` | Rd = sign_extend(mem8[Rb + sext(off)]) | -- |
| STW | `STW Rd, [Rb + #off]` | mem32[Rb + sext(off)] = Rd | -- |
| STH | `STH Rd, [Rb + #off]` | mem16[Rb + sext(off)] = Rd[15:0] | -- |
| STB | `STB Rd, [Rb + #off]` | mem8[Rb + sext(off)] = Rd[7:0] | -- |

The offset may be omitted for zero: `LDW R1, [R2]`.

Negative offsets use subtraction syntax: `LDW R1, [R2 - #8]`.

Word and halfword accesses must be naturally aligned (word to 4-byte,
halfword to 2-byte boundary). Misaligned accesses trap to vector 6.

**Stack operations** use SP (R14) as the base:

```asm
    ; Push R1
    SUB  SP, #4
    STW  R1, [SP]

    ; Pop into R1
    LDW  R1, [SP]
    ADD  SP, #4
```

### Branches

All branch instructions take a label (or numeric offset) as their operand.
The branch range is +/- 8 MB from the branch instruction.

```asm
    BEQ  target             ; branch to 'target' if Z=1
    B    loop               ; unconditional branch
    BL   subroutine         ; call: saves PC+4 to LR, then branches
```

See the condition code table above for the full set.

**BL (branch and link)** is the subroutine call instruction. It saves the
return address (PC+4) into R13 (LR) before branching. Return with `RET`.

### Subroutine Call and Return

```asm
    BL   my_function        ; call
    ; ...
my_function:
    ; ... function body ...
    RET                     ; return (JMP R13)
```

RET is a pseudo-instruction that assembles to `JMP R13`.

For nested calls, the callee must save LR to the stack before calling
further subroutines:

```asm
my_function:
    SUB  SP, #4
    STW  LR, [SP]           ; save return address

    BL   helper              ; nested call (clobbers LR)

    LDW  LR, [SP]           ; restore return address
    ADD  SP, #4
    RET
```

### System Instructions

| Instruction | Syntax | Operation | Privileged |
|-------------|--------|-----------|------------|
| JMP | `JMP Rs` | PC = Rs | No |
| EI | `EI` | Enable interrupts (SR.I = 1, delayed one instruction) | No |
| DI | `DI` | Disable interrupts (SR.I = 0, immediate) | Yes |
| WRSYS | `WRSYS Rd, #dev, #reg` | Write Rd to system register | Yes |
| RDSYS | `RDSYS Rd, #dev, #reg` | Read system register into Rd | Yes |
| RDSPR | `RDSPR Rd, {ESR\|EPC\|USP}` | Read special-purpose register into Rd | Yes |
| WRSPR | `WRSPR {ESR\|EPC\|USP}, Rd` | Write Rd to special-purpose register | Yes |
| ERET | `ERET` | Exception return via EPC/ESR (restore PC + SR) | Yes |
| GETSR | `GETSR Rd` | Rd = SR | No |
| SETSR | `SETSR Rs` | SR = Rs | Yes |
| SYSCALL | `SYSCALL` | Trap to vector 5 (system call) | No |
| BREAK | `BREAK` | Trap to vector 6 (debug breakpoint) | No |
| ICACHE_INV | `ICACHE_INV` | Invalidate instruction cache | Yes |

**EI timing guarantee:** The instruction immediately after EI always executes
before any pending interrupt is recognized. This enables the `EI` / `ERET`
pattern in interrupt handlers without a race.

**DI timing guarantee:** Takes effect immediately. The next instruction
executes with interrupts disabled.

Privileged instructions executed in user mode (SR.S = 0) trap to vector 4
(privilege violation).

**ERET forms:** The no-argument form restores SR and PC from the exception
registers (ESR/EPC) -- this is the normal return path for interrupt and fault
handlers. The two-argument form loads SR from Rd and PC from Rs atomically --
this is used for context switches (returning to a different process than the
one that was interrupted).

**WRSYS/RDSYS:** Access device-mapped system registers (MMU control, TLB
entries, system ID, etc.). See `doc/isa/sysregs-reference.md` for the
device/register map and assembly recipes.

### Pseudo-Instructions

| Pseudo | Expansion | Notes |
|--------|-----------|-------|
| NOP | `ADD R0, R0` | No visible effect (R0 write discarded) |
| RET | `JMP R13` | Return from subroutine |

### Useful Idioms

| Pattern | Code | Notes |
|---------|------|-------|
| Clear register | `MOV Rd, R0` | |
| Negate | `NOT Rd, Rs` then `ADD Rd, #1` | Two's complement: -x = ~x + 1 |
| Test for zero | `CMP Rd, R0` then `BZ target` | |
| Countdown loop | `SUB Rd, #1` then `BNZ loop` | SUB sets Z flag |
| 32-bit constant | `LLI Rd, #lo` then `LUI Rd, #hi` | |
| Absolute jump | `LLI Rd, #addr` then `JMP Rd` | For addresses > branch range |

---

## Exception and Interrupt Vectors

The vector table is at **fixed physical addresses** starting at 0x00. The vector
fetch bypasses the MMU (no TLB mapping needed for the vector page). Each
entry is a 32-bit instruction word (typically a branch to the handler).

`vector_addr = vector_number × 4`

| Vector | Address | Source | Status |
|--------|---------|--------|--------|
| 0 | 0x00 | Reset | Implemented |
| 1 | 0x04 | External IRQ | Implemented |
| 2 | 0x08 | TLB miss | Implemented |
| 3 | 0x0C | TLB protection fault | Implemented |
| 4 | 0x10 | Privilege violation | Implemented |
| 5 | 0x14 | SYSCALL | Implemented |
| 6 | 0x18 | BREAK (debug) | Implemented |
| 7 | 0x1C | Illegal instruction | Implemented |
| 8--15 | 0x20--0x3C | Reserved (NMI, alignment, bus error, etc.) | -- |

On exception entry, the hardware:
1. Saves PC and SR to exception registers (EPC, ESR)
2. Sets S = 1 (supervisor), I = 0 (interrupts disabled)
3. Loads PC from the vector table entry (physical fetch, MMU bypassed)

`ERET` restores ESR then EPC (returns to interrupted/faulting instruction).
`WRSPR EPC, Rd`/`WRSPR ESR, Rd` modifies the return state before `ERET` (e.g., skip a faulting instruction, or context switch to a different process).
`RDSPR Rd, ESR`/`RDSPR Rd, EPC` reads the exception registers so the kernel can save them.

---

## Assembler Syntax Reference

### Source Format

```asm
; Comments start with semicolon
label:                      ; Labels end with colon
    ADD  R1, R2             ; Register-register (assembler picks Format R)
    ADD  R1, #5             ; Register-immediate (assembler picks Format L)
    CMP  R1, #10            ; Same: assembler picks encoding automatically
    LDW  R3, [R4 + #8]     ; Memory: [base + #offset] or [base - #offset]
    BEQ  label              ; Branches take labels or numeric offsets
    ERET                    ; Exception return via EPC/ESR
    .word 0xDEADBEEF        ; Raw data directive
    .org 0x1000             ; Set assembly address
    .equ NAME, 0xFF         ; Named constant
```

### Immediate Values

Immediates are prefixed with `#`:

```asm
    LLI  R1, #42            ; decimal
    LLI  R1, #0xFF          ; hexadecimal
    LLI  R1, #0b1010        ; binary
    LLI  R1, #-1            ; negative decimal
    LLI  R1, #my_label      ; label address (resolved by assembler)
    LLI  R1, #TLB_V         ; named constant (built-in or .equ)
```

### Directives

| Directive | Example | Effect |
|-----------|---------|--------|
| `.org` | `.org 0x1000` | Set current address |
| `.word` | `.word 0xDEADBEEF` | Emit a raw 32-bit word |
| `.equ` | `.equ NAME, 0xFF` | Define a named constant |

---

## Appendix A: Instruction Encoding

All instructions are 32 bits. Bits [31:30] select one of four formats.

### Format R -- Register Operations (bits [31:30] = 00)

```
31 30  29      25  24   21  20   17  16  15          0
[ 00 ][ op (5) ][ Rd (4) ][ Rs (4) ][ F][ spare (16) ]
```

- **op:** ALU operation or system function (see opcode table below)
- **Rd:** destination and first source register
- **Rs:** second source register
- **F:** flag-only bit. When F=1, flags update but Rd is not written.
  Used for CMP (SUB with F=1) and TEST (AND with F=1).
- **spare:** reserved. For WRSYS/RDSYS: bits [15:12] = device, [11:8] = register.

**Format R opcode table:**

| op | Mnemonic | op | Mnemonic | op | Mnemonic | op | Mnemonic |
|----|----------|----|----------|----|----------|----|----------|
| 0 | ADD | 8 | MOV | 16 | WRSYS | 24 | JMP |
| 1 | SUB | 9 | NOT | 17 | RDSYS | 25 | EI |
| 2 | AND | 10 | MUL | 18 | GETSR | 26 | DI |
| 3 | OR | 11 | MULU | 19 | SETSR | 27 | WRSPR |
| 4 | XOR | 12 | DIV | 20 | SYSCALL | 28 | RDSPR |
| 5 | SHL | 13 | DIVU | 21 | BREAK | 29 | (free) |
| 6 | SHR | 14 | MOD | 22 | ERET | 30 | (free) |
| 7 | SAR | 15 | MODU | 23 | ICACHE_INV | 31 | (free) |

### Format L -- Immediate Operations (bits [31:30] = 01)

```
31 30  29   27  26   23  22       16  15             0
[ 01 ][ op(3) ][ Rd(4) ][ spare(7) ][   imm16 (16)   ]
```

- **op:** immediate operation (3 bits, 8 possible)
- **Rd:** destination register
- **imm16:** 16-bit immediate value

| op | Mnemonic | Immediate treatment |
|----|----------|---------------------|
| 0 | LLI | zero-extend |
| 1 | LLIS | sign-extend |
| 2 | LUI | shift left 16, OR into Rd |
| 3 | ADD #imm | zero-extend, add to Rd |
| 4 | SUB #imm | zero-extend, subtract from Rd |
| 5 | CMP #imm | zero-extend, subtract from Rd (flags only) |
| 6--7 | (reserved) | |

The assembler automatically routes `ADD Rd, #imm`, `SUB Rd, #imm`, and
`CMP Rd, #imm` to these Format L encodings. The internal encoding names
INC/DEC/CMPI are accepted as aliases for backward compatibility.

### Format M -- Memory Operations (bits [31:30] = 10)

```
31 30  29  28 27  26  25   22  21   18  17              2  1  0
[ 10 ][ L ][ sz ][ SE][ Rd(4) ][ Rb(4) ][  offset16 (16)  ][sp]
```

- **L:** 1 = load, 0 = store
- **sz:** access size: 00 = byte, 01 = halfword, 10 = word
- **SE:** sign-extend on load (ignored for stores and word loads)
- **Rd:** data register (destination for loads, source for stores)
- **Rb:** base address register
- **offset16:** 16-bit signed byte offset
- **sp:** spare (2 bits)

Effective address = Rb + sign_extend(offset16).

### Format B -- Branch Operations (bits [31:30] = 11)

```
31 30  29    26  25                           4  3    0
[ 11 ][ cond(4) ][        offset22 (22)        ][ sp  ]
```

- **cond:** condition code (see condition code table)
- **offset22:** signed 22-bit word offset from the branch instruction
- **sp:** spare (4 bits)

Branch target = PC + sign_extend(offset22) * 4.

The offset is relative to the branch instruction's own address (not PC+4).
Branch range: +/- 2^23 bytes = +/- 8 MB.

---

## Appendix B: Implementation Status

Instructions marked **Yes** have working microcode and pass simulation tests.
Instructions marked **Trap** dispatch to the illegal instruction handler for
software emulation. Instructions marked **No** are defined in the ISA and
assembler but do not yet have microcode.

| Instruction | Implemented | Notes |
|-------------|-------------|-------|
| ADD, SUB, AND, OR, XOR | Yes | Single-cycle ALU ops |
| SHL, SHR, SAR | Yes | Single-cycle shifts |
| MOV | Yes | No flag update |
| NOT | Yes | Updates flags |
| CMP, TEST | Yes | Via SUB/AND with F-bit |
| MUL, MULU, DIV, DIVU, MOD, MODU | Trap | Illegal instruction trap → SW emulation |
| LLI, LLIS, LUI | Yes | |
| ADD/SUB/CMP #imm | Yes | Format L encoding (formerly INC/DEC/CMPI) |
| LDW, STW | Yes | STALL-based, latency-agnostic |
| LDH, LDHS, LDB, LDBS, STH, STB | Yes | byte_ext/byte_rep + byte_en lane selection |
| B, BEQ/BZ, BNE/BNZ, and all Bcc | Yes | All 16 conditions |
| BL | Yes | Saves PC+4 to R13 |
| JMP (RET) | Yes | |
| EI, DI | Yes | ei_shadow, privilege check |
| WRSYS, RDSYS | Yes | Privileged; see `doc/isa/sysregs-reference.md` |
| RDSPR | Yes | Unified: reads ESR, EPC, or USP (SPR in IR[15:12]) |
| WRSPR | Yes | Unified: writes ESR, EPC, or USP (SPR in IR[15:12]) |
| ERET | Yes | Returns via EPC/ESR |
| BREAK | Yes | Trap to vector 6 |
| GETSR, SETSR | No | |
| SYSCALL | Yes | Trap to vector 5, unprivileged |
| ICACHE_INV | No | |
| NOP (pseudo) | Yes | ADD R0, R0 |
| RET (pseudo) | Yes | JMP R13 |
