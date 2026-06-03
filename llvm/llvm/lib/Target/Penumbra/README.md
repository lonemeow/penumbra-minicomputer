# Penumbra LLVM Backend

LLVM backend for the Penumbra 32-bit RISC minicomputer. It compiles C
(and any LLVM-IR-producing frontend) all the way to linked ELF binaries
for two environments:

- `penumbra-unknown-none` — bare-metal: the boot ROM and hardware test
  programs.
- `penumbra-unknown-netbsd` — the NetBSD kernel and userland.

This file is a map for someone opening the backend for the first time:
what the target looks like, how a program flows through it, and where
each piece of that flow lives. The neighbouring `CLAUDE.md` has the
exhaustive per-file detail and the build/test commands; `doc/system/`
holds the authoritative ISA and ABI specs.

## Architecture at a Glance

- 32-bit little-endian, ILP32 data model
  (`e-m:e-p:32:32-i32:32-i64:64-n32-S32`).
- 16 registers. Special roles: `R0` = zero, `R12` = thread pointer
  (TP), `R13` = link register (LR), `R14` = stack pointer (SP),
  `R15` = program counter (PC). The allocator reserves R0/R12/R14/R15.
- Calling convention: arguments in `R1`–`R4` (overflow on the stack),
  return value in `R1` (with `R2` holding the high half of an i64),
  callee-saved `R5`–`R10` and `R13`.
- 4 instruction formats: R (register), L (immediate), M (memory),
  B (branch).
- 2-operand *destructive* ALU (`dst op= src`, like x86/68k — not the
  3-operand form of RISC-V). The selector models this with tied
  operands.
- ARM-style NZCV condition flags, 16 condition codes.
- Integer multiply and divide run on a hardware `divmul` peer unit;
  divide-by-zero traps in hardware. Floating point is software-only
  (compiler-rt libcalls).
- Software-managed TLB, split I/D cache.

## How a Program Gets Compiled

There are two pipelines to keep straight. The outer one is the
*toolchain* — the chain of executables that turns source into a memory
image. The inner one is the *compiler's GlobalISel pass pipeline* — what
happens inside `clang`/`llc` to lower IR onto the ISA.

### The toolchain pipeline (source → memory image)

```
C source                        Assembly (handwritten)
    |                                    |
    v                                    v
  clang -c                          llvm-mc -filetype=obj
    |                                    |
    v                                    v
  .o (ELF)  ─────────────────────>  .o (ELF)
                    |
                    v
               ld.lld -T <script>
                    |
                    v
               .elf (linked)
                    |
                    v
             llvm-objcopy -O binary
                    |
                    v
               bin2hex.py  -->  program.hex  -->  simulator / FPGA
```

`clang -c` goes straight to an object file — that is the normal path,
not a slower fallback. Handwritten assembly (crt0, ROM, hardware tests)
takes the `llvm-mc` branch but lands in the same object format. The
final `objcopy`/`bin2hex` steps exist because the simulator and FPGA
boot ROM load `$readmemh`-style hex, not ELF.

### Inside the compiler (LLVM IR → machine code)

Penumbra is a **GlobalISel** target — there is no SelectionDAG. A
function moves through these passes (each Penumbra source file below
plugs into one of them):

1. **IR passes** — `AtomicExpand`, then `PenumbraLowerTLS` rewrites
   `@llvm.threadlocal.address` (general-dynamic → a `__tls_get_addr`
   call; local/initial-exec → an inline TP-relative address).
2. **IRTranslator** — LLVM IR → generic MIR (gMIR): virtual registers
   and target-independent `G_*` opcodes. `PenumbraCallLowering` plugs
   in here to implement the ABI for formal arguments, returns, and
   calls.
3. **Pre-legalizer combiner** — IR-level cleanup. A small set at `-O0`
   (`PenumbraO0PreLegalizerCombiner`); the full upstream canonicalization
   set at `-O1+` (`PenumbraPreLegalizerCombiner`). Rule lists live in
   `PenumbraCombine.td`.
4. **Legalizer** (`PenumbraLegalizerInfo`) — make every generic op legal
   for the ISA. The governing shape is *legal at the native integer
   width, narrow/widen/libcall everything else*: `s32` is the legal
   integer; sub-word types widen to `s32`; `s64` narrows to `s32` pairs
   or falls back to a compiler-rt libcall. Multiply/divide are the
   clearest example — see "Multiply and divide" below.
5. **Post-legalizer combiner** (`-O1+` only) —
   `PenumbraPostLegalizerCombiner` runs a few target-specific cleanups
   in C++ (e.g. re-canonicalizing `G_SUB x, c` back into `G_ADD x, -c`
   when the negated immediate fits, promoting narrow zext-loads).
6. **RegBankSelect** — trivial here: one register bank (`GPRRegBank`)
   covers all 16 registers.
