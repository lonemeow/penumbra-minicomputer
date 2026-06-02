# Penumbra FPU — ISA Design

A forward-looking ISA design for a hardware floating-point unit. This
document defines how floating point sits in the Penumbra ISA: the
register model, encoding, instruction set, and IEEE semantics. It does
**not** specify RTL — datapath details (mantissa width, normalize
shifter, iteration counts) are left to an implementation.

The guiding principle is **IEEE-754 correctness first**. Where
correctness conflicts with performance or implementation simplicity,
correctness wins. The unit produces correctly-rounded results for every
operation IEEE requires, handles subnormals (no flush-to-zero), and
propagates NaNs and infinities per the standard.

## Scope

- **Double-first.** Generic C code uses `double` everywhere (literals,
  `math.h` returns, `printf`/`scanf`, `awk`, `bc`), so `double` is the
  primary target; `float` rides along at lower cost. Both are hardware.
- **Operations:** add, subtract, multiply, divide, square root, fused
  multiply-add (4 sign variants), conversions, compare, min/max,
  classify. These are the correctly-rounded IEEE operations (`+ − × ÷ √
  fma`) plus the support operations real code needs.
- **No 80-bit extended precision.** `long double == double`, matching
  the ABI.

## Register Model — floats live in the GPRs (Zfinx/Zdinx style)

Floating-point values occupy the **general-purpose registers**, not a
separate FP register file. This is RISC-V's `Zfinx` model for single
precision and `Zdinx` for double.

- A `float` (binary32) occupies one GPR.
- A `double` (binary64) occupies an **aligned GPR pair**: the named
  register holds bits `[31:0]`, the next register holds bits `[63:32]`
  (sign and exponent are in the high register). The ABI's existing
  64-bit pair rules apply.

Consequences:

- **No FP load/store instructions** — a `float` is a 32-bit word loaded
  with `LDW`; a `double` is two `LDW`s. Memory traffic reuses the
  existing memory format.
- **No GPR↔FP move instructions** — there is no second register file to
  move between. The value an FP op produces is already in a GPR for the
  next integer op to consume.
- **Context switch is nearly free** — FP state *is* the GPRs, which the
  kernel already saves. The only added per-thread state is `FPCSR` (see
  below): one register.

The trade is register pressure — a live `double` consumes two of the
~11 allocatable GPRs — and that double operations read four source words
and write two. For the general-use workloads Penumbra targets (a `double`
or two live at a time, not tight numeric kernels) the pressure is
tolerable, and the wider operand traffic is absorbed by the multi-cycle
peer-unit protocol (operands are latched across cycles before iteration;
the 64-bit result is written back as two registers, exactly as the
divmul peer already writes its low/high halves).

## Peer-unit model

The FPU is a **peer to the ALU**, on the same register-bus fabric as the
divmul unit, and uses the same micro-sequencer protocol: a one-cycle
start pulse latches operands, `o_busy` holds through iteration, the
unified STALL gate ORs `fpu_busy` with the other busy sources, and
results reach the register file through the `wb_src` writeback mux. A
64-bit (`double`) result writes back over two cycles (low register then
high register) — structurally identical to divmul's `DML_LO`/`DML_HI`
tail.

## Encoding

The FPU claims one Format-R **master opcode** from the reserved
peer-unit region (`10100`; see
[instruction-encoding.md](../../system/instruction-encoding.md)). A
sub-operation field in the spare bits selects the specific FP operation,
exactly as `MUL`/`DIV` repurpose the spare field to carry `Rdh`.

```
31 30  29     25 24   21 20   17 16  15  12 11 10    7 6    4 3    0
[ 00 ][ 10100 ][  Rd  ][  Rs  ][F][ fop ][P][  Rt  ][  rm  ][ cvt  ]
```

| Field | Bits  | Description                                                    |
|-------|:-----:|----------------------------------------------------------------|
| op    | 29:25 | `10100` — FPU master opcode                                    |
| Rd    | 24:21 | Destination + first source (low register of a `double` pair)   |
| Rs    | 20:17 | Second source (low register of a `double` pair)                |
| F     | 16    | Flag-only bit. On `FSUB` selects `FCMP` (set flags, no write). Must be 0 for all other `fop`. |
| fop   | 15:12 | FP sub-operation (table below)                                 |
| P     | 11    | Precision: 0 = single (binary32), 1 = double (binary64)        |
| Rt    | 10:7  | Third source register (FMA family; ignored otherwise)          |
| rm    | 6:4   | Rounding mode: 0 = dynamic (use `FPCSR`), else static (below)   |
| cvt   | 3:0   | Convert type selector for `FCVT`; spare otherwise              |

The 16-bit spare field comfortably holds the whole FP ISA in one master
opcode, leaving the other two reserved peer-unit slots (`10101`,
`10110`) free.

### Sub-operations (`fop`)

