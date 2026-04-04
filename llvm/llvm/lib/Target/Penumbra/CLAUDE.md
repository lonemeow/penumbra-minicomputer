# Penumbra LLVM Backend — Claude Code Context

This file provides LLVM backend context for work under `llvm/`. The root `CLAUDE.md` has project-wide conventions.

## Build
- Build from `llvm/llvm/`, build dir `build/llvm/` (overridable via `LLVM_PREFIX` in Makefile)
- `cmake -G Ninja -DLLVM_TARGETS_TO_BUILD=Penumbra -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_USE_SPLIT_DWARF=ON -DLLVM_INCLUDE_TESTS=ON -DLLVM_BUILD_TESTS=ON -DLLVM_PARALLEL_LINK_JOBS=2`
- Uses ccache and Ninja
- `-DLLVM_PARALLEL_LINK_JOBS=2` limits link parallelism (debug builds OOM at full parallelism on 15 GB WSL2)
- Target triple: `penumbra-unknown-none` (eventually `penumbra-unknown-netbsd`)
- Run codegen tests: `build/llvm/bin/llvm-lit llvm/llvm/test/CodeGen/Penumbra/`
- Regenerate CHECK lines: `python3 llvm/llvm/utils/update_llc_test_checks.py --llc-binary build/llvm/bin/llc <test>.ll`
- Penumbra registered in `utils/UpdateTestChecks/asm.py` (reuses AVR scrubber/function-RE)

## Current State
**End-to-end functional.** C boot ROM compiles with clang, links with lld, and runs on the simulated Penumbra CPU (prints "Penumbra/1" via UART).

**MC-layer assembler:** `llvm-mc -triple=penumbra` encodes all 4 instruction formats. Seven fixup/relocation types: branch22, imm16, memoffset16, lo16, hi16, 32, none. Pseudo-instructions LI, LA, NOP, RET expanded in the AsmParser. `%lo16()`/`%hi16()` MCSpecifierExpr modifiers parsed and printed (full `clang -S` → `llvm-mc` roundtrip works). Register aliases (pc, sp, lr, zero, tp), SPR names (epc, esr, usp), `[Rb]` without offset, and expression offsets (`[pc + label - .]`) all supported. RDSPR/WRSPR/RDSYS/WRSYS instructions fully encoded.

**GlobalISel codegen (hybrid TableGen + C++):** Simple 1:1 patterns are expressed as TableGen `Pat<>` rules in `PenumbraGISel.td` and imported via `selectImpl()`. Complex multi-instruction sequences remain in manual C++. **TableGen handles:** ALU reg-reg (ADD/SUB/AND/OR/XOR), ALU reg-imm (ADDi/SUBi/ANDi with uimm16), shifts (SHL/SHR/SAR reg and SHLi/SHRi/SARi with uimm5), NOT, simple constants (LLI for uimm16, LLIS for simm16neg), all load variants (LDW/LDH/LDHS/LDB/LDBS for i32 and p0), all store variants (STW/STH/STB with GPRz zero-register substitution). **Manual C++ handles:** wide constants (LLI+LUI), global addresses (LLI+LUI with lo16/hi16 via shared `emitLoadSymbolAddr`), frame-index folding into load/store + LEAfi for escaped addresses, G_PTR_ADD (→ADD), G_PTRMASK (→AND), G_INTTOPTR/G_PTRTOINT/G_FREEZE (→COPY), G_ICMP+G_BRCOND fold (CMP+Bcc with pointer compare), G_BR, G_PHI, G_SELECT (ICMP fold into SELECT_CC_GPR), G_ZEXT/G_SEXT, jump tables (G_JUMP_TABLE + G_BRJT → SHLi+ADD+LDW+BRIND). **Other features:** calling convention (R1-R4 args, R1 return / R1:R2 for i64, R13/LR callee-saved), i64 support (return/args split into R1:R2 pairs, bitwise ops narrowed to per-half i32, multi-word compare via XOR+OR, add/sub via G_UADDO/G_UADDE lowering to ADD+CMP carry sequences; hardware ADC/SBC exist but need CCR register bank for GlobalISel to use them; zext/sext i32→i64 via narrowScalarIf splitting), extensions (G_ANYEXT/G_TRUNC/G_SEXT_INREG), MUL/DIV/REM → s32 strength-reduced when possible (constant power-of-2 MUL→SHL, power-of-2±1 MUL→SHL+ADD/SUB, power-of-2 UDIV→LSHR, power-of-2 UREM→AND), otherwise libcalls; s64 MUL/DIV/REM all via libcalls (__muldi3/__udivdi3/__umoddi3/etc.), G_MEMCPY/G_MEMMOVE/G_MEMSET via libcalls (memcpy/memmove/memset), varargs (G_VASTART custom-lowered, G_VAARG generic lowering, R1-R4 save area in variadic prologues). No SelectionDAG — GlobalISel only.

