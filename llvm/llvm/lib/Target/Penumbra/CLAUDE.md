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

Codegen infrastructure in progress: CallingConv.td, RegisterInfo, FrameLowering header committed. Using GlobalISel (not SelectionDAG).

## File Map (`llvm/llvm/lib/Target/Penumbra/`)

| File | Description |
|------|-------------|
| `Penumbra.td` | Top-level TableGen: includes, ProcessorModel, AsmWriter, Target, pointer remap |
| `PenumbraRegisterInfo.td` | 16 GPRs (R0=zero, R12=TP, R13=LR, R14=SP, R15=PC), GPR/GPR_Allocatable/CCR classes, HWEncoding |
| `PenumbraInstrInfo.td` | All 4 instruction formats (R/L/M/B) with bit-accurate encoding. Tied-operand constraints for 2-address ops. Custom `brtarget22` and `imm16op` operand types with encoder methods |
| `PenumbraCallingConv.td` | CC_Penumbra (R1-R4 args, stack overflow), RetCC_Penumbra (R1), CSR_Penumbra (R5-R10) |
| `PenumbraRegisterInfo.{h,cpp}` | Reserved regs (R0, R12, R14, R15), callee-saved, eliminateFrameIndex, getFrameRegister(R14) |
| `PenumbraFrameLowering.h` | StackGrowsDown, Align(4), hasFPImpl()=false. Prologue/epilogue declared (no .cpp yet) |
| `PenumbraTargetMachine.{h,cpp}` | Inherits `CodeGenTargetMachineImpl`, data layout `e-m:e-p:32:32-i32:32-n32-S32` |
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
