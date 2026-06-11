# Penumbra ABI Specification

**Version:** 0.1 (draft)
**Target triple:** `penumbra-unknown-netbsd` (bare-metal: `penumbra-unknown-none`)
**ELF machine:** `EM_PENUMBRA` (0xF0DA, private range)

This document defines the Application Binary Interface for the Penumbra architecture. It is the contract between the compiler, linker, and operating system. All compiled code must conform to this specification to ensure interoperability.

---

## 1. Data Model

Penumbra uses the **ILP32** data model.

| C type | Size (bytes) | Alignment (bytes) |
|--------|-------------:|-------------------:|
| `_Bool` / `bool` | 1 | 1 |
| `char` | 1 | 1 |
| `short` | 2 | 2 |
| `int` | 4 | 4 |
| `long` | 4 | 4 |
| `long long` | 8 | 8 |
| `float` | 4 | 4 |
| `double` | 8 | 8 |
| `long double` | 8 | 8 |
| pointer | 4 | 4 |
| `size_t` | 4 | 4 |
| `ptrdiff_t` | 4 | 4 |
| `wchar_t` | 4 | 4 |

- `char` is **signed** by default (matches x86, RISC-V, MIPS, and SPARC — the convention most portable software assumes). The ISA provides both zero- and sign-extending byte loads (`LDB`/`LDBS`), so the choice is performance-neutral for loads.
- Bit-fields are packed LSB-first within their storage unit.
- `long double` is the same as `double` (no extended precision — no FPU).

### Byte Order

**Little-endian.** Address A+0 holds bits [7:0] of a word at address A.

### Alignment

All data types are naturally aligned. The hardware traps on misaligned word and halfword accesses (alignment fault, vector 8). The compiler must ensure correct alignment; there is no software misalignment handler in the default runtime.

Stack pointer must be 4-byte aligned at all times.

The 8-byte alignment of 64-bit types (matching the ARM EABI / RISC-V
ILP32 convention) governs struct layout and static/heap placement.
Because the stack guarantees only 4-byte alignment, automatic 64-bit
objects may in practice be 4-byte aligned. This is harmless: no
Penumbra memory access is wider than one 32-bit word, so 64-bit types
are always read and written as two word halves and never require
8-byte alignment for correctness.

---

## 2. Register Convention

Penumbra has 16 general-purpose 32-bit registers (R0–R15).

| Register | Alias | Role | Preserved across calls? |
|----------|-------|------|:-----------------------:|
| R0 | `zero` | Hardwired zero | — (constant) |
| R1 | `a1` | Argument 1 / return value | No (caller-saved) |
| R2 | `a2` | Argument 2 / return value (high) | No (caller-saved) |
| R3 | `a3` | Argument 3 | No (caller-saved) |
| R4 | `a4` | Argument 4 | No (caller-saved) |
| R5 | `s1` | Callee-saved | **Yes** |
| R6 | `s2` | Callee-saved | **Yes** |
| R7 | `s3` | Callee-saved | **Yes** |
| R8 | `s4` | Callee-saved | **Yes** |
| R9 | `s5` | Callee-saved | **Yes** |
| R10 | `s6` / `fp` | Callee-saved / frame pointer | **Yes** |
| R11 | `t1` | Temporary / scratch | No (caller-saved) |
| R12 | `tp` | Thread pointer (reserved) | — (not allocatable) |
| R13 | `lr` | Link register (set by BL) | No (caller-saved) |
| R14 | `sp` | Stack pointer (banked USP/SSP) | **Yes** |
| R15 | `pc` | Program counter (read-only) | — (not writable) |

**Summary:**
- **Caller-saved (volatile):** R1–R4, R11, R13
- **Callee-saved (non-volatile):** R5–R10, R14
- **Reserved:** R0 (zero), R12 (thread pointer), R15 (PC)
- R12 is reserved as the thread pointer for TLS. The kernel/runtime sets it on context switch; compiled code must not modify it. On bare-metal targets without threads, R12 is unused but still not allocatable (forward compatibility).
- R13 is caller-saved because BL overwrites it. Non-leaf functions must save R13 in their prologue before any nested call.
- Frame pointer (when needed) uses R10 (`fp`), the highest callee-saved register.

### Condition Flags

The status register flags (N, Z, C, V) are **caller-saved** — they are not preserved across function calls. A callee may freely clobber flags.

---

## 3. Function Calling Convention

### Argument Passing