7. **InstructionSelect** (`PenumbraInstructionSelector`) — turns gMIR
   into real `MachineInstr`s. This is a **hybrid** selector:
   `selectImpl()` dispatches the simple 1:1 cases from TableGen `Pat<>`
   rules (`PenumbraGISel.td`), and hand-written C++ handles the awkward
   ones — LLI+LUI address materialization, GOT-relative PIC, jump
   tables, fused ICMP+BRCOND, varargs, return-address capture.

After selection, the MC layer emits the bytes:
`PenumbraAsmPrinter` (MachineInstr → MCInst, plus pseudo expansions like
`RET` → `JMP R13`) → `PenumbraMCCodeEmitter` (MCInst → 32-bit words and
fixups) → `PenumbraAsmBackend` (resolve fixups, range-check immediates,
request relocations) → `PenumbraELFObjectWriter` (fixups → ELF reloc
types).

### Multiply and divide

A good worked example of the legalizer philosophy. At `s32`:

- The variable-operand cases (`G_MUL`, `G_SDIV`/`G_SREM`,
  `G_UDIV`/`G_UREM`, and the fused `G_SDIVREM`/`G_UDIVREM`) stay legal
  and select directly to the hardware divmul unit
  (`MUL`/`DIV`/`DIVU`/`DIV_P`/`DIVU_P`). A `divmul` operation produces
  quotient and remainder together, so `a/b; a%b` on the same operands
  costs one divide.
- Constant power-of-2 (and 2ⁿ±1) operands are strength-reduced to shifts
  and adds during custom lowering, because that beats a ~34-cycle
  hardware divide.

At `s64` everything libcalls (`__muldi3`, `__udivdi3`, …) — except a
genuine 32×32→64 widening multiply, which is a single hardware multiply
emitted as a low/high pair (`G_MUL` + `G_S/UMULH`).

## Design Decisions

**GlobalISel, hybrid selection.** No SelectionDAG. Simple selections are
TableGen `Pat<>` rules driven through `selectImpl()`; the genuinely
target-specific work (tied-operand destructive ALU, address
materialization, PIC, jump tables) is hand-written C++ in
`PenumbraInstructionSelector`. This keeps the boilerplate in TableGen
while retaining full control over the unusual cases.

**Address materialization with lo16/hi16.** A 32-bit address is built
from an `LLI` (load low 16) + `LUI` (load upper 16) pair, each carrying
its own relocation so the linker can patch the halves independently. The
codegen path attaches `MachineOperand` target flags; the assembler path
uses `%lo16()`/`%hi16()` specifier expressions. PIC extends this:
GOT-relative globals and TLS general-dynamic accesses anchor the
LLI/LUI pair on the `ADD`'s own PC, so the addends are fixed `-8`/`-4`
offsets.

**Pseudo-instruction expansion happens in two places, by audience.**
The codegen `RET` pseudo is expanded in `PenumbraAsmPrinter`
(→ `JMP R13`). The handwritten-assembly conveniences `LI`, `LA`, `NOP`,
and `RET` are expanded in `PenumbraAsmParser` — they exist for crt0 and
test programs; compiler-generated code emits the underlying
instructions directly.

**Range-checked encoding.** The code emitter and asm backend hard-error
on out-of-range immediates rather than silently truncating, so a
miscompile surfaces as a build failure instead of wrong bits.

**`EM_PENUMBRA = 0xF0DA`.** Private ELF machine number, defined centrally
in `llvm/BinaryFormat/ELF.h` and shared by the MC layer, the binary
utilities, and lld.

## Source Layout

Grouped by where each file sits in the pipeline above. (`CLAUDE.md` has
a finer-grained table with the non-obvious gotchas.)

**TableGen — the declarative description**

```
Penumbra.td                  Top-level: target, ProcessorModel, includes
PenumbraRegisterInfo.td      16 GPRs, register classes, HW encoding
PenumbraInstrInfo.td         All 4 formats, bit-accurate; tied-operand
                             constraints; codegen pseudos (RET, SELECT_*)
PenumbraCallingConv.td       CC/RetCC/CSR (R1-R4 args, R1/R2 ret, R5-R10+R13 saved)
PenumbraGISel.td             Simple 1:1 selection Pat<> rules
PenumbraCombine.td           Combiner rule-group lists (pre/post/-O0)
GISel/PenumbraRegisterBanks.td   Single GPRRegBank
```

**Codegen — lowering and selection**

