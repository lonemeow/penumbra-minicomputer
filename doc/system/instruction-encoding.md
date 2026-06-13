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
| 00000 | ADD      | 01000 | MOV      | 10000 | MUL        |
| 00001 | SUB      | 01001 | NOT      | 10001 | MULU       |
| 00010 | AND      | 01010 | ADC      | 10010 | DIV        |
| 00011 | OR       | 01011 | SBC      | 10011 | DIVU       |
| 00100 | XOR      |       |          | 10111 | WRSYS      |
| 00101 | SHL      |       |          | 11000 | RDSYS      |
| 00110 | SHR      |       |          | 11001 | SYSCALL    |
| 00111 | SAR      |       |          | 11010 | BREAK      |
|       |          |       |          | 11011 | ERET       |
|       |          |       |          | 11100 | EI         |
|       |          |       |          | 11101 | DI         |
|       |          |       |          | 11110 | WRSPR      |
|       |          |       |          | 11111 | RDSPR      |

The op-bit-4 partition is intentional: **op[4]=0** is the **single-cycle
ALU region** (ADD/SUB/AND/OR/XOR/SHL/SHR/SAR/MOV/NOT/ADC/SBC plus 4
reserved slots `01100`–`01111` for future single-cycle additions like
CLZ, CTZ, BSWAP, POPCNT); **op[4]=1** is the **multi-cycle / system
region**, with peer-unit ops (MUL/MULU/DIV/DIVU) in the low quarter,
3 reserved slots (`10100`–`10110`) for future peer-unit ops (FPU
master opcode, crypto accelerator, etc.), and system ops at the top
(`10111`–`11111`). The micro-sequencer's dispatch formula
(`{0, op[4], 0, op[3:0], 0}`) inherits this partition directly — see
[datapath.md](../internals/penumbra1/datapath.md).

**Reserved sub-encoding: `WRSPR SR`.** `WRSPR` (op `11110`) with SPR
number `0011` (SR) in `[15:12]` is reserved and traps to `VEC_ILLEGAL`;
there is no direct SR write. `RDSPR SR` (op `11111`, same SPR field) is
valid — it reads the status register. The other SPR numbers
(`ESR`/`EPC`/`USP`/`SCR0–3`) are writable via `WRSPR`.

### Format R sub-encoding for MUL/DIV

`MUL`, `MULU`, `DIV`, and `DIVU` repurpose part of the spare field to
carry a **third register operand** `Rdh`, the high-half result
register. It is **write-only**. The F bit is unused for these opcodes
and must be 0.

```
31 30  29     25 24   21 20   17 16  15  12 11           0
[ 00 ][ op (5) ][ Rd (4) ][ Rs (4) ][0][ Rdh (4) ][ spare (12) ]
```

| Operation   | Behaviour                                                              |
|-------------|------------------------------------------------------------------------|
| MUL, MULU   | `Rdh:Rd = Rd × Rs`  (low half → Rd, high half → Rdh)                   |
| DIV, DIVU   | `Rd, Rdh = Rd / Rs, Rd % Rs` (32/32: quotient → Rd, remainder → Rdh) |

`Rdh` is write-only for all four opcodes — there is no high-half
*input*. `DIV`/`DIVU` are always 32/32: the dividend is the 32-bit `Rd`,
and `Rdh` receives the remainder. Setting `Rdh = R0` discards the
high-half result — writes to R0 are silently dropped — so the product
high half (`MUL`) or remainder (`DIV`) simply vanishes when not needed.

The assembler defaults the third operand to `R0` when omitted, so
the common 32-bit forms look exactly like 2-operand arithmetic:

```asm
MUL  R1, R2              ; encoded as MUL R1, R2, R0 — low 32 bits only
MUL  R1, R2, R3          ; full 64-bit product: R3:R1 = R1 × R2
DIV  R1, R2              ; 32/32: R1 = R1/R2, remainder discarded
DIV  R1, R2, R3          ; 32/32: quotient → R1, remainder → R3
```

There is no 64/32 narrowing-divide form — no mnemonic carries a
dividend high half, because the unit has no high-half input. See the
[instruction set guide](./instruction-set.md#muldiv-register-pair-semantics)
for the rationale (Penumbra has a real 32×32→64 `MUL`, so it never
needs a narrowing divide to undo a widening multiply).

`Rdh[15:12]` reuses the same bit positions that hold the SPR number
(`RDSPR`/`WRSPR`) and device number (`WRSYS`/`RDSYS`) — the existing
IR field extractor serves all three roles. Bits `[11:0]` of the
instruction remain spare.

There is no `MOD` opcode. To compute a remainder, use `DIV` with a
scratch register as `Rd` (which receives the discarded quotient) and
the desired remainder destination as `Rdh`:

```asm
MOV  R5, R1              ; if R1's value must survive
DIV  R5, R2, R1          ; R5 = R1/R2 (discarded), R1 = R1 % R2
```

If the dividend register can be clobbered, the `MOV` is unnecessary —
just pass the dividend as `Rd` and the remainder destination as `Rdh`.

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
| 1100 | JALR      | `R13 = PC+4; PC = Rd` (indirect call, Rd captured *before* R13 write) |
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

Conditions `0001`–`1110` are paired: each condition's inverse is the
numerically **adjacent code** — odd `n` pairs with `n+1` (`BEQ`=0001
↔ `BNE`=0010, …, `BVS`=0111 ↔ `BVC`=1000). Inverse pairs do *not*
differ in a single bit, so `cond ^ 1` is not an inversion (it maps
`BEQ` to `B`); tools derive the inverse with an explicit table or the
odd/even adjacency rule, and the hardware evaluates conditions as a
plain 16-way decode. `BL` (cond=`1111`) is always taken and writes
PC+4 into R13 before branching.

---

## Implementation Status

Instructions marked **Yes** are implemented in hardware and pass
simulation tests. Opcodes not listed here are unallocated and dispatch
to the illegal-instruction handler (`VEC_ILLEGAL`).

| Instruction                       | Implemented | Notes                               |
|-----------------------------------|:-----------:|-------------------------------------|
| ADD, SUB, AND, OR, XOR            | Yes         | Single-cycle ALU ops                |
| SHL, SHR, SAR                     | Yes         | Register and immediate forms        |
| ADC, SBC                          | Yes         | Multi-word arithmetic               |
| MOV                               | Yes         | No flag update                      |
| NOT                               | Yes         | Updates flags                       |
| CMP, TEST                         | Yes         | SUB/AND with F-bit                  |
| MUL, MULU                         | Yes         | Hardware `divmul` peer unit; 32×32→64, high half → optional `Rdh` |
| DIV, DIVU                         | Yes         | Hardware `divmul` peer unit; 32/32, remainder → optional `Rdh`; DIV0 → `VEC_ARITH` |
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
| RDSPR                             | Yes         | SPR in IR[15:12]: ESR/EPC/USP/SR/SCR0–3 |
| WRSPR                             | Yes         | SPR in IR[15:12]: ESR/EPC/USP/SCR0–3. `WRSPR SR` (SPR 3) reserved → `VEC_ILLEGAL` |
| ERET                              | Yes         | No-operand form only; context switch = `WRSPR EPC/ESR` + `ERET` |
| SYSCALL, BREAK                    | Yes         |                                     |
| NOP, RET, LA, LI (pseudo)         | Yes         |                                     |