| fop  | Mnemonic | Operation                          | Notes                          |
|:----:|----------|------------------------------------|--------------------------------|
| 0000 | FADD     | `Rd = Rd + Rs`                     |                                |
| 0001 | FSUB     | `Rd = Rd − Rs`                     | **F=1 ⇒ `FCMP`** (flags only)  |
| 0010 | FMUL     | `Rd = Rd × Rs`                     |                                |
| 0011 | FDIV     | `Rd = Rd ÷ Rs`                     | correctly rounded              |
| 0100 | FSQRT    | `Rd = √Rs`                         | unary; correctly rounded       |
| 0101 | FMADD    | `Rd = (Rd × Rs) + Rt`              | single rounding                |
| 0110 | FMSUB    | `Rd = (Rd × Rs) − Rt`              | single rounding                |
| 0111 | FNMADD   | `Rd = −(Rd × Rs) − Rt`             | single rounding                |
| 1000 | FNMSUB   | `Rd = −(Rd × Rs) + Rt`             | single rounding                |
| 1001 | FCVT     | type conversion (`cvt` field)      | see Conversions                |
| 1010 | FMIN     | `Rd = min(Rd, Rs)`                 | IEEE minNum semantics          |
| 1011 | FMAX     | `Rd = max(Rd, Rs)`                 | IEEE maxNum semantics          |
| 1100 | FCLASS   | `Rd = class(Rs)` (mask → GPR)      | IEEE classification            |
| 1101–1111 | —   | reserved                           |                                |

`FSQRT` is a single instruction (not an estimate + Newton-Raphson step):
the divide already requires an iterative datapath, square root shares
that structure, and only a full digit-recurrence (or equivalent)
implementation gives the correctly-rounded result IEEE requires.
Estimate/step instructions (`frsqrte`-style) produce approximations that
still need a software correction for last-bit accuracy, so they would be
more total work for a less-correct result.

`FABS`, `FNEG`, and `FCOPYSIGN` are **not** FP opcodes. They are pure
sign-bit manipulation (bit 31 of the value, which for a `double` is bit
31 of the *high* register) and are expressed with integer logic ops on
the GPR holding the sign — no peer-unit round trip. The reserved
single-cycle ALU slots (`01100`–`01111`) are the natural home if a
dedicated sign op is ever wanted to avoid materializing the mask.

### Static rounding modes (`rm`)

| rm  | Mode                                   |
|:---:|----------------------------------------|
| 000 | dynamic — use `FPCSR.frm`              |
| 001 | RNE — round to nearest, ties to even   |
| 010 | RTZ — round toward zero (truncate)     |
| 011 | RDN — round toward −∞                  |
| 100 | RUP — round toward +∞                  |
| others | reserved                            |

Most code emits `rm = 000` (dynamic). The static field exists so the
compiler can pin a mode without touching `FPCSR` — most importantly,
C casts from float to integer truncate, so `FCVT`-to-integer for a cast
emits static `RTZ` regardless of the dynamic mode.

### Conversions (`FCVT`, `cvt` field)

The `cvt` field encodes source and destination types as `{dst[1:0],
src[1:0]}` with type codes `00 = i32`, `01 = u32`, `10 = f32`, `11 =
f64`. The `P` precision bit is ignored for `FCVT`. Meaningful
conversions:

| From → To        | Notes                                            |
|------------------|--------------------------------------------------|
| i32/u32 → f32/f64 | integer to float (f64 from i32/u32 is exact)    |
| f32/f64 → i32/u32 | float to integer; honors `rm` (RTZ for C casts) |
| f32 → f64         | widen, exact                                     |
| f64 → f32         | narrow, correctly rounded                        |

Out-of-range float→integer conversions and NaN inputs follow IEEE/C
semantics (saturate to the integer min/max and set the invalid flag);
the exact result table is specified with the implementation.

## Compare and branch (`FCMP`)

Penumbra has a real condition-flag register (NZCV) and 16 conditional
branches. An FP compare therefore sets the **integer condition flags**
and reuses the existing `Bcc` instructions — there is no separate FP
condition flag and no new branch opcode. This mirrors `CMP` (which is
`SUB` with `F=1`): `FCMP` is **`FSUB` with `F=1`** — the subtract is
performed for its ordering result, the flags are set, and the register
write is suppressed.

The wrinkle FP adds over integer compare is the **unordered** relation:
if either operand is NaN, the result is *unordered* — none of `<`, `=`,
`>` hold. So an FP compare produces **four** mutually-exclusive
outcomes (LT, EQ, GT, UN) that must be encoded into the four flags such
that the existing `Bcc` conditions evaluate to the right branch
decisions. ARM solves this for its `VCMP` (it sets NZCV so the standard
condition codes work, with unordered distinguished on `C`/`V`); Penumbra
should adopt the same approach, choosing the mapping that best fits its
own flag set and `Bcc` truth tables.

<!-- TODO(human): define the FCMP flag-setting truth table -->

| Relation (Rd vs Rs) | N | Z | C | V | Which `Bcc` conditions become true |
|---------------------|:-:|:-:|:-:|:-:|------------------------------------|
| Rd < Rs   (LT)      | ? | ? | ? | ? | ?                                  |
| Rd = Rs   (EQ)      | ? | ? | ? | ? | ?                                  |
| Rd > Rs   (GT)      | ? | ? | ? | ? | ?                                  |
| unordered (UN)      | ? | ? | ? | ? | ?                                  |

