# Penumbra Instruction Set Guide

Programmer's reference for the Penumbra ISA. Covers what you need to
write assembly by hand or target Penumbra from a compiler.
Architectural context (registers, SR, privilege, exception model) is
in [architecture.md](./architecture.md). Bit-level encoding — opcode
tables, field widths — is in the separate
[instruction-encoding.md](./instruction-encoding.md) reference, which
most readers will not need.

## Registers

| Register  | Notes                                                           |
|:---------:|-----------------------------------------------------------------|
| R0        | Hardwired zero. Reads return 0. Writes discarded.               |
| R1 – R11  | General-purpose.                                                |
| R12 (TP)  | Thread pointer (ABI convention — hardware doesn't care).        |
| R13 (LR)  | Link register — hardware target of `BL`.                        |
| R14 (SP)  | Stack pointer. Hardware-banked USP/SSP on privilege transitions.|
| R15 (PC)  | Program counter. Readable (for PC-relative); not writable via normal instructions. |

The assembler accepts `ZERO`, `TP`, `LR`, `SP`, `PC` as register
aliases. SPR names `ESR`, `EPC`, `USP`, `SR` are accepted by
`RDSPR`/`WRSPR`.

## Condition Codes

Used by all branch instructions. The 16 conditions cover unsigned,
signed, and flag-based tests. Each condition and its inverse differ
only in bit 0 — simplifying inversion in hardware and compilers.

| Code | Mnemonic       | Meaning                 | Test            |
|:----:|----------------|-------------------------|-----------------|
| 0    | `B` (AL)       | Always                  | 1               |
| 1    | `BEQ` / `BZ`   | Equal / zero            | Z               |
| 2    | `BNE` / `BNZ`  | Not equal / not zero    | !Z              |
| 3    | `BCS` / `BHS`  | Carry set / unsigned ≥  | C               |
| 4    | `BCC` / `BLO`  | Carry clear / unsigned < | !C             |
| 5    | `BMI`          | Minus (negative)        | N               |
| 6    | `BPL`          | Plus (positive or zero) | !N              |
| 7    | `BVS`          | Overflow set            | V               |
| 8    | `BVC`          | Overflow clear          | !V              |
| 9    | `BHI`          | Unsigned >              | C & !Z          |
| 10   | `BLS`          | Unsigned ≤              | !C \| Z         |
| 11   | `BGE`          | Signed ≥                | N == V          |
| 12   | `BLT`          | Signed <                | N != V          |
| 13   | `BGT`          | Signed >                | !Z & (N == V)   |
| 14   | `BLE`          | Signed ≤                | Z \| (N != V)   |
| 15   | `BL`           | Branch-and-link (call)  | Always; saves PC+4 to LR |

Use `BHI`/`BHS`/`BLO`/`BLS` after unsigned compares,
`BGT`/`BGE`/`BLT`/`BLE` after signed compares. `BZ`/`BNZ` read more
naturally after a countdown decrement.

---

## Instructions

### Data Movement

| Instruction | Syntax              | Operation                           | Flags |
|-------------|---------------------|-------------------------------------|:-----:|
| MOV         | `MOV Rd, Rs`        | `Rd = Rs`                           | —     |
| LLI         | `LLI Rd, #imm16`    | `Rd = zero_extend(imm16)`           | —     |
| LLIS        | `LLIS Rd, #imm16`   | `Rd = sign_extend(imm16)`           | —     |
| LUI         | `LUI Rd, #imm16`    | `Rd = Rd \| (imm16 << 16)`          | —     |

`LLI` clears the upper 16 bits. `LLIS` sign-extends. `LUI` **ORs** into
the upper half, leaving the lower half intact. The standard pattern
for loading a full 32-bit constant:

```asm
LLI  R1, #0x5678            ; R1 = 0x00005678
LUI  R1, #0x1234            ; R1 = 0x12345678
```

For small values (0–65535), `LLI` alone suffices. For small negatives
(−32768 to −1), use `LLIS`. For upper-half-only constants, clear first
because `LUI` ORs:

```asm
MOV  R1, R0                 ; clear R1
LUI  R1, #0xFF00            ; R1 = 0xFF000000
```

`LLI` also accepts labels as immediates:

```asm
LLI  LR, #my_label          ; R13 = address of my_label
```

### Arithmetic

| Instruction | Syntax              | Operation                          | Flags |
|-------------|---------------------|------------------------------------|:-----:|
| ADD         | `ADD Rd, Rs`        | `Rd = Rd + Rs`                     | NZCV  |
| ADD         | `ADD Rd, #imm16`    | `Rd = Rd + zero_extend(imm16)`     | NZCV  |
| SUB         | `SUB Rd, Rs`        | `Rd = Rd - Rs`                     | NZCV  |
| SUB         | `SUB Rd, #imm16`    | `Rd = Rd - zero_extend(imm16)`     | NZCV  |
| ADC         | `ADC Rd, Rs`        | `Rd = Rd + Rs + C` (add with carry)| NZCV  |
| SBC         | `SBC Rd, Rs`        | `Rd = Rd - Rs - ~C` (sub w/ borrow)| NZCV  |
| MUL / MULU  | `MUL Rd, Rs`        | `Rd = Rd * Rs` (signed / unsigned) | NZCV  |
| DIV / DIVU  | `DIV Rd, Rs`        | `Rd = Rd / Rs` (signed / unsigned) | NZCV  |
| MOD / MODU  | `MOD Rd, Rs`        | `Rd = Rd % Rs` (signed / unsigned) | NZCV  |

All ALU arithmetic is **2-operand destructive**: the first operand is
both a source and the destination. Save values you still need before
overwriting them.

The assembler automatically picks Format R (register-register) or
Format L (register-immediate) based on the second operand. `MUL`/`DIV`/
`MOD` are multi-cycle and stall the pipeline. They are currently
trapped as illegal instructions for software emulation; hardware
support will be added incrementally (see `CPU_ISA.HW_MUL`/`HW_DIV`).

### Logic

| Instruction | Syntax         | Operation         | Flags |
|-------------|----------------|-------------------|:-----:|
| AND         | `AND Rd, Rs`   | `Rd = Rd & Rs`    | NZCV  |
| OR          | `OR Rd, Rs`    | `Rd = Rd \| Rs`   | NZCV  |
| XOR         | `XOR Rd, Rs`   | `Rd = Rd ^ Rs`    | NZCV  |
| NOT         | `NOT Rd, Rs`   | `Rd = ~Rs`        | NZCV  |

`NOT` is the only unary ALU op. The source is `Rs`, not `Rd`.

### Shifts

| Instruction | Syntax           | Operation                              | Flags |
|-------------|------------------|----------------------------------------|:-----:|
| SHL         | `SHL Rd, Rs`     | `Rd = Rd << Rs[4:0]`                   | NZCV  |
| SHR         | `SHR Rd, Rs`     | `Rd = Rd >> Rs[4:0]` (logical)         | NZCV  |
| SAR         | `SAR Rd, Rs`     | `Rd = Rd >> Rs[4:0]` (arithmetic)      | NZCV  |
| SHL         | `SHL Rd, #imm5`  | immediate-shift variants (Format L)    | NZCV  |
| SHR         | `SHR Rd, #imm5`  |                                        | NZCV  |
| SAR         | `SAR Rd, #imm5`  |                                        | NZCV  |

Register shifts take the low 5 bits of `Rs` (0–31). Immediate shifts
encode the count in the Format L imm5 field.

### Comparison

| Instruction | Syntax              | Operation                                           | Flags |
|-------------|---------------------|-----------------------------------------------------|:-----:|
| CMP         | `CMP Rd, Rs`        | `flags = Rd - Rs` (Rd unchanged)                    | NZCV  |
| CMP         | `CMP Rd, #imm16`    | `flags = Rd - zero_extend(imm16)` (Rd unchanged)    | NZCV  |
| TEST        | `TEST Rd, Rs`       | `flags = Rd & Rs` (Rd unchanged)                    | NZCV  |

`CMP` and `TEST` are `SUB`/`AND` with the F-bit set, which suppresses
the register write. `CMP Rd, R0` is the idiomatic zero test since R0
is always 0.

### Memory Access

All memory operations use base-register + signed 16-bit offset.

| Instruction | Syntax                    | Operation                                |
|-------------|---------------------------|------------------------------------------|
| LDW         | `LDW Rd, [Rb + #off]`     | `Rd = mem32[Rb + sext(off)]`             |
| LDH / LDHS  | `LDH Rd, [Rb + #off]`     | zero- / sign-extended halfword           |
| LDB / LDBS  | `LDB Rd, [Rb + #off]`     | zero- / sign-extended byte               |
| STW         | `STW Rd, [Rb + #off]`     | `mem32[Rb + sext(off)] = Rd`             |
| STH         | `STH Rd, [Rb + #off]`     | store low halfword                       |
| STB         | `STB Rd, [Rb + #off]`     | store low byte                           |

- **Offset zero** may be omitted: `LDW R1, [R2]`.
- **Negative offsets** use subtraction syntax: `LDW R1, [R2 - #8]`.
- **Alignment.** Word accesses must be 4-byte aligned, halfword
  accesses 2-byte aligned. Misaligned accesses trap to `VEC_ALIGN`
  (vector 8).

Stack operations use SP (R14) as base:

```asm
; Push R1
SUB  SP, #4
STW  R1, [SP]

; Pop into R1
LDW  R1, [SP]
ADD  SP, #4
```

### Branches

| Instruction | Syntax        | Operation                                     |
|-------------|---------------|-----------------------------------------------|
| Bcc         | `Bcc target`  | Branch if condition `cc` is met               |
| B           | `B target`    | Unconditional branch                          |
| BL          | `BL target`   | Branch-and-link: `R13 = PC+4`, then branch    |
| JMP         | `JMP Rs`      | Indirect jump: `PC = Rs`                      |

Branch range: ±8 MB from the branch instruction. `BL` writes the
return address to R13 unconditionally — the target register is not
selectable.

### Subroutine Call and Return

```asm
BL   my_function            ; call
; ...

my_function:
    ; ... function body ...
    RET                      ; pseudo for JMP R13
```

For **nested calls**, save LR to the stack before calling further:

```asm
my_function:
    SUB  SP, #4
    STW  LR, [SP]            ; save return address

    BL   helper               ; nested call clobbers LR

    LDW  LR, [SP]            ; restore
    ADD  SP, #4
    RET
```

### System Instructions

| Instruction | Syntax                     | Operation                                | Privileged |
|-------------|----------------------------|------------------------------------------|:----------:|
| EI          | `EI`                       | `SR.I = 1` (one-instruction delay)       | No         |
| DI          | `DI`                       | `SR.I = 0` (immediate)                   | Yes        |
| WRSYS       | `WRSYS Rd, #dev, #reg`     | Write sysreg                             | Yes        |
| RDSYS       | `RDSYS Rd, #dev, #reg`     | Read sysreg                              | Yes        |
| RDSPR       | `RDSPR Rd, {ESR\|EPC\|USP\|SR}` | Read SPR                            | Yes        |
| WRSPR       | `WRSPR {ESR\|EPC\|USP\|SR}, Rd` | Write SPR                           | Yes        |
| ERET        | `ERET` **or** `ERET Rd, Rs`| Exception return                         | Yes        |
| SYSCALL     | `SYSCALL`                  | Trap to `VEC_SYSCALL` (5)                | No         |
| BREAK       | `BREAK`                    | Trap to `VEC_BREAK` (6)                  | No         |

**EI timing guarantee.** The instruction immediately after `EI` always
executes before any pending interrupt is recognised. This enables the
`EI; ERET` pattern without a race.

**DI timing guarantee.** Takes effect immediately. The next instruction
executes with interrupts disabled.

**ERET forms.** The no-argument form restores SR from `ESR` and PC
from `EPC` — the normal fault-return path. The two-argument form
`ERET Rd, Rs` loads SR from `Rd` and PC from `Rs` atomically — used
for context switches (returning to a **different** process than the
one that was interrupted).

**WRSYS/RDSYS.** Access device-mapped system registers (MMU, TLB,
sysid, caches, bus controller, timer). See
[sysregs.md](./sysregs.md).

---

## Pseudo-Instructions

| Pseudo | Expansion         | Notes                                      |
|--------|-------------------|--------------------------------------------|
| `NOP`  | `ADD R0, R0`      | No effect (R0 write discarded)             |
| `RET`  | `JMP R13`         | Return from subroutine                     |
| `LA Rd, label`  | `LLI Rd, #lo` + `LUI Rd, #hi` | Load 32-bit address |
| `LI Rd, #imm32` | `LLI Rd, #lo` + `LUI Rd, #hi` | Load 32-bit constant |

## Useful Idioms

| Pattern             | Code                                   | Notes                          |
|---------------------|----------------------------------------|--------------------------------|
| Clear register      | `MOV Rd, R0`                           |                                |
| Negate              | `NOT Rd, Rs` then `ADD Rd, #1`         | Two's complement: `-x = ~x+1`  |
| Test for zero       | `CMP Rd, R0` then `BZ target`          |                                |
| Countdown loop      | `SUB Rd, #1` then `BNZ loop`           | `SUB` sets Z flag              |
| 32-bit constant     | `LLI Rd, #lo` then `LUI Rd, #hi`       | Or use `LI`/`LA` pseudo-op     |
| Absolute jump       | `LLI Rd, #addr` (+`LUI`) then `JMP Rd` | For targets beyond branch range|

---

## Assembler Syntax

### Source Format

```asm
; Comments start with a semicolon. // is also accepted (LLVM convention).
label:                      ; labels end with colon
    ADD  R1, R2             ; assembler picks Format R
    ADD  R1, #5             ; assembler picks Format L
    CMP  R1, #10
    LDW  R3, [R4 + #8]      ; memory: [base + #offset] or [base - #offset]
    BEQ  label
    ERET
    .word 0xDEADBEEF
    .org  0x1000
    .equ  NAME, 0xFF
```

### Immediate Values

Immediates are prefixed with `#`:

```asm
LLI  R1, #42                ; decimal
LLI  R1, #0xFF              ; hexadecimal
LLI  R1, #0b1010            ; binary
LLI  R1, #-1                ; negative
LLI  R1, #my_label          ; label address
LLI  R1, #TLB_V             ; named constant (built-in or .equ)
```

### Directives

| Directive   | Example                 | Effect                              |
|-------------|-------------------------|-------------------------------------|
| `.org`      | `.org 0x1000`           | Set current address                 |
| `.word`     | `.word 0xDEADBEEF`      | Emit a raw 32-bit word              |
| `.byte`     | `.byte 0x42`            | Emit a raw 8-bit byte               |
| `.asciz`    | `.asciz "hi"`           | Emit null-terminated ASCII string   |
| `.equ`      | `.equ NAME, 0xFF`       | Define a named constant             |

---

## Further Reading

- **Bit-level encoding** — [instruction-encoding.md](./instruction-encoding.md)
  covers Format R/L/M/B field widths, opcode numbers, the condition-code
  encoding, and implementation status. Needed only for writing an
  assembler, disassembler, or binary tool.
- **Architectural context** — [architecture.md](./architecture.md) for
  the register model, SR, privilege, exception model, and vector table.
- **System registers** — [sysregs.md](./sysregs.md) for device/register
  numbers used by `WRSYS`/`RDSYS` and SPR numbering for `WRSPR`/`RDSPR`.

