# Penumbra Instruction Encoding Reference

Bit-level reference for authors of assemblers, disassemblers, debuggers,
binary tools, and anyone who needs the exact on-disk form of Penumbra
instructions.

If you just want to **write** Penumbra assembly or target the ISA from
a compiler back-end, use [instruction-set.md](./instruction-set.md)
instead — this document is the machine-level reference, not the
programmer's manual.

All instructions are 32 bits. Bits `[31:30]` select one of four formats.

| Prefix | Format | Purpose                                                     |
|:------:|:------:|-------------------------------------------------------------|
| `00`   | R      | Register-register ALU and system operations                 |
| `01`   | L      | Immediate operations (load imm, add/sub/cmp/and/test #imm, imm-shifts) |
| `10`   | M      | Memory load/store with register + signed 16-bit offset       |
| `11`   | B      | Branch (conditional, unconditional, branch-and-link)         |

---

## Format R — Register Operations (prefix `00`)

```
31 30  29     25 24   21 20   17 16  15             0
[ 00 ][ op (5) ][ Rd (4) ][ Rs (4) ][F][ spare (15) ]
```

| Field  | Bits  | Description                                                |
|--------|:-----:|------------------------------------------------------------|
| prefix | 31:30 | `00` — Format R                                            |
| op     | 29:25 | Operation (32 opcodes; see table)                          |
| Rd     | 24:21 | Destination and first source register                      |
| Rs     | 20:17 | Second source register                                     |
| F      | 16    | Flag-only bit: 0 = write result + flags; 1 = flags only    |
| spare  | 15:0  | Reserved. For `WRSYS`/`RDSYS`: `[15:12]` = device, `[11:8]` = register. For `WRSPR`/`RDSPR`: `[15:12]` = SPR number. |

**`F` bit usage.** `CMP` is encoded as `SUB` with `F=1`; `TEST` is
`AND` with `F=1`. The ALU computes normally, flags update, but the
register write is suppressed.

### Format R opcodes

| op    | Mnemonic | op    | Mnemonic | op    | Mnemonic   |
|:-----:|----------|:-----:|----------|:-----:|------------|
| 00000 | ADD      | 01000 | MOV      | 10111 | WRSYS      |
| 00001 | SUB      | 01001 | NOT      | 11000 | RDSYS      |
| 00010 | AND      | 01010 | ADC      | 11001 | SYSCALL    |
| 00011 | OR       | 01011 | SBC      | 11010 | BREAK      |
| 00100 | XOR      | 01100 | MUL      | 11011 | ERET       |
| 00101 | SHL      | 01101 | MULU     | 11100 | EI         |
| 00110 | SHR      | 01110 | DIV      | 11101 | DI         |
| 00111 | SAR      | 01111 | DIVU     | 11110 | WRSPR      |
|       |          | 10000 | MOD      | 11111 | RDSPR      |
|       |          | 10001 | MODU     |       |            |

Opcodes 10010–10110 are reserved for future ALU expansion.

---

## Format L — Immediate Operations (prefix `01`)

```
31 30  29  26 25   22 21       16 15             0
[ 01 ][ op(4) ][ Rd(4) ][ spare(6) ][ imm16 (16) ]
```

| Field  | Bits  | Description                     |
|--------|:-----:|---------------------------------|
| prefix | 31:30 | `01` — Format L                 |
| op     | 29:26 | Immediate operation (16 opcodes)|
| Rd     | 25:22 | Destination register            |
| spare  | 21:16 | Reserved (6 bits)               |
| imm16  | 15:0  | 16-bit immediate                |

### Format L opcodes

| op   | Mnemonic  | Operation                                          |
|:----:|-----------|----------------------------------------------------|
| 0000 | LLI       | `Rd = zero_extend(imm16)`                          |
| 0001 | LLIS      | `Rd = sign_extend(imm16)`                          |
| 0010 | LUI       | `Rd = Rd \| (imm16 << 16)`                         |
| 0011 | ADD #imm  | `Rd = Rd + zero_extend(imm16)`                     |
| 0100 | SUB #imm  | `Rd = Rd - zero_extend(imm16)`                     |
| 0101 | CMP #imm  | `flags = Rd - zero_extend(imm16)` (no write)       |
| 0110 | AND #imm  | `Rd = Rd & zero_extend(imm16)`                     |
| 0111 | TEST #imm | `flags = Rd & zero_extend(imm16)` (no write)       |
| 1000 | SHL #imm  | `Rd = Rd << imm[4:0]`                              |
| 1001 | SHR #imm  | `Rd = Rd >> imm[4:0]` (logical)                    |
| 1010 | SAR #imm  | `Rd = Rd >> imm[4:0]` (arithmetic)                 |
| 1011 | JMP       | `PC = Rd` (Rd field carries source register)       |
| 1100 | JALR      | `R13 = PC+4; PC = Rd` (indirect call)              |
| 1101–1111 | —     | Reserved                                           |

---

## Format M — Memory Load/Store (prefix `10`)

```
31 30  29  28 27  26 25   22 21   18 17              2 1  0
[ 10 ][ L ][ sz ][SE][ Rd(4) ][ Rb(4) ][ offset16 (16) ][sp]
```

| Field    | Bits  | Description                                            |
|----------|:-----:|--------------------------------------------------------|
| prefix   | 31:30 | `10` — Format M                                        |
| L        | 29    | 1 = load, 0 = store                                    |
| sz       | 28:27 | Size: 00 = byte, 01 = halfword, 10 = word              |
| SE       | 26    | Sign-extend on load (ignored for stores and word loads)|
| Rd       | 25:22 | Data register (destination for loads, source for stores)|
| Rb       | 21:18 | Base address register                                  |
| offset16 | 17:2  | 16-bit signed byte offset                              |
| sp       | 1:0   | Spare (2 bits)                                         |

Effective address: `EA = Rb + sign_extend(offset16)`.
Range: −32768 to +32767 bytes.

### Load/Store encoding

| L | sz | SE | Mnemonic | Operation                     |
|:-:|:--:|:--:|----------|-------------------------------|
| 1 | 10 | 0  | LDW      | `Rd = mem32[EA]`              |
| 1 | 01 | 0  | LDH      | zero-extended halfword        |
| 1 | 01 | 1  | LDHS     | sign-extended halfword        |
| 1 | 00 | 0  | LDB      | zero-extended byte            |
| 1 | 00 | 1  | LDBS     | sign-extended byte            |
| 0 | 10 | x  | STW      | `mem32[EA] = Rd`              |
| 0 | 01 | x  | STH      | `mem16[EA] = Rd[15:0]`        |
| 0 | 00 | x  | STB      | `mem8[EA] = Rd[7:0]`          |

---

## Format B — Branch (prefix `11`)

```
31 30  29     26 25                           4 3    0
[ 11 ][ cond(4) ][       offset22 (22)         ][ sp ]
```

| Field    | Bits  | Description                                       |
|----------|:-----:|---------------------------------------------------|
| prefix   | 31:30 | `11` — Format B                                   |
| cond     | 29:26 | Condition code (see table)                        |
| offset22 | 25:4  | 22-bit signed word offset from the branch itself  |
| sp       | 3:0   | Spare (4 bits)                                    |

Target: `PC + sign_extend(offset22 << 2)`. Range: ±8 MB.

The offset is relative to the **branch instruction itself** (not PC+4).
The assembler encodes `offset22 = (target - PC) >> 2`. This avoids an
extra adder stage in hardware — important for the discrete build.

### Condition codes

| cond | Mnemonic       | Meaning                  | Flags tested       |
|:----:|----------------|--------------------------|--------------------|
| 0000 | `B` (AL)       | Always (unconditional)   | —                  |
| 0001 | `BEQ`          | Equal / zero             | Z=1                |
| 0010 | `BNE`          | Not equal                | Z=0                |
| 0011 | `BCS` / `BHS`  | Carry set / unsigned ≥   | C=1                |
| 0100 | `BCC` / `BLO`  | Carry clear / unsigned < | C=0                |
| 0101 | `BMI`          | Minus / negative         | N=1                |
| 0110 | `BPL`          | Plus / positive or zero  | N=0                |
| 0111 | `BVS`          | Overflow set             | V=1                |
| 1000 | `BVC`          | Overflow clear           | V=0                |
| 1001 | `BHI`          | Unsigned >               | C=1 & Z=0          |
| 1010 | `BLS`          | Unsigned ≤               | C=0 \| Z=1         |
| 1011 | `BGE`          | Signed ≥                 | N=V                |
| 1100 | `BLT`          | Signed <                 | N≠V                |
| 1101 | `BGT`          | Signed >                 | Z=0 & N=V          |
| 1110 | `BLE`          | Signed ≤                 | Z=1 \| N≠V         |
| 1111 | `BL`           | Branch-and-link (always) | — (saves PC+4→R13) |

Conditions `0001`–`1110` are paired: each condition and its inverse
differ only in bit 0, allowing the condition-evaluation logic to be
`base_result XOR cond[0]`. `BL` (cond=`1111`) is always taken; the
microcode writes PC+4 into R13 before branching.

---

## Implementation Status

Instructions marked **Yes** have working microcode and pass simulation
tests. Instructions marked **Trap** dispatch to the illegal-instruction
handler for software emulation.

| Instruction                       | Implemented | Notes                               |
|-----------------------------------|:-----------:|-------------------------------------|
| ADD, SUB, AND, OR, XOR            | Yes         | Single-cycle ALU ops                |
| SHL, SHR, SAR                     | Yes         | Register and immediate forms        |
| ADC, SBC                          | Yes         | Multi-word arithmetic               |
| MOV                               | Yes         | No flag update                      |
| NOT                               | Yes         | Updates flags                       |
| CMP, TEST                         | Yes         | SUB/AND with F-bit                  |
| MUL, MULU, DIV, DIVU, MOD, MODU   | Trap        | Illegal-instr trap → SW emulation   |
| LLI, LLIS, LUI                    | Yes         |                                     |
| ADD/SUB/CMP/AND/TEST #imm         | Yes         | Format L encoding                   |
| LDW, STW                          | Yes         | Stall-based, latency-agnostic       |
| LDH/LDHS/LDB/LDBS/STH/STB         | Yes         | `byte_ext`/`byte_rep` + `byte_en` lanes |
| B, Bcc (all 16 conditions)        | Yes         |                                     |
| BL                                | Yes         | Writes PC+4 to R13 in microcode     |
| JMP (RET)                         | Yes         |                                     |
| JALR                              | Yes         | Indirect call                       |
| EI, DI                            | Yes         | `ei_shadow`, privilege check        |
| WRSYS, RDSYS                      | Yes         | Privileged                          |
| RDSPR, WRSPR                      | Yes         | SPR in IR[15:12]: ESR/EPC/USP/SR/SCR0–3 |
| ERET (1- and 2-arg)               | Yes         |                                     |
| SYSCALL, BREAK                    | Yes         |                                     |
| NOP, RET, LA, LI (pseudo)         | Yes         |                                     |