Arguments are assigned, left to right, to a sequence of 4-byte
**argument slots**. The first four slots are R1, R2, R3, R4; further
slots are 4-byte stack words at increasing addresses, with slot 5 at
`[SP + 0]` as seen at the call instruction. A slot is never skipped
for alignment: every argument occupies the next free slot(s), and a
two-slot argument may straddle the register/stack boundary (first
half in R4, second half in the first stack slot).

| Argument type | Slots | Contents |
|---------------|:-----:|----------|
| scalar ≤ 4 bytes (integers, pointers, `float`) | 1 | value; sub-word types widened to 32 bits per their signedness |
| 64-bit scalar (`long long`, `double`, `long double`) | 2 | low word first, high word second |
| aggregate ≤ 4 bytes | 1 | the aggregate's memory image in the slot's low-order bytes; remaining bytes undefined |
| aggregate 5–8 bytes | 2 | the aggregate's memory image; first slot = lower-addressed word |
| aggregate > 8 bytes | 1 | pointer to a caller-owned temporary copy |

Aggregates (structs and unions) are classified by size alone; field
types and declared alignment do not affect slot assignment.
Zero-sized aggregates (a GNU C extension) occupy no slot.

For aggregates passed by reference (> 8 bytes), the caller allocates
a temporary copy of the argument value in its own frame and passes
the copy's address. The callee may modify the copy freely; the copy
is dead once the call returns. The temporary carries the alignment
guarantees of any automatic object of its type. In all size classes,
by-value semantics hold: **a callee's writes to its parameter are
never visible in the caller's argument object.**

**Variadic functions.** Anonymous (variadic) arguments use the same
slot assignment as named arguments. A variadic callee spills R1–R4
into a save area placed directly below its incoming stack arguments,
forming one contiguous slot array; `va_list` is a pointer that walks
this array.

### Return Values

| Returned type | Location |
|---------------|----------|
| scalar ≤ 4 bytes | R1 |
| 64-bit scalar | R1 = low word, R2 = high word |
| aggregate ≤ 4 bytes | R1 (memory image, as for arguments) |
| aggregate 5–8 bytes | R1 = first word, R2 = second word |
| aggregate > 8 bytes | hidden result pointer (see below) |

For aggregate returns larger than 8 bytes, the caller allocates the
result object and passes its address as a **hidden first argument**
in R1; all explicit arguments shift one slot. The callee writes the
result through that pointer. R1–R4 hold no defined values on return
from such a function — in particular, the callee is **not** required
to leave the result pointer in R1.

### Stack Frame

```
        ┌──────────────────────┐  ← caller's SP before call
        │  stack argument N    │     (highest address)
        │  ...                 │
        │  stack argument 5    │  ← SP + 0 at call entry (before prologue)
        ├──────────────────────┤
        │  saved R13 (LR)     │  ← callee saves if non-leaf
        │  saved R10 (FP)     │  ← if frame pointer used
        │  saved R9            │
        │  ...                 │
        │  saved R5            │  ← callee-saved registers
        ├──────────────────────┤
        │  local variables     │
        │  spill slots         │
        │  outgoing stack args │
        ├──────────────────────┤  ← SP during callee body (lowest address)
```

- Stack grows **downward** (toward lower addresses).
- SP (R14) is decremented in the prologue and restored in the epilogue.
- SP must be **4-byte aligned** at all times.
- **No red zone** — the region below SP is not safe from asynchronous clobbering (interrupts use the supervisor stack, but signal handlers on the user stack would clobber it).
- Leaf functions that use no stack space and don't clobber callee-saved registers need no prologue/epilogue.

### Prologue / Epilogue

Typical non-leaf function:

```asm
func:
    ; --- prologue ---
    sub   sp, #FRAME_SIZE       ; allocate frame
    stw   lr, [sp + #LR_OFF]   ; save return address
    stw   s1, [sp + #S1_OFF]   ; save callee-saved regs as needed
    stw   s2, [sp + #S2_OFF]
    ; optional: mov fp, sp      ; set up frame pointer (fp = r10)

    ; --- body ---
    ...
    bl    other_func            ; nested call (clobbers lr)
    ...

    ; --- epilogue ---
    ldw   s2, [sp + #S2_OFF]   ; restore callee-saved
    ldw   s1, [sp + #S1_OFF]
    ldw   lr, [sp + #LR_OFF]   ; restore return address
    add   sp, #FRAME_SIZE       ; deallocate frame
    ret                         ; jmp lr
```