**Inline assembly:** `asm volatile("..." : "=r"(out) : "r"(in) : "cc", "memory")` works. Supported constraints: `r` (GPR), `i` (immediate), `~{cc}` (condition flags clobber → SR), `~{memory}` (compiler memory fence). InlineAsmLowering wired into GlobalISel via subtarget.

**Clang:** `clang --target=penumbra-unknown-none -c file.c` works at `-O0` through `-O2`. Boot ROM compiles and runs correctly at all three levels. Higher levels or new code patterns may trigger unlegalized ops (G_SMAX, etc.).

**lld:** `ld.lld -T rom.ld` links Penumbra ELF objects. Supports all 6 relocation types. EM_PENUMBRA (0xF0DA) defined in central `llvm/BinaryFormat/ELF.h`.

## File Map (`llvm/llvm/lib/Target/Penumbra/`)

| File | Description |
|------|-------------|
| `Penumbra.td` | Top-level TableGen: includes, ProcessorModel, AsmWriter, Target, pointer remap |
| `PenumbraRegisterInfo.td` | 16 GPRs (R0=zero, R12=TP, R13=LR, R14=SP, R15=PC) with alt names, GPR/GPR_Allocatable/CCR classes, HWEncoding |
| `PenumbraInstrInfo.td` | All 4 instruction formats (R/L/M/B) with bit-accurate encoding. FormatR (ALU+system), FormatL (immediates + JMP/JALR), FormatL_Reg (register-only Format L), FormatM (memory), FormatB (branches). Tied-operand constraints for 2-address ops. ADC/SBC with Uses=[SR]. Custom `brtarget22` and `imm16op` operand types with encoder methods. `GPRz` RegisterOperand with `GIZeroRegister=R0` for automatic zero-register substitution. Pseudos: RET, LEAfi, SELECT_GPR, SELECT_CC_GPR, ADJCALLSTACK{DOWN,UP} |
| `PenumbraGISel.td` | TableGen imported GlobalISel patterns. ~28 `Pat<>` rules: ALU reg-reg/reg-imm, shifts reg/imm, NOT, constants (LLI/LLIS), all load/store variants (i32 and p0 types). ImmLeaf predicates: uimm16, simm16, simm16neg, uimm5. `PenumbraPtrVT` for p0-typed patterns |
| `PenumbraCallingConv.td` | CC_Penumbra (R1-R4 args, stack overflow), RetCC_Penumbra (R1, R2 for i64), CSR_Penumbra (R5-R10, R13) |
| `PenumbraRegisterInfo.{h,cpp}` | Reserved regs (R0, R12, R14, R15), callee-saved, eliminateFrameIndex, getFrameRegister(R14) |
| `PenumbraFrameLowering.{h,cpp}` | StackGrowsDown, Align(4), hasFPImpl()=false. Prologue (SUBi SP) / epilogue (ADDi SP) |
| `PenumbraISelLowering.{h,cpp}` | TargetLowering: addRegisterClass(i32, GPR_Allocatable), getCCAssignFn(), EmitInstrWithCustomInserter (SELECT_GPR/SELECT_CC_GPR diamond expansion), inline asm constraints (getConstraintType, getRegForInlineAsmConstraint: `r`→GPR_Allocatable, `{cc}`→SR/CCR) |
| `PenumbraSubtarget.{h,cpp}` | Central hub: owns InstrInfo, FrameLowering, TLInfo, and GlobalISel objects (CallLowering, InlineAsmLowering, InstructionSelector, LegalizerInfo, RegBankInfo) |
| `PenumbraTargetMachine.{h,cpp}` | Inherits `CodeGenTargetMachineImpl`, data layout `e-m:e-p:32:32-i32:32-i64:64-n32-S32`, PenumbraPassConfig (GlobalISel pipeline), `setGlobalISel(true)` |
| `PenumbraAsmPrinter.cpp` | MachineInstr → MCInst emission. Expands RET pseudo to JMP R13, handles COPY and stack pseudos. Wraps MO_GlobalAddress with lo16/hi16 MCSpecifierExpr based on target flags. PrintAsmOperand/PrintAsmMemoryOperand for inline asm template expansion |
| `PenumbraMachineFunctionInfo.h` | Per-function state: VarArgsFrameIndex (frame index of R1-R4 save area for variadic functions) |
| `GISel/PenumbraCallLowering.{h,cpp}` | lowerFormalArguments (R1-R4 → vregs; variadic: spills R1-R4 to save area, records VarArgsFrameIndex), lowerReturn (vreg → R1 + RET), lowerCall |
| `GISel/PenumbraLegalizerInfo.{h,cpp}` | Legal ops: G_ADD/SUB/AND/OR/XOR on s32, G_SHL/SHR/SAR on {s32,s32} (value+shift amount both clamped), G_LOAD/STORE s32/s16/s8, G_CONSTANT s32/p0, G_FRAME_INDEX/G_GLOBAL_VALUE p0, G_PTR_ADD/G_PTRMASK {p0,s32}, G_INTTOPTR/G_PTRTOINT {p0,s32}, G_ICMP {s1,s32}/{s1,p0}, G_SELECT {s32/p0,s1}, G_PHI, G_BRCOND, G_ZEXT/G_SEXT/G_ANYEXT (sub-word→s32 legal, s32→s64 narrowScalarIf), G_TRUNC, G_SEXT_INREG (lowered), G_MUL/G_UDIV/G_UREM (custom s32: strength-reduce power-of-2 constants to shifts/logic, libcall fallback; s64 libcall), G_SDIV/G_SREM (libcall s32+s64), G_ABS (lowered), G_FREEZE (legal, no-op), G_MEMCPY/G_MEMMOVE/G_MEMSET (libcall), G_CTTZ/G_CTLZ/G_CTPOP and _ZERO_UNDEF variants (lowered to shift/logic), G_VASTART (custom), G_VAARG (lowered). Custom legalizeCustom() override for G_VASTART, G_MUL, G_UDIV, G_UREM. |
| `GISel/PenumbraRegisterBankInfo.{h,cpp}` | Single GPR bank covering all 16 registers. Maps all ops to GPR |
| `GISel/PenumbraRegisterBanks.td` | `def GPRRegBank : RegisterBank<"GPRBank", [GPR]>` |
| `GISel/PenumbraInstructionSelector.cpp` | Hybrid selector: calls `selectImpl()` for TableGen-imported patterns, then manual C++ for complex cases. Manual handles: wide constants (LLI+LUI), G_GLOBAL_VALUE (LLI+LUI with lo16/hi16 target flags), G_LOAD/G_STORE frame-index folding, G_PTR_ADD (→ADD), G_PTRMASK (→AND), G_INTTOPTR/G_PTRTOINT/G_FREEZE (→COPY), G_FRAME_INDEX (fold into memop or LEAfi for escaped addresses), G_ICMP+G_BRCOND fold (CMP+Bcc), G_BR, G_PHI, G_SELECT, G_ZEXT/G_SEXT, COPY constraint. Simple ALU/shift/constant/load/store patterns now handled by TableGen. LLI+LUI pairs use SSA-correct intermediate vregs. |
| `MCTargetDesc/PenumbraMCTargetDesc.{h,cpp}` | Registers all MC components |
| `MCTargetDesc/PenumbraInstPrinter.{h,cpp}` | MCInst → assembly text |
| `MCTargetDesc/PenumbraMCCodeEmitter.cpp` | MCInst → binary bytes. Custom `encodeBranchTarget` and `encodeImm16` create fixups |
| `MCTargetDesc/PenumbraAsmBackend.cpp` | Fixup resolution (branch22, imm16, lo16, hi16), `maybeAddReloc` for ELF relocations, NOP emission (`0x00000000` = ADD R0,R0) |
| `MCTargetDesc/PenumbraELFObjectWriter.cpp` | ELF relocation mapping. Uses `EM_PENUMBRA` from `llvm/BinaryFormat/ELF.h` |
| `MCTargetDesc/PenumbraFixupKinds.h` | Fixup kinds (branch22, imm16, lo16, hi16) and MCSpecifierExpr specifier values (S_Lo16, S_Hi16) |
| `MCTargetDesc/PenumbraMCAsmInfo.{h,cpp}` | ELF-based, little-endian, `;` comments. `printSpecifierExpr` for `%lo16()`/`%hi16()` output |
| `AsmParser/PenumbraAsmParser.cpp` | Assembly text → MCInst. Pseudo-instruction expansion: LI (constant or symbol → LLI/LLIS/LUI), LA (symbol → LLI+LUI with lo16/hi16), NOP (→ ADD R0,R0), RET (→ JMP R13). Registers, immediates (optional `#`), memory operands |
| `TargetInfo/PenumbraTargetInfo.{h,cpp}` | Target registration (`Triple::penumbra`) |

