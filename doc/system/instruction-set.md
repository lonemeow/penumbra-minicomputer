# Penumbra Instruction Set Guide

This is the programmer's reference for the Penumbra ISA. It covers everything
needed to write assembly by hand or implement a compiler targeting Penumbra
assembler output.

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

The status register (SR) is modified implicitly by flag-setting instructions and explicitly by `WRSPR SR`, `ERET`, and exception entry. It can be read via `RDSPR Rd, SR`.

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

Used by all branch instructions.

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

---

## Instructions

### Data Movement

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| MOV | `MOV Rd, Rs` | Rd = Rs |
| LLI | `LLI Rd, #imm16` | Rd = zero_extend(imm16) |
| LLIS | `LLIS Rd, #imm16` | Rd = sign_extend(imm16) |
| LUI | `LUI Rd, #imm16` | Rd = Rd \| (imm16 << 16) |

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

### Logic

| Instruction | Syntax | Operation | Flags |
|-------------|--------|-----------|-------|
| AND | `AND Rd, Rs` | Rd = Rd & Rs | NZCV |
| OR | `OR Rd, Rs` | Rd = Rd \| Rs | NZCV |
| XOR | `XOR Rd, Rs` | Rd = Rd ^ Rs | NZCV |
| NOT | `NOT Rd, Rs` | Rd = ~Rs | NZCV |

### Shifts

| Instruction | Syntax | Operation | Flags |
|-------------|--------|-----------|-------|
| SHL | `SHL Rd, Rs` | Rd = Rd << Rs[4:0] | NZCV |
| SHR | `SHR Rd, Rs` | Rd = Rd >> Rs[4:0] (logical) | NZCV |
| SAR | `SAR Rd, Rs` | Rd = Rd >> Rs[4:0] (arithmetic) | NZCV |

### Comparison

| Instruction | Syntax | Operation | Flags |
|-------------|--------|-----------|-------|
| CMP | `CMP Rd, Rs` | flags = Rd - Rs (Rd unchanged) | NZCV |
| CMP | `CMP Rd, #imm16` | flags = Rd - zero_extend(imm16) | NZCV |
| TEST | `TEST Rd, Rs` | flags = Rd & Rs (Rd unchanged) | NZCV |

### Memory Access

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| LDW | `LDW Rd, [Rb + #off]` | Rd = mem32[Rb + sext(off)] |
| LDH | `LDH Rd, [Rb + #off]` | Rd = zero_extend(mem16[Rb + sext(off)]) |
| LDHS | `LDHS Rd, [Rb + #off]` | Rd = sign_extend(mem16[Rb + sext(off)]) |
| LDB | `LDB Rd, [Rb + #off]` | Rd = zero_extend(mem8[Rb + sext(off)]) |
| LDBS | `LDBS Rd, [Rb + #off]` | Rd = sign_extend(mem8[Rb + sext(off)]) |
| STW | `STW Rd, [Rb + #off]` | mem32[Rb + sext(off)] = Rd |
| STH | `STH Rd, [Rb + #off]` | mem16[Rb + sext(off)] = Rd[15:0] |
| STB | `STB Rd, [Rb + #off]` | mem8[Rb + sext(off)] = Rd[7:0] |

### Branches

| Instruction | Syntax | Operation |
|-------------|--------|-----------|
| Bcc | `Bcc target` | Branch if condition `cc` is met |
| B | `B target` | Unconditional branch |
| BL | `BL target` | Branch and link (call): LR = PC+4, branch |
| JMP | `JMP Rs` | Indirect jump: PC = Rs |

### System Instructions

| Instruction | Syntax | Operation | Privileged |
|-------------|--------|-----------|------------|
| EI | `EI` | Enable interrupts (one-instruction delay) | No |
| DI | `DI` | Disable interrupts (immediate) | Yes |
| WRSYS | `WRSYS Rd, #dev, #reg` | Write Rd to system register | Yes |
| RDSYS | `RDSYS Rd, #dev, #reg` | Read system register into Rd | Yes |
| RDSPR | `RDSPR Rd, {SPR}` | Read special-purpose register | Yes |
| WRSPR | `WRSPR {SPR}, Rd` | Write special-purpose register | Yes |
| ERET | `ERET` | Exception return: restore PC+SR | Yes |
| SYSCALL | `SYSCALL` | Trap to vector 5 | No |
| BREAK | `BREAK` | Trap to vector 6 | No |

---

## Assembler Syntax

### Source Format

```asm
; Comments start with semicolon
label:                      ; Labels end with colon
    ADD  R1, R2             ; Register-register
    ADD  R1, #5             ; Register-immediate
    LDW  R3, [R4 + #8]     ; Memory: [base + #offset]
    BEQ  label              ; Branches
    .word 0xDEADBEEF        ; Raw data
    .org 0x1000             ; Set address
    .equ NAME, 0xFF         ; Constant
```

### Directives

| Directive | Example | Effect |
|-----------|---------|--------|
| `.org` | `.org 0x1000` | Set current address |
| `.word` | `.word 0x1234` | Emit raw 32-bit word |
| `.byte` | `.byte 0x42` | Emit raw 8-bit byte |
| `.asciz`| `.asciz "hi"` | Emit null-terminated ASCII string |
| `.equ` | `.equ K, 5` | Define a named constant |
