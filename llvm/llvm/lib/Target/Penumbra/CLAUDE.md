# Penumbra LLVM Backend — Claude Code Context

This file provides LLVM backend context for work under `llvm/`. The root `CLAUDE.md` has project-wide conventions.

## Build
- Build from `llvm/llvm/`, build dir `build/llvm/`
- `cmake -G Ninja -DLLVM_TARGETS_TO_BUILD=Penumbra`
- Uses ccache and Ninja
- Use `-j2` for link steps (debug builds OOM at full parallelism on 15 GB WSL2)
- Target triple: `penumbra-unknown-none` (eventually `penumbra-unknown-netbsd`)

## Current State
MC-layer assembler produces working ELF objects and raw hex. `llvm-mc -triple=penumbra` parses, matches, encodes, and emits all 4 instruction formats. Fixups for branch22 (PC-relative) and imm16 (absolute) are fully implemented — local labels resolve correctly. ELF relocations defined (R_PENUMBRA_32, R_PENUMBRA_BRANCH22, R_PENUMBRA_IMM16). Encodings verified bit-for-bit against `pasm.py`. Assembly syntax accepts ARM-style `#` prefix on immediates (optional).

GlobalISel codegen pipeline is functional: `llc -march=penumbra -global-isel` compiles LLVM IR to Penumbra assembly. Supports i32 ALU ops (add/sub/and/or/xor/shifts), constants (LLI/LLIS/LUI), s32 loads/stores with frame-index folding, full calling convention (R1-R4 args, R1 return), control flow (G_ICMP+G_BRCOND folded to CMP+Bcc, G_BR, G_PHI), and G_SELECT (conditional select via branch diamond, with G_ICMP fold into CMP+Bcc). No SelectionDAG — GlobalISel only.

## File Map (`llvm/llvm/lib/Target/Penumbra/`)

