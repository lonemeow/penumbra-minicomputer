# Penumbra LLVM Backend

LLVM backend for the Penumbra 32-bit RISC minicomputer.  Supports the full
compilation pipeline from C source to linked ELF binaries.

## Architecture at a Glance

- 32-bit little-endian, ILP32 data model
- 16 registers (R0=zero, R14=SP, R13=LR, R15=PC)
- 4 instruction formats: R (register), L (immediate), M (memory), B (branch)
- 2-operand destructive ALU (like x86/68k, not 3-operand like RISC-V)
- ARM-style NZCV condition flags, 16 condition codes
- Software-managed TLB, split I/D cache

## Pipeline

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
               ld.lld -T rom.ld
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

## Design Decisions

**GlobalISel only, no SelectionDAG.**  The instruction selector is a manual
C++ `select()` function (not TableGen patterns).  This gives full control over
instruction selection for a non-standard 2-operand ISA and avoids the
complexity of teaching SelectionDAG about tied operands.

**MC-layer pseudo-instructions.**  LI, LA, NOP, and RET are expanded in the
AsmParser (not the AsmPrinter or codegen).  They exist for handwritten
assembly (crt0, test programs) — the compiler emits LLI/LUI directly.

**lo16/hi16 relocations.**  32-bit addresses are materialized as LLI+LUI
pairs.  Each half gets its own relocation type so the linker can patch them
independently.  The codegen path uses MachineOperand target flags; the
assembler path uses MCSpecifierExpr.

**EM_PENUMBRA = 0xF0DA.**  Private ELF machine number defined in
`llvm/BinaryFormat/ELF.h`.  Used by both the MC layer and lld.

## Source Layout

```
Penumbra.td                  Top-level TableGen (target, processor, includes)
PenumbraRegisterInfo.td      16 GPRs, register classes, HW encoding
PenumbraInstrInfo.td         All 4 formats with bit-accurate encoding
PenumbraCallingConv.td       CC_Penumbra: R1-R4 args, R1 return

PenumbraTargetMachine.cpp    Data layout, GlobalISel pipeline setup
PenumbraSubtarget.cpp        Central hub: owns all target objects
PenumbraFrameLowering.cpp    Prologue/epilogue (SUBi/ADDi SP)
PenumbraISelLowering.cpp     SELECT_GPR/SELECT_CC_GPR diamond expansion
PenumbraRegisterInfo.cpp     Reserved regs, eliminateFrameIndex
PenumbraAsmPrinter.cpp       MachineInstr -> MCInst, pseudo expansion

GISel/
  PenumbraCallLowering.cpp   Formal args, return, call lowering
  PenumbraLegalizerInfo.cpp  Legal type/op combinations
  PenumbraRegisterBankInfo.cpp  Single GPR bank
  PenumbraInstructionSelector.cpp  Manual select() for all ops

MCTargetDesc/
  PenumbraMCCodeEmitter.cpp  MCInst -> binary, fixup creation
  PenumbraAsmBackend.cpp     Fixup resolution, relocation emission
  PenumbraELFObjectWriter.cpp  ELF relocation type mapping
  PenumbraFixupKinds.h       Fixup kinds + MCSpecifierExpr specifiers
  PenumbraMCAsmInfo.cpp      Assembly syntax config, %lo16/%hi16 printing
  PenumbraInstPrinter.cpp    MCInst -> assembly text

AsmParser/
  PenumbraAsmParser.cpp      Assembly text -> MCInst, LI/LA/NOP/RET expansion

TargetInfo/
  PenumbraTargetInfo.cpp     Target registration (Triple::penumbra)
```

## lld Integration

`llvm/lld/ELF/Arch/Penumbra.cpp` provides the linker target.  Handles
6 relocation types.  Linker script `hw/rom/rom.ld` places code at
0xFFFF_E000 (boot ROM address).

## Building

```sh
cd build/llvm
cmake -G Ninja \
  -DLLVM_TARGETS_TO_BUILD=Penumbra \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  ../../llvm/llvm
ninja -j4 clang lld llvm-mc llvm-objcopy
```

## Known Limitations

- `-O1+` triggers unlegalized generic ops (G_SMAX, MUL/DIV)
- `%lo16()`/`%hi16()` assembly syntax not parseable (use `clang -c`, not `-S` + `llvm-mc`)
- No function calls from C yet (lowerCall is a stub)
- No MUL/DIV hardware support (trap + SW emulation planned)