## Triple Integration
`penumbra` added to `Triple.h` (arch enum), `Triple.cpp` (name, prefix, parsing, 32-bit, little-endian, ELF format, DwarfCFI). Also in `llvm/llvm/CMakeLists.txt` `LLVM_ALL_TARGETS`. Note: `TargetDataLayout.cpp:computeDataLayout()` has a `-Wswitch` warning for unhandled `penumbra` — harmless (we provide our own data layout).

## Hex Output Pipeline
`llvm-mc` → ELF object → `llvm-objcopy -O binary` → `bin2hex.py` → `$readmemh` hex. Same approach as ARM/RISC-V embedded. `bin2hex.py` (`sw/tools/bin2hex.py`) reads flat LE binary, emits one 32-bit word per line in uppercase hex.

## lld Support (`llvm/lld/ELF/Arch/Penumbra.cpp`)
Minimal ELF linker target. Handles all 6 relocation types. Registered via `EM_PENUMBRA` (0xF0DA) in `llvm/BinaryFormat/ELF.h`. Emulation string `elf32penumbra`, output format `elf32-penumbra`. Triple mapping for `Triple::penumbra` in `InputFiles.cpp`.

## ELF Relocations
| Type | Value | Description | Field |
|------|-------|-------------|-------|
| `R_PENUMBRA_NONE` | 0 | No relocation | — |
| `R_PENUMBRA_32` | 1 | Absolute 32-bit (.word symbol) | Full word |
| `R_PENUMBRA_BRANCH22` | 2 | PC-relative 22-bit word offset | bits [25:4] |
| `R_PENUMBRA_IMM16` | 3 | 16-bit immediate | bits [15:0] |
| `R_PENUMBRA_LO16` | 4 | Low 16 bits of absolute address | bits [15:0] |
| `R_PENUMBRA_HI16` | 5 | High 16 bits of absolute address | bits [15:0] |

