# Penumbra Toolchain & OS Strategy

## Target OS: NetBSD

### Decision (2026-03-28)

The OS target was changed from Minix 2 to NetBSD.

**Why not Minix 2:** Minix 2 does not support demand paging or copy-on-write fork(). Every fork() copies the entire address space, which is unacceptable for real workloads on a 32-bit platform.

**Why not Minix 3:** Minix 3 has demand paging, but its microkernel architecture imposes heavy IPC overhead. Every system call, filesystem operation, and driver interaction crosses process boundaries via message passing. On a single-issue microcoded CPU with no caches, this overhead dominates.

**Why NetBSD:**
- **Demand paging and COW fork** via UVM, the machine-independent virtual memory subsystem
- **Clean MD/MI separation.** NetBSD's `sys/arch/<port>/` structure isolates machine-dependent code. A new port requires: `locore.S`, `trap.c`, `pmap.c`, `machdep.c`, `autoconf.c`, clock/interrupt glue, console driver
- **Software-managed TLB is first-class.** The MIPS port (`sys/arch/mips/`) has used software TLB refill for 30+ years. Penumbra's TLB design (64-entry 2-way SA, ASID, per-page RWX+U+G, software refill via exception) maps directly to the MIPS pmap model
- **Monolithic kernel.** System calls are function calls into the kernel, not cross-process messages. One trap, one context switch — not three
- **Prior experience.** We have direct NetBSD kernel experience: bug fixes in amiga and sgimips early boot code, device driver work. Familiar with the port structure, build system, and kernel internals

### Hardware Requirements for NetBSD

| Feature | Status | Notes |
|---------|--------|-------|
| Supervisor/user mode | Done | SR.S bit, dispatch-time privilege checks |
| Software-managed TLB | Done | 64-entry 2-way SA, ASID, RWX+U+G permissions |
| TLB miss exception | Done | Vector 2, handler refills TLB, ERET retries |
| TLB protection fault | Done | Vector 3, triggers COW copy in UVM |
| Exception save/restore | Done | EPC/ESR, ERET, RDSPR, WRSPR |
| SYSCALL trap | Not yet | Microcode needed (vector 5, like BREAK) |
| Timer interrupt | Not yet | Drives hardclock() / scheduler |
| UART | Not yet | Console I/O (com driver) |
| Interrupt controller | Not yet | Multiple devices, priority |
| Sub-word loads/stores | Not yet | Byte/halfword for strings, structs |
| SDRAM controller | Not yet | NetBSD kernel needs 2-4 MB minimum |
| Real cache | Not yet | cache_stub is pass-through; kernel working set needs caching |

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
- **Better backend infrastructure** for new targets. TableGen instruction descriptions, SelectionDAG pattern matching, and the register allocator are modular and well-documented
- **Better optimization** for novel RISC targets, based on research into other homebrew CPU LLVM backends
- **Active development** of the backend framework; GCC's backend interface is more entangled
- **Clang frontend** provides C11/C++17 support out of the box; GCC requires more porting effort in the frontend

### LLVM Backend Work Items

A Penumbra LLVM backend (`lib/Target/Penumbra/`) requires:

1. **TableGen instruction descriptions** — encoding, operands, patterns for each instruction
2. **Register info** — 16 registers, calling convention, reserved registers (R0, R14, R15)
3. **Instruction selection** — lowering IR to Penumbra instructions via SelectionDAG patterns
4. **Frame lowering** — stack frame layout, prologue/epilogue generation
5. **ABI/calling convention** — argument passing, return values, callee-saved registers
6. **MC layer** — assembly printer, object file emission (ELF)
7. **Immediate materialization** — LLI/LUI patterns for 32-bit constants

### ISA Characteristics Affecting Codegen

| Feature | Impact | Mitigation |
|---------|--------|------------|
| 2-operand destructive (`Rd = Rd op Rs`) | Register allocator inserts MOV copies | Normal for x86/Thumb; allocator handles it |
| No conditional execution | SELECT lowers to branches | Standard for most RISC targets |
| 16-bit immediates | Generous (RISC-V has 12-bit) | LLI/LUI pair for 32-bit constants |
| No hardware multiply (initially) | Need libgcc-style emulation | Software `__mulsi3` etc., hardware added later |
| Single link register (R13) | Leaf functions don't need stack frame | Non-leaf must save/restore R13 |
| No barrel shifter in address calc | Array indexing needs explicit shift+add | Compiler can strength-reduce |

### Calling Convention (Proposed)

```
R0        Zero (hardwired)
R1-R4     Arguments / return values (caller-saved)
R5-R11    Callee-saved (7 registers)
R12       Scratch / temporary (caller-saved)
R13       Link register (caller-saved, set by BL)
R14       Stack pointer (hardware-banked USP/SSP)
R15       Program counter (read-only)
```

**Stack frame:**
- Grows downward (DEC SP)
- Callee saves R5-R11 and R13 (if non-leaf) in prologue
- Arguments beyond R1-R4 passed on stack
- Return value in R1 (64-bit in R1:R2)
- Frame pointer: optional, use R11 if needed

This convention gives 4 argument registers (matches ARM32 and MIPS o32), 7 callee-saved registers (good register pressure for loops), and 2 scratch registers (R1 doubles as return, R12 for temporaries).

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
| LLVM backend skeleton | Done | Triple, registers, instruction encodings, target machine — compiles and links |
| Calling convention | Proposed | Needs validation via hand-written assembly |
| NetBSD MD layer | Not started | |
| ELF object format | Not started | Need linker script for Penumbra memory map |

---

## Open Questions

1. **ELF machine number.** Need to pick an unofficial `EM_PENUMBRA` value for ELF headers. Use a number in the private range (0xF000–0xFFFF).

2. **Relocation types.** The assembler currently emits flat hex. For LLVM, need ELF relocations for: 22-bit branch offset (Format B), 16-bit immediate (Format L), LLI+LUI pairs (32-bit address materialization).

3. **Debug info.** DWARF support in the LLVM backend for source-level debugging. Lower priority but valuable.

4. **Floating point.** Initially software-emulated via compiler soft-float. Hardware FPU is a future project (reserved ALU opcodes 0x0B–0x10 for MUL/DIV, further slots for FP).

5. **Alignment.** The ISA doc mentions alignment faults but they're not implemented. NetBSD expects either hardware alignment checking or the ability to handle misaligned accesses in software. Decision: trap on misaligned access (simplest hardware, matches MIPS behavior).