**Large frames (FRAME_SIZE > 65535 bytes).** `SUBi`/`ADDi` take a 16-bit
immediate, so a frame that doesn't fit requires materializing the size
in a scratch register first:

```asm
    lli   t1, #lo16(FRAME_SIZE)
    lui   t1, #hi16(FRAME_SIZE)
    sub   sp, t1                ; prologue
    ...
    lli   t1, #lo16(FRAME_SIZE)
    lui   t1, #hi16(FRAME_SIZE)
    add   sp, t1                ; epilogue
```

Memory accesses to frame slots at offsets larger than ±32767 similarly
need base+offset materialization before the load/store. The compiler
handles this automatically in `PenumbraFrameLowering::adjustSP` and
`PenumbraRegisterInfo::eliminateFrameIndex`.

### Variadic Functions

- Named arguments follow normal register/stack rules.
- Anonymous (variadic) arguments are passed on the **stack** — the compiler spills R1–R4 (the register arguments) to the stack in the callee prologue so that `va_arg` can walk a contiguous memory region.
- `va_list` is a `char *` pointing to the next variadic argument on the stack.

---

## 4. ELF Object Format

### File Header

- `e_machine`: `EM_PENUMBRA` (0xF0DA)
- `e_flags`: 0 (no flags defined yet)
- Class: ELFCLASS32
- Data: ELFDATA2LSB (little-endian)
- OS/ABI: `ELFOSABI_NONE` (bare-metal) or `ELFOSABI_NETBSD`

### Relocation Types

Penumbra uses **RELA** relocations (explicit addend).

| Value | Name | Description | Field |
|-------|------|-------------|-------|
| 0 | `R_PENUMBRA_NONE` | No relocation | — |
| 1 | `R_PENUMBRA_32` | Absolute 32-bit (.word symbol) | Full word |
| 2 | `R_PENUMBRA_BRANCH22` | PC-relative 22-bit word offset | bits [25:4] |
| 3 | `R_PENUMBRA_IMM16` | 16-bit immediate | bits [15:0] |
| 4 | `R_PENUMBRA_LO16` | Low 16 bits of absolute address | bits [15:0] |
| 5 | `R_PENUMBRA_HI16` | High 16 bits of absolute address | bits [15:0] |
| 6 | `R_PENUMBRA_MEMOFFSET16_PCREL` | PC-relative 16-bit memory offset | bits [17:2] |
| 7 | `R_PENUMBRA_IMM16_PCREL` | PC-relative 16-bit immediate | bits [15:0] |
| 8 | `R_PENUMBRA_RELATIVE` | PIE dynamic relocation (bias adjust) | Full word |
| 9 | `R_PENUMBRA_TLS_GD_LO16` | TLS GD: low 16 bits (static: TP offset) | bits [15:0] |
| 10 | `R_PENUMBRA_TLS_GD_HI16` | TLS GD: high 16 bits (static: TP offset) | bits [15:0] |
| 11 | `R_PENUMBRA_GLOB_DAT` | GOT entry (absolute address) | Full word |
| 12 | `R_PENUMBRA_JUMP_SLOT` | PLT GOT entry | Full word |
| 13 | `R_PENUMBRA_TLS_TPOFF32` | TLS IE: TP-relative offset in GOT | Full word |
| 14 | `R_PENUMBRA_TLS_DTPMOD32` | TLS GD: module index in GOT | Full word |
| 15 | `R_PENUMBRA_TLS_DTPOFF32` | TLS GD: module offset in GOT | Full word |
| 16 | `R_PENUMBRA_TLS_GD_PCREL` | TLS GD: PC-relative to GOT entry (PIC) | bits [15:0] |
| 17 | `R_PENUMBRA_PC32` | PC-relative 32-bit (.eh_frame FDE pointers) | Full word |
| 18 | `R_PENUMBRA_GOT_PCREL_LO16` | GOT PC-relative: low 16 bits | bits [15:0] |
| 19 | `R_PENUMBRA_GOT_PCREL_HI16` | GOT PC-relative: high 16 bits | bits [15:0] |
| 20 | `R_PENUMBRA_TLS_GD_GOT_PCREL_LO16` | TLS GD GOT PC-relative: low 16 | bits [15:0] |
| 21 | `R_PENUMBRA_TLS_GD_GOT_PCREL_HI16` | TLS GD GOT PC-relative: high 16 | bits [15:0] |

Where: S = symbol value, A = addend, P = relocation position.

