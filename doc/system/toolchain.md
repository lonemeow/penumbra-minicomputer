# Penumbra Toolchain & OS Strategy

## Target OS: NetBSD

### Decision (2026-03-28)

The OS target was changed from Minix 2 to NetBSD.

**Why not Minix 2:** Minix 2 does not support demand paging or copy-on-write fork(). Every fork() copies the entire address space, which is unacceptable for real workloads on a 32-bit platform.

**Why not Minix 3:** Minix 3 has demand paging, but its microkernel architecture imposes heavy IPC overhead. Every system call, filesystem operation, and driver interaction crosses process boundaries via message passing. On a single-issue microcoded CPU with no caches, this overhead dominates.

**Why NetBSD:**
- **Demand paging and COW fork** via UVM, the machine-independent virtual memory subsystem
- **Clean MD/MI separation.** NetBSD's `sys/arch/<port>/` structure isolates machine-dependent code. A new port requires: `locore.S`, `trap.c`, `pmap.c`, `machdep.c`, `autoconf.c`, clock/interrupt glue, console driver
- **Software-managed TLB is first-class.** The MIPS port (`sys/arch/mips/`) has used software TLB refill for 30+ years. Penumbra's TLB design (64-entry 2-way SA + 8-entry FA pinned, ASID, per-page RWX+U+G, software refill via exception) maps directly to the MIPS pmap model
- **Monolithic kernel.** System calls are function calls into the kernel, not cross-process messages. One trap, one context switch — not three
- **Prior experience.** We have direct NetBSD kernel experience: bug fixes in amiga and sgimips early boot code, device driver work. Familiar with the port structure, build system, and kernel internals

### Hardware Requirements for NetBSD

| Feature | Status | Notes |
|---------|--------|-------|
| Supervisor/user mode | Done | SR.S bit, dispatch-time privilege checks |
| Software-managed TLB | Done | 64-entry 2-way SA + 8-entry FA pinned, ASID, RWX+U+G permissions |
| TLB miss exception | Done | Vector 2, handler refills TLB, ERET retries |
| TLB protection fault | Done | Vector 3, triggers COW copy in UVM |
| Exception save/restore | Done | EPC/ESR, ERET, RDSPR, WRSPR |
| SYSCALL trap | Done | Vector 5, intercepted at dispatch |
| Timer interrupt | Done | `SYSDEV_TIMER` drives NetBSD hardclock |
| UART | Done | NS16550A-compatible real UART + 16450 sim UART; `com(4)` driver attached |
| Device IRQ dispatch | Done | Shared wire-OR `/IRQ` line, kernel `intr_dispatch` walks registered handlers (`VEC_EXT_IRQ`) |
| Sub-word loads/stores | Done | LDH/LDHS/LDB/LDBS/STH/STB with byte_ext/byte_rep |
| SDRAM controller | Done | SDRAM v2 controller (`hw/rtl/io/sdram/`) running 32 MB W9825 @ 100 MHz CL2 on ULX3S |
| Real cache | Done | L1: split I/D VIPT (1 KiB each, direct-mapped, write-through D). L2: 64 KiB 4-way unified, write-invalidate-on-hit (`doc/internals/l2-cache.md`) |

### Porting Reference

The primary reference is `sys/arch/mips/` — specifically:
- `mips/pmap.c` — software TLB management, maps to Penumbra's WRSYS/RDSYS TLB ops
- `mips/trap.c` — exception dispatch, translates to Penumbra vector numbers
- `mips/locore.S` — trap entry/exit, context switch (MIPS TLBWR/TLBWI → Penumbra WRSYS)
- `mips/mips_machdep.c` — machine setup, memory probe

Translation from MIPS TLB operations to Penumbra:
| MIPS | Penumbra | Notes |
|------|----------|-------|
| `TLBWR` (write random) | `WRSYS` to TLB_VPN, TLB_PTE, TLB_INDEX | Software picks the slot |
| `TLBWI` (write indexed) | Same | Same mechanism |
| `TLBR` (read indexed) | `RDSYS` from TLB_VPN, TLB_PTE | |
| `TLBP` (probe) | Software walk | No hardware probe instruction |
| `MFC0 EntryHi` etc. | `RDSYS` from MMU registers | |

### Atomics

NetBSD needs compare-and-swap or load-linked/store-conditional for kernel locking. On uniprocessor, the standard approach is to disable interrupts around critical sections:

```asm
DI                      ; disable interrupts
LDW  R1, [R2 + #0]     ; load current value
CMP  R1, R3            ; compare with expected
BNE  cas_fail
STW  R4, [R2 + #0]     ; store new value
cas_fail:
EI                      ; re-enable
```

This maps to NetBSD's `__cpu_simple_lock` on uniprocessor builds. No hardware atomics needed unless we pursue SMP.

---

## Compiler Toolchain: LLVM

### Decision

LLVM, not GCC or lcc/vbcc, for the Penumbra compiler toolchain.

**Why LLVM:**
- **Better backend infrastructure** for new targets. TableGen instruction descriptions, GlobalISel instruction selection, and the register allocator are modular and well-documented
- **Better optimization** for novel RISC targets, based on research into other homebrew CPU LLVM backends
- **Active development** of the backend framework; GCC's backend interface is more entangled
- **Clang frontend** provides C11/C++17 support out of the box; GCC requires more porting effort in the frontend

### LLVM Backend Components

The Penumbra LLVM backend (`llvm/llvm/lib/Target/Penumbra/`) consists of:

1. **TableGen instruction descriptions** — encoding, operands, patterns for each instruction
2. **Register info** — 16 registers, calling convention, reserved registers (R0, R12, R14, R15)
3. **Instruction selection** — GlobalISel with hybrid TableGen patterns (`PenumbraGISel.td`) + manual C++ for complex cases
4. **Frame lowering** — stack frame layout, prologue/epilogue generation (SUBi/ADDi SP)
5. **ABI/calling convention** — R1-R4 args, R1 return, R5-R10 callee-saved, R13/LR
6. **MC layer** — assembly parser, printer, ELF object emission, fixups and relocations (see `doc/system/abi.md` §4 for the full relocation table)
7. **Immediate materialization** — LLI (uimm16), LLIS (simm16neg) via TableGen; LLI+LUI pair for wide constants in C++
8. **lld linker support** — `elf32penumbra` emulation, all relocation types including PIE and TLS

### ISA Characteristics Affecting Codegen

| Feature | Impact | Mitigation |
|---------|--------|------------|
| 2-operand destructive (`Rd = Rd op Rs`) | Register allocator inserts MOV copies | Normal for x86/Thumb; allocator handles it |
| No conditional execution | SELECT lowers to branches | Standard for most RISC targets |
| 16-bit immediates | Generous (RISC-V has 12-bit) | LLI/LUI pair for 32-bit constants |
| No hardware multiply (initially) | Need libgcc-style emulation | Software `__mulsi3` etc., hardware added later |
| Single link register (R13) | Leaf functions don't need stack frame | Non-leaf must save/restore R13 |
| No barrel shifter in address calc | Array indexing needs explicit shift+add | Compiler can strength-reduce |

### Calling Convention (Implemented)

```
R0        Zero (hardwired)
R1-R4     Arguments / return values (caller-saved)
R5-R10    Callee-saved (6 registers); R10 = frame pointer if needed
R11       Scratch / temporary (caller-saved)
R12       Thread pointer (reserved, not allocatable)
R13       Link register (caller-saved, set by BL)
R14       Stack pointer (hardware-banked USP/SSP)
R15       Program counter (read-only)
```

**Stack frame:**
- Grows downward (DEC SP)
- Callee saves R5-R10 and R13 (if non-leaf) in prologue
- Arguments beyond R1-R4 passed on stack
- Return value in R1 (64-bit in R1:R2)
- Frame pointer: optional, use R10 if needed
- No home space / shadow area for register arguments

See `doc/system/abi.md` for the full ABI specification.

This convention gives 4 argument registers (matches ARM32 and MIPS o32), 6 callee-saved registers, 2 scratch registers (R11, R13/LR), and a reserved thread pointer (R12) for TLS.

### Build System

NetBSD uses its own build framework (`build.sh`) which supports cross-compilation. The flow:

```
1. Build LLVM cross-compiler (host tools)
   - clang targeting penumbra-unknown-netbsd
   - llvm-mc, llvm-objdump, lld

2. Build NetBSD userland + kernel
   - build.sh -m penumbra -T llvm
   - Produces: kernel, root filesystem image

3. Load into simulation or FPGA
   - Kernel image → program.hex (or boot from simulated storage)
```

### Progress

| Item | Status | Notes |
|------|--------|-------|
| ISA assembler (pasm.py) | Done | Two-pass, all formats, labels, .equ constants |
| Microcode assembler (uasm.py) | Done | Symbolic fields, slot validation |
| LLVM MC-layer assembler | Done | All 4 formats, fixups + ELF relocations (see `doc/system/abi.md` §4), pseudo-instructions (LI/LA/NOP/RET) |
| ABI specification | Done | ILP32, register convention, calling convention, stack frame, ELF relocations. See `doc/system/abi.md` |
| bin2hex.py | Done | Flat binary → $readmemh hex (pipeline: llvm-mc → objcopy → bin2hex) |
| Calling convention | Done | R1–R4 args, R5–R10 callee-saved, R11 scratch, R12 TP, R13 LR. Implemented in `PenumbraCallingConv.td` |
| LLVM codegen (GlobalISel) | Done | Hybrid TableGen + C++ instruction selection, frame lowering, register allocation. `-O0` through `-O2` working. Boot ROM, kernel, and NetBSD userland all compile |
| Linker (lld) | Done | `elf32penumbra` emulation, full Penumbra relocation set (`doc/system/abi.md` §4), PIE + shared library support. `EM_PENUMBRA` (0xF0DA) |
| Inline assembly | Done | `r`/`i` constraints, `~{cc}`/`~{memory}` clobbers |
| NetBSD MD layer | Done | Boots to single-user shell with FFS root mounted rw on `ld0f`; full dynamically-linked userland. See `doc/system/netbsd/porting-status.md` |

---

## Open Questions

1. **Debug info.** DWARF support in the LLVM backend for source-level debugging. Lower priority but valuable.

2. **Floating point.** Software-emulated via compiler soft-float today. Hardware FPU is a future project. (The integer multi-cycle ops MUL/MULU/DIV/DIVU, Format R opcodes 16–19, are implemented by the divmul peer unit — see `doc/internals/divmul.md`.) See `doc/TODO.md` § Phase 5 (FPU).

## Resolved Questions

1. **ELF machine number.** `EM_PENUMBRA = 0xF0DA` (private range). Implemented in PenumbraELFObjectWriter.cpp.

2. **Relocation types.** Implemented: full Penumbra relocation set covering static, PIC/PIE, GOT/PLT, TLS GD/IE/LE, COPY, and IRELATIVE. Local fixups resolved by assembler; relocations emitted for external symbols. See `doc/system/abi.md` §4 for the authoritative table — that is the single source of truth for type/value/field mappings.

3. **Alignment.** Implemented: MMU checks alignment for word (addr[1:0]==0), half (addr[0]==0), byte (always OK). Traps to VEC_ALIGN (vector 8). Works in both bypass and MMU-enabled mode.
