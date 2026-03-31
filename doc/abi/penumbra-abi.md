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
| `long long` | 8 | 4 |
| `float` | 4 | 4 |
| `double` | 8 | 4 |
| `long double` | 8 | 4 |
| pointer | 4 | 4 |
| `size_t` | 4 | 4 |
| `ptrdiff_t` | 4 | 4 |
| `wchar_t` | 4 | 4 |

- `char` is **unsigned** by default (matches ARM convention; avoids sign-extension on byte loads with LDB).
- Bit-fields are packed LSB-first within their storage unit.
- `long double` is the same as `double` (no extended precision — no FPU).

### Byte Order

**Little-endian.** Address A+0 holds bits [7:0] of a word at address A.

### Alignment

All data types are naturally aligned. The hardware traps on misaligned word and halfword accesses (alignment fault, vector 8). The compiler must ensure correct alignment; there is no software misalignment handler in the default runtime.

Stack pointer must be 4-byte aligned at all times.

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

1. The first four scalar arguments are passed in **R1, R2, R3, R4** (in order).
2. Arguments beyond four are passed on the **stack**, pushed right-to-left (C convention), so that argument 5 is at the lowest stack address.
3. Each stack argument occupies a 4-byte slot (smaller types are widened to 32 bits).
4. 64-bit arguments (`long long`, `double`): passed in an **aligned register pair** (R1:R2 or R3:R4). If the next available register is odd-numbered (R3 when a 64-bit arg needs passing), the odd register is skipped and the pair starts at the next even register. If no register pair is available, the argument goes on the stack, 4-byte aligned.
5. Structs and unions ≤ 4 bytes are passed by value in a single register. Structs 5–8 bytes are passed in a register pair (same alignment rules as 64-bit scalars). Structs > 8 bytes are passed by **reference** — the caller allocates a copy on its stack and passes a pointer in the next available register.

### Return Values

| Size | Location |
|------|----------|
| ≤ 4 bytes | R1 |
| 5–8 bytes | R1 (low), R2 (high) |
| > 8 bytes | Caller passes hidden pointer in R1; callee writes to it and returns the pointer in R1 |

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

| Number | Name | Field | Calculation | Description |
|--------|------|-------|-------------|-------------|
| 0 | `R_PENUMBRA_NONE` | — | — | No relocation |
| 1 | `R_PENUMBRA_32` | word32 | S + A | Absolute 32-bit |
| 2 | `R_PENUMBRA_IMM16` | Format L imm16 [15:0] | (S + A) & 0xFFFF | 16-bit immediate (lower half) |
| 3 | `R_PENUMBRA_IMM16_HI` | Format L imm16 [15:0] | (S + A) >> 16 | 16-bit immediate (upper half, for LUI) |
| 4 | `R_PENUMBRA_BRANCH22` | Format B offset22 [25:4] | (S + A - P) >> 2 | PC-relative branch, 22-bit signed word offset |

Where: S = symbol value, A = addend, P = relocation position.

**LLI+LUI pair (32-bit address materialization):**
The linker resolves `R_PENUMBRA_IMM16` on LLI and `R_PENUMBRA_IMM16_HI` on LUI to produce the full 32-bit address.

### Sections

Standard ELF sections. The linker script defines the memory map:

```
ROM:  0xFFFF_E000 – 0xFFFF_FFFF  (8 KB, boot ROM)
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
- TLS access model: **Local Exec** (static linking, single executable). Other models (Initial Exec, General Dynamic) will be added when shared libraries are supported.

On bare-metal targets (`penumbra-unknown-none`) without an OS thread scheduler, R12 is unused but still reserved for forward compatibility with the NetBSD ABI.

---

## 8. Position-Independent Code

Not defined in this version. Initial code will be statically linked. PIC/GOT/PLT support will be added when shared libraries are needed for the NetBSD port.

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