**LLI+LUI pair (32-bit address materialization):**
The linker resolves `R_PENUMBRA_LO16` on LLI and `R_PENUMBRA_HI16` on LUI to produce the full 32-bit address.

### Sections

Standard ELF sections. The linker script defines the memory map:

```
ROM:  0xFFFF_0000 – 0xFFFF_FFFF  (64 KB, boot ROM)
RAM:  0x0000_0000 – 0x00FF_FFFF  (16 MB, main memory)
MMIO: 0xFF00_0000 – 0xFF00_001F  (UART)
```

---

## 5. Struct and Union Layout

Structs are laid out in declaration order with padding inserted to satisfy each member's natural alignment. The struct's overall alignment is the maximum alignment of its members. The struct's size is padded to a multiple of its alignment.

**Example:**
```c
struct example {
    char  a;    // offset 0, size 1
                // 1 byte padding
    short b;    // offset 2, size 2
    int   c;    // offset 4, size 4
    char  d;    // offset 8, size 1
                // 3 bytes padding
};              // total size: 12, alignment: 4
```

Unions are sized to hold the largest member, aligned to the most strictly aligned member.

---

## 6. Stack Argument Passing Order

**No home space.** There is no caller-allocated shadow area for register arguments (unlike MIPS o32). Only arguments that do not fit in R1–R4 are placed on the stack.

Stack arguments are laid out at ascending addresses from SP: argument 5 at `[SP + 0]`, argument 6 at `[SP + 4]`, and so on. The caller allocates this space before the call and deallocates it afterward.

**Variadic functions** must spill R1–R4 to the stack in their prologue to create a contiguous argument area for `va_arg` to walk. This cost is paid only by the small number of truly variadic functions (printf family, exec family, etc.) — not by every call site in the system. This matches ARM32 AAPCS behavior.

```asm
; Variadic prologue example (printf-like function):
my_variadic:
    sub  sp, #16 + FRAME        ; 16 bytes for R1-R4 spill + local frame
    stw  r1, [sp + #FRAME + 0]  ; spill register args to top of frame
    stw  r2, [sp + #FRAME + 4]  ; so they're contiguous with stack args
    stw  r3, [sp + #FRAME + 8]  ; from caller
    stw  r4, [sp + #FRAME + 12]
    stw  lr, [sp + #0]          ; save LR in local frame as usual
    ; va_list starts at sp + FRAME + (named_args * 4)
```

---

## 7. Thread-Local Storage

**R12 (`tp`)** is reserved as the thread pointer. It points to the current thread's Thread Control Block (TCB), from which TLS variables are accessed at fixed offsets.

- The kernel sets R12 on thread creation and context switch.
- Compiled code must never modify R12.
- The register allocator must not use R12 for any purpose.
- TLS access model: Uses Variant I (no TCB gap). **Local Exec / Initial Exec** used for static linking (resolved directly to `R_TPREL`). **General Dynamic** is used for PIC/shared code, which replaces accesses with calls to `__tls_get_addr`.

On bare-metal targets (`penumbra-unknown-none`) without an OS thread scheduler, R12 is unused but still reserved for forward compatibility with the NetBSD ABI.

---

## 8. Position-Independent Code

PIC and PIE are supported.

- **Global Address Materialization (PIC/PIE):** Achieved using GOT-indirect addressing with full 32-bit reach.
- **Relocations:** Uses `R_PENUMBRA_GOT_PCREL_LO16` and `R_PENUMBRA_GOT_PCREL_HI16` to form a GOT-indirect PC-relative offset to the GOT entry.
- **Dynamic Linking:** PLT entries are 16 bytes. Shared libraries use `R_PENUMBRA_GLOB_DAT` for GOT and `R_PENUMBRA_JUMP_SLOT` for PLT. PIE relies on `R_PENUMBRA_RELATIVE` for bias adjustment.

---

## 9. DWARF Register Mapping

For debug info (DWARF `.debug_frame` / `.eh_frame`):

| DWARF Number | Register | Notes |
|-------------:|----------|-------|
| 0 | R0 | zero |
| 1–15 | R1–R15 | GPRs |
| 16 | SR | Status register |
| 17 | EPC | Exception PC |
| 18 | ESR | Exception SR |

The **return address** register for DWARF CFA is **R13** (DWARF number 13).

---

## Revision History

| Version | Date | Changes |
|---------|------|---------|
| 0.1 | 2026-03-31 | Initial draft |