## Key Implementation Notes
- **applyFixup Data pointer:** Pre-positioned at fixup location — do NOT add `Fixup.getOffset()`. Use `Data[i]` directly.
- **maybeAddReloc:** Must be called at the start of `applyFixup()` to generate ELF relocations for unresolved symbols. Without it, all symbol references silently resolve to zero.
- **PC-relativity:** Set on `MCFixup` itself (`PCRel=true` in `MCFixup::create`), not in `MCFixupKindInfo`.
- **Destructive 2-operand ops:** TableGen patterns use tied-operand constraints. Register allocator handles via COPY insertion.
- **Global address materialization:** Instruction selector emits LLI+LUI with target flags (`S_Lo16`/`S_Hi16`). AsmPrinter converts flags to `MCSpecifierExpr` wrappers. MCCodeEmitter maps specifiers to `fixup_penumbra_lo16`/`fixup_penumbra_hi16`. LLI+LUI pairs use an intermediate vreg (`%tmp = LLI lo` → `%dst = LUI %tmp, hi`) for SSA correctness — required for `-O1+` passes like OptimizePHIs.
- **Assembly text roundtrip:** `%lo16()`/`%hi16()` syntax is parsed by the AsmParser's operand parser and emitted by `printSpecifierExpr`. Full `clang -S` → `llvm-mc` roundtrip works.
- **Register class constraining:** All instruction selector helpers must call `constrainSelectedInstRegOperands()` — vregs left with only a bank assignment (no regclass) cause assertions after selection.