| File | Description |
|------|-------------|
| `Penumbra.td` | Top-level TableGen: includes, ProcessorModel, AsmWriter, Target, pointer remap |
| `PenumbraRegisterInfo.td` | 16 GPRs (R0=zero, R12=TP, R13=LR, R14=SP, R15=PC), GPR/GPR_Allocatable/CCR classes, HWEncoding |
| `PenumbraInstrInfo.td` | All 4 instruction formats (R/L/M/B) with bit-accurate encoding. Tied-operand constraints for 2-address ops. Custom `brtarget22` and `imm16op` operand types with encoder methods |
| `PenumbraCallingConv.td` | CC_Penumbra (R1-R4 args, stack overflow), RetCC_Penumbra (R1), CSR_Penumbra (R5-R10) |
| `PenumbraRegisterInfo.{h,cpp}` | Reserved regs (R0, R12, R14, R15), callee-saved, eliminateFrameIndex, getFrameRegister(R14) |
| `PenumbraFrameLowering.{h,cpp}` | StackGrowsDown, Align(4), hasFPImpl()=false. Prologue (SUBi SP) / epilogue (ADDi SP) |
| `PenumbraISelLowering.{h,cpp}` | TargetLowering: addRegisterClass(i32, GPR_Allocatable), getCCAssignFn(), EmitInstrWithCustomInserter (SELECT_GPR/SELECT_CC_GPR diamond expansion) |
| `PenumbraSubtarget.{h,cpp}` | Central hub: owns InstrInfo, FrameLowering, TLInfo, and GlobalISel objects (CallLowering, InstructionSelector, LegalizerInfo, RegBankInfo) |
| `PenumbraTargetMachine.{h,cpp}` | Inherits `CodeGenTargetMachineImpl`, data layout `e-m:e-p:32:32-i32:32-n32-S32`, PenumbraPassConfig (GlobalISel pipeline) |
| `PenumbraAsmPrinter.cpp` | MachineInstr → MCInst emission. Expands RET pseudo to JMP R13, handles COPY and stack pseudos |
| `GISel/PenumbraCallLowering.{h,cpp}` | lowerFormalArguments (R1-R4 → vregs), lowerReturn (vreg → R1 + RET), lowerCall (stub) |
| `GISel/PenumbraLegalizerInfo.{h,cpp}` | Legal ops: G_ADD/SUB/AND/OR/XOR/SHL/SHR/SAR on s32, G_LOAD/STORE s32, G_CONSTANT s32/p0, G_FRAME_INDEX p0, G_ICMP {s1,s32}, G_SELECT {s32/p0,s1}, G_PHI, G_BRCOND |
| `GISel/PenumbraRegisterBankInfo.{h,cpp}` | Single GPR bank covering all 16 registers. Maps all ops to GPR |
| `GISel/PenumbraRegisterBanks.td` | `def GPRRegBank : RegisterBank<"GPRBank", [GPR]>` |
| `GISel/PenumbraInstructionSelector.cpp` | Manual select(): ALU ops, G_CONSTANT (LLI/LLIS/LUI), G_LOAD/G_STORE (frame-index folding into LDW/STW), G_FRAME_INDEX, G_ICMP+G_BRCOND fold (CMP+Bcc), G_BR, G_PHI, G_SELECT (ICMP fold into SELECT_CC_GPR, fallback SELECT_GPR), standalone G_ICMP (0/1 via SELECT_CC_GPR), COPY constraint |
| `MCTargetDesc/PenumbraMCAsmInfo.{h,cpp}` | ELF-based, little-endian, `;` comments |
| `MCTargetDesc/PenumbraMCTargetDesc.{h,cpp}` | Registers all MC components |
| `MCTargetDesc/PenumbraInstPrinter.{h,cpp}` | MCInst → assembly text |
| `MCTargetDesc/PenumbraMCCodeEmitter.cpp` | MCInst → binary bytes. Custom `encodeBranchTarget` and `encodeImm16` create fixups |
| `MCTargetDesc/PenumbraAsmBackend.cpp` | Fixup resolution, NOP emission (`0x00000000` = ADD R0,R0), ELF object writer |
| `MCTargetDesc/PenumbraELFObjectWriter.cpp` | ELF relocation mapping (`EM_PENUMBRA = 0xF0DA`) |
| `MCTargetDesc/PenumbraFixupKinds.h` | `fixup_penumbra_branch22` (bits [25:4]), `fixup_penumbra_imm16` (bits [15:0]) |
| `AsmParser/PenumbraAsmParser.cpp` | Assembly text → MCInst. Registers, immediates (optional `#`), memory operands |
| `TargetInfo/PenumbraTargetInfo.{h,cpp}` | Target registration (`Triple::penumbra`) |

## Triple Integration
`penumbra` added to `Triple.h` (arch enum), `Triple.cpp` (name, prefix, parsing, 32-bit, little-endian, ELF format, DwarfCFI). Also in `llvm/llvm/CMakeLists.txt` `LLVM_ALL_TARGETS`. Note: `TargetDataLayout.cpp:computeDataLayout()` has a `-Wswitch` warning for unhandled `penumbra` — harmless (we provide our own data layout).

## Hex Output Pipeline
`llvm-mc` → ELF object → `llvm-objcopy -O binary` → `bin2hex.py` → `$readmemh` hex. Same approach as ARM/RISC-V embedded. `bin2hex.py` (`sw/tools/bin2hex.py`) reads flat LE binary, emits one 32-bit word per line in uppercase hex.

## Key Implementation Notes
- **applyFixup Data pointer:** Pre-positioned at fixup location — do NOT add `Fixup.getOffset()`. Use `Data[i]` directly.
- **PC-relativity:** Set on `MCFixup` itself (`PCRel=true` in `MCFixup::create`), not in `MCFixupKindInfo`.
- **Destructive 2-operand ops:** TableGen patterns use tied-operand constraints. Register allocator handles via COPY insertion.