```
PenumbraTargetMachine.cpp    Data layout, GlobalISel pass pipeline, TLS-lowering IR pass
PenumbraSubtarget.cpp        Central hub: owns InstrInfo, TLInfo, all GISel objects
PenumbraISelLowering.cpp     SELECT diamond expansion, jump-table encoding,
                             inline-asm constraints, addressing-mode hooks
PenumbraTargetTransformInfo.h  Cost-model/LSR hooks (header-only); disables vectorizers
PenumbraFrameLowering.cpp    Prologue/epilogue, SP adjust, emergency spill slot
PenumbraRegisterInfo.cpp     Reserved regs, callee-saved list, eliminateFrameIndex
PenumbraInstrInfo.cpp        Instruction-info queries (copy, branch analysis, …)
PenumbraMachineFunctionInfo.h  Per-function state (e.g. varargs save area)
PenumbraAsmPrinter.cpp       MachineInstr → MCInst; RET expansion; reloc-specifier wrapping
GISel/
  PenumbraCallLowering.cpp        Formal args, returns, calls (incl. PIC/TLS)
  PenumbraLegalizerInfo.cpp       Legal type/op combinations
  PenumbraRegisterBankInfo.cpp    Single-bank reg-bank mapping
  PenumbraInstructionSelector.cpp Hybrid select() (selectImpl + manual C++)
  PenumbraPreLegalizerCombiner.cpp    Pre-legalizer combiner (-O1+)
  PenumbraO0PreLegalizerCombiner.cpp  Pre-legalizer combiner (-O0)
  PenumbraPostLegalizerCombiner.cpp   Post-legalizer combiner + custom rules
```

**MC layer — text/binary encoding and ELF**

```
MCTargetDesc/
  PenumbraMCTargetDesc.cpp     Registers all MC components; branch-target analysis
  PenumbraInstPrinter.cpp      MCInst → assembly text
  PenumbraMCCodeEmitter.cpp    MCInst → binary; fixup creation
  PenumbraAsmBackend.cpp       Fixup resolution; relocation requests; NOP = ADD R0,R0
  PenumbraELFObjectWriter.cpp  Fixup → ELF reloc type mapping
  PenumbraFixupKinds.h         Fixup kinds + reloc-specifier values
  PenumbraMCAsmInfo.cpp        Syntax config; %lo16/%hi16/%pcrel/%tlsgd printing
AsmParser/
  PenumbraAsmParser.cpp        Assembly text → MCInst; LI/LA/NOP/RET expansion
Disassembler/
  PenumbraDisassembler.cpp     Binary → MCInst (custom decoders for branches,
                               signed immediates, signed mem offsets)
TargetInfo/
  PenumbraTargetInfo.cpp       Target registration (Triple::penumbra)
```

## lld Integration

`llvm/lld/ELF/Arch/Penumbra.cpp` is the linker target. It handles the
full `R_PENUMBRA_*` relocation set — the authoritative table is the
[Relocation Types](../../../../../doc/system/abi.md#relocation-types)
chapter of the ABI spec — well beyond simple absolute/branch fixups:

- **Static + PIE.** Absolute (`R_PENUMBRA_32`, lo16/hi16), branch
  (`BRANCH22`), and PC-relative immediates, plus `R_PENUMBRA_RELATIVE`
  for self-relocating PIE images (the bootloader and `ld.elf_so`).
- **GOT / PLT.** GOT-relative PC-relative pairs for PIC globals;
  16-byte PLT stubs; `R_PENUMBRA_GLOB_DAT` / `R_PENUMBRA_JUMP_SLOT` /
  `R_PENUMBRA_IRELATIVE` / `R_PENUMBRA_COPY`. Shared libraries
  (`-shared`) link and run.
- **TLS.** General-dynamic via GOT-indirect PC-relative access to the
  `tls_index` pair (`__tls_get_addr`); local/initial-exec via TP-relative
  offsets (Variant I, no TCB gap).

Relocations use the RELA form (explicit addends). The bare-metal link
script `hw/rom/rom.ld` places code at `0xFFFF_0000` (the boot ROM
address); NetBSD userland links as PIE / dynamic instead.

## Building

One-time configure (from the repo root):

```sh
cmake -G Ninja -S llvm/llvm -B build/llvm \
  -DLLVM_TARGETS_TO_BUILD=Penumbra \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  -DLLVM_INCLUDE_TESTS=ON -DLLVM_BUILD_TESTS=ON \
  -DLLVM_PARALLEL_LINK_JOBS=2
```

Incremental rebuild — name only the tools you need (a bare
`ninja -C build/llvm` also builds upstream unit tests):

```sh
ninja -C build/llvm -j10 llc clang lld \
  llvm-mc llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-readobj
```

`-j10` for compilation; the `-DLLVM_PARALLEL_LINK_JOBS=2` cap keeps the
linker steps from OOM-ing a 15 GB WSL2 box. Backend regression tests
(Lit + FileCheck) live in `test/CodeGen/Penumbra/`. See `CLAUDE.md` for
the test commands and the compiler-correctness suite.

## What's Not Here

- **Hardware floating point.** FP is software-only via compiler-rt
  libcalls; there is no FPU and no float register bank.
- **Hardware ADC/SBC for GlobalISel.** Add/subtract-with-carry chains
  (e.g. i64 arithmetic) lower to explicit ADD + carry-compare sequences;
  using the hardware ADC/SBC instructions would need a condition-code
  register bank that GlobalISel does not yet model.