`FCMP` is the *quiet* compare (signals invalid only on a signaling NaN).
A signaling ordered compare, if wanted, is a separate variant — not
expressible through the `F` bit, which is already spent selecting
compare-vs-arithmetic.

## FPCSR — control/status register

Floating-point control and status live in one CPU-internal register,
`FPCSR`, accessed via `RDSPR`/`WRSPR` as **SPR 8** (one of the reserved
SPR slots). Layout follows RISC-V's `fcsr`:

```
 31                        8 7   5 4   3   2   1   0
┌──── reserved (0) ─────────┬ frm ┬ NV ┬ DZ ┬ OF ┬ UF ┬ NX ┐
└───────────────────────────┴─────┴────┴────┴────┴────┴────┘
```

| Bits | Field | Description                                            |
|:----:|:-----:|--------------------------------------------------------|
| 7:5  | frm   | Dynamic rounding mode (same codes as static `rm` 001–100, biased) |
| 4    | NV    | Invalid operation (sticky)                             |
| 3    | DZ    | Divide by zero (sticky)                                |
| 2    | OF    | Overflow (sticky)                                      |
| 1    | UF    | Underflow (sticky)                                     |
| 0    | NX    | Inexact (sticky)                                       |

The five exception flags are **sticky/accrued**: an operation that
raises an exception ORs its bit in; software clears them. This is
IEEE-754 *default* exception handling — the operation produces the
standard result (correctly-rounded value, ±∞, or NaN) and continues. It
is fully conformant; trapping is recommended by the standard but not
required.

`FPCSR` is the only piece of FP state outside the GPRs, so it is the
only FP register the kernel must save/restore on a context switch.

### No FP exception trapping (in this design)

There is no `VEC_FP` vector and no per-exception trap-enable bits. All FP
exceptions are handled by the default sticky-flag mechanism. Trap-enable
bits plus a `VEC_FP` vector are a clean future extension if signaling
behavior is ever wanted; nothing here precludes adding them.

## IEEE-754 conformance notes

The design commits to the following, all required for conformance and
none negotiable against performance:

- **Correctly-rounded results** for `+ − × ÷ √` and `fma` (the fused
  product is computed to full width and rounded once).
- **Subnormals** are supported with gradual underflow. Flush-to-zero is
  *not* permitted — it is the most common conformance shortcut and is
  explicitly excluded here.
- **NaN handling:** quiet and signaling NaNs are distinguished;
  operations propagate quiet NaNs and signal invalid on signaling NaNs
  and invalid operations (e.g. `0 × ∞`, `∞ − ∞`, `√negative`).
- **Signed zero and infinities** behave per the standard.
- **All four binary rounding modes** (RNE, RTZ, RDN, RUP) are
  implemented; RNE is the reset default.

## Comparison to other simple RISCs

| Architecture | FP registers | GPR↔FP bridge | FP compare → branch | sqrt |
|--------------|--------------|---------------|---------------------|------|
| MIPS (CP1)   | separate ×32 | `mtc1`/`mfc1`, `lwc1`/`swc1` | FCSR cond bit + `bc1t/f` | full `sqrt` |
| RISC-V F/D   | separate ×32 | `fmv`, `flw`/`fsw` | compare writes 0/1 to GPR | full `fsqrt` |
| RISC-V Zfinx/Zdinx | **none — GPRs** | none (reuse `lw`/`sw`) | compare → GPR | full | 
| ARM VFP/M4F  | separate     | `vmov`, `vldr`/`vstr` | FPSCR → APSR NZCV, then `Bcc` | full / M4F single-only |
| **Penumbra (this design)** | **none — GPRs (Zdinx)** | **none** | **`FCMP` sets NZCV, reuse `Bcc`** | **full `FSQRT`** |

Penumbra's design is closest to RISC-V `Zdinx` (FP in the integer
registers) but diverges on compare: where RISC-V writes a 0/1 boolean to
a GPR (because it has no condition-flag register), Penumbra already has
NZCV and 16 conditional branches, so `FCMP` sets the flags and reuses
the existing branch machinery — the ARM approach, on the RISC-V register
model.

## Implementation Status

- **ISA spec:** this document — a forward design, not yet ratified into
  the frozen v1 ISA. The master opcode slot (`10100`) and SPR 8 are
  reserved for it.
- **Microcode / RTL / ISS / LLVM:** none. Floating point is software
  (soft-float libcalls) today; this document describes the hardware ISA
  that would replace those libcalls.

## See Also

- [instruction-encoding.md](../../system/instruction-encoding.md) —
  Format R opcode partition, reserved peer-unit slots
- [divmul.md](../divmul.md) — the peer-unit protocol this design reuses
- [architecture.md](../../system/architecture.md) — SPR numbering,
  condition codes, exception model
- [abi.md](../../system/abi.md) — 64-bit register-pair rules, data model
