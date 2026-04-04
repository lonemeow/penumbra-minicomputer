# Penumbra LLVM Backend — Claude Code Context

This file provides LLVM backend context for work under `llvm/`. The root `CLAUDE.md` has project-wide conventions.

## Build
- Build from `llvm/llvm/`, build dir `build/llvm/` (overridable via `LLVM_PREFIX` in Makefile)
- ```
  cmake -G Ninja -DLLVM_TARGETS_TO_BUILD=Penumbra \
    -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_USE_SPLIT_DWARF=ON \
    -DLLVM_INCLUDE_TESTS=ON -DLLVM_BUILD_TESTS=ON \
    -DLLVM_PARALLEL_LINK_JOBS=2
  ```
- Uses ccache and Ninja
- `-DLLVM_PARALLEL_LINK_JOBS=2` limits link parallelism
  (debug builds OOM at full parallelism on 15 GB WSL2)
- Target triple: `penumbra-unknown-none` (eventually `penumbra-unknown-netbsd`)
- Run codegen tests: `build/llvm/bin/llvm-lit llvm/llvm/test/CodeGen/Penumbra/`
- Regenerate CHECK lines:
  `python3 llvm/llvm/utils/update_llc_test_checks.py
  --llc-binary build/llvm/bin/llc <test>.ll`
- Penumbra registered in `utils/UpdateTestChecks/asm.py`
  (reuses AVR scrubber/function-RE)

## Current State
**End-to-end functional.** C boot ROM compiles with clang,
links with lld, and runs on the simulated Penumbra CPU
(prints "Penumbra/1" via UART).

**MC-layer assembler:** `llvm-mc -triple=penumbra` encodes all 4
instruction formats.
- Nine fixup/relocation types: branch22, imm16, memoffset16,
  lo16, hi16, memoffset16_pcrel, imm16_pcrel, 32, none.
- Pseudo-instructions LI, LA, NOP, RET expanded in the AsmParser.
- `%lo16()`/`%hi16()`/`%pcrel()` MCSpecifierExpr modifiers
  parsed and printed (full `clang -S` → `llvm-mc` roundtrip works).
- Register aliases (pc, sp, lr, zero, tp),
  SPR names (epc, esr, usp),
  `[Rb]` without offset,
  and expression offsets (`[pc + label - .]`) all supported.
- RDSPR/WRSPR/RDSYS/WRSYS instructions fully encoded.
- Relocation type names registered in `ELFRelocs/Penumbra.def`
  for `llvm-readobj`/`llvm-objdump`.

**GlobalISel codegen (hybrid TableGen + C++):**
Simple 1:1 patterns are expressed as TableGen `Pat<>` rules in
`PenumbraGISel.td` and imported via `selectImpl()`.
Complex multi-instruction sequences remain in manual C++.

**TableGen handles:**
ALU reg-reg (ADD/SUB/AND/OR/XOR),
ALU reg-imm (ADDi/SUBi/ANDi with uimm16),
shifts (SHL/SHR/SAR reg and SHLi/SHRi/SARi with uimm5),
NOT, simple constants (LLI for uimm16, LLIS for simm16neg),
all load variants (LDW/LDH/LDHS/LDB/LDBS for i32 and p0,
s1 widened to s8),
all store variants (STW/STH/STB with GPRz zero-register
substitution, s1 widened to s8).

**Manual C++ handles:**
wide constants (LLI+LUI),
global addresses (static: LLI+LUI with lo16/hi16;
PIC: MOV PC + ADDi %pcrel with +4 addend correction),
frame-index folding into load/store + LEAfi for escaped addresses,
G_PTR_ADD (→ADD), G_PTRMASK (→AND),
G_INTTOPTR/G_PTRTOINT/G_FREEZE (→COPY),
G_IMPLICIT_DEF (→IMPLICIT_DEF),
G_ICMP+G_BRCOND fold (CMP+Bcc with pointer compare),
G_BR, G_PHI,
G_SELECT (ICMP fold into SELECT_CC_GPR),
G_ZEXT/G_SEXT,
jump tables (static: LLI+LUI base + absolute entries;
PIC: MOV PC + ADDi base + EK_LabelDifference32 entries
+ ADD base back).

**Other features:**
- Calling convention: R1-R4 args, R1 return / R1:R2 for i64,
  R13/LR callee-saved.
- i64 support: return/args split into R1:R2 pairs,
  bitwise ops narrowed to per-half i32,
  multi-word compare via XOR+OR,
  add/sub via G_UADDO/G_UADDE lowering to ADD+CMP carry sequences;
  hardware ADC/SBC exist but need CCR register bank
  for GlobalISel to use them;
  zext/sext i32→i64 via narrowScalarIf splitting.
- Extensions: G_ANYEXT/G_TRUNC/G_SEXT_INREG.
- MUL/DIV/REM: s32 strength-reduced when possible
  (constant power-of-2 MUL→SHL, power-of-2±1 MUL→SHL+ADD/SUB,
  power-of-2 UDIV→LSHR, power-of-2 UREM→AND),
  otherwise libcalls;
  s64 all via libcalls (__muldi3/__udivdi3/__umoddi3/etc.).
- G_MEMCPY/G_MEMMOVE/G_MEMSET via libcalls.
- Varargs: G_VASTART custom-lowered, G_VAARG generic lowering,
  R1-R4 save area in variadic prologues.
- No SelectionDAG — GlobalISel only.

**Inline assembly:**
`asm volatile("..." : "=r"(out) : "r"(in) : "cc", "memory")` works.
Supported constraints: `r` (GPR), `i` (immediate),
`~{cc}` (condition flags clobber → SR),
`~{memory}` (compiler memory fence).
InlineAsmLowering wired into GlobalISel via subtarget.

**Clang:** `clang --target=penumbra-unknown-none -c file.c` works
at `-O0` through `-O2`.
Boot ROM compiles and runs correctly at all three levels.
`-fPIC` supported: uses PC-relative addressing
(MOV PC + ADDi %pcrel) for globals,
label-difference jump table entries.
NetBSD stage 1 bootloader builds fully PIC.
Higher levels or new code patterns may trigger
unlegalized ops (G_SMAX, etc.).

**lld:** `ld.lld -T rom.ld` links Penumbra ELF objects.
Supports all 6 relocation types.
EM_PENUMBRA (0xF0DA) defined in central `llvm/BinaryFormat/ELF.h`.

## File Map (`llvm/llvm/lib/Target/Penumbra/`)

| File | Description |
|------|-------------|
| `Penumbra.td` | Top-level TableGen: includes, ProcessorModel, AsmWriter, Target, pointer remap |
| `PenumbraRegisterInfo.td` | 16 GPRs (R0=zero, R12=TP, R13=LR, R14=SP, R15=PC), alt names, GPR/GPR\_Allocatable/CCR classes, HWEncoding |
| `PenumbraInstrInfo.td` | All 4 formats (R/L/M/B) with bit-accurate encoding. Tied-operand constraints for 2-addr ops. ADC/SBC Uses=[SR]. `GPRz` with GIZeroRegister=R0. Pseudos: RET, LEAfi, SELECT\_GPR, SELECT\_CC\_GPR, ADJCALLSTACK |
| `PenumbraGISel.td` | ~28 TableGen `Pat<>` rules: ALU reg-reg/reg-imm, shifts, NOT, constants (LLI/LLIS), all load/store (i32/p0). ImmLeaf predicates: uimm16, simm16, simm16neg, uimm5 |
| `PenumbraCallingConv.td` | CC\_Penumbra (R1-R4 args, stack overflow), RetCC\_Penumbra (R1, R2 for i64), CSR\_Penumbra (R5-R10, R13) |
| `PenumbraRegisterInfo.{h,cpp}` | Reserved regs (R0, R12, R14, R15), callee-saved, eliminateFrameIndex, getFrameRegister(R14) |
| `PenumbraFrameLowering.{h,cpp}` | StackGrowsDown, Align(4), hasFPImpl()=false. Prologue (SUBi SP) / epilogue (ADDi SP) |
| `PenumbraISelLowering.{h,cpp}` | TargetLowering: JT encoding (EK\_LabelDifference32 for PIC, EK\_BlockAddress for static), SELECT diamond expansion, inline asm (`r`→GPR\_Allocatable, `{cc}`→SR/CCR) |
| `PenumbraSubtarget.{h,cpp}` | Central hub: owns InstrInfo, FrameLowering, TLInfo, and all GlobalISel objects |
| `PenumbraTargetMachine.{h,cpp}` | Data layout `e-m:e-p:32:32-i32:32-i64:64-n32-S32`, GlobalISel pipeline, `setGlobalISel(true)`. PIC via `-fPIC` |
| `PenumbraAsmPrinter.cpp` | MachineInstr → MCInst. Expands RET→JMP R13. Wraps globals/JTI with lo16/hi16/pcrel MCSpecifierExpr. PrintAsmOperand for inline asm |
| `PenumbraMachineFunctionInfo.h` | Per-function state: VarArgsFrameIndex for variadic R1-R4 save area |
| `GISel/PenumbraCallLowering.{h,cpp}` | lowerFormalArguments (R1-R4→vregs, variadic save area), lowerReturn (vreg→R1+RET), lowerCall |
| `GISel/PenumbraLegalizerInfo.{h,cpp}` | See "Legalization" section below |
| `GISel/PenumbraRegisterBankInfo.{h,cpp}` | Single GPR bank covering all 16 registers. Maps all ops to GPR |
| `GISel/PenumbraRegisterBanks.td` | `def GPRRegBank : RegisterBank<"GPRBank", [GPR]>` |
| `GISel/PenumbraInstructionSelector.cpp` | Hybrid: `selectImpl()` for TableGen patterns, manual C++ for complex cases. See "Manual C++ handles" above for full list. LLI+LUI pairs use SSA-correct intermediate vregs. |
| `MCTargetDesc/PenumbraMCTargetDesc.{h,cpp}` | Registers all MC components |
| `MCTargetDesc/PenumbraInstPrinter.{h,cpp}` | MCInst → assembly text |
| `MCTargetDesc/PenumbraMCCodeEmitter.cpp` | MCInst → binary bytes. Custom `encodeBranchTarget` and `encodeImm16` create fixups |
| `MCTargetDesc/PenumbraAsmBackend.cpp` | Fixup resolution (branch22, imm16, lo16, hi16), `maybeAddReloc` for ELF relocs, NOP = `0x00000000` (ADD R0,R0) |
| `MCTargetDesc/PenumbraELFObjectWriter.cpp` | ELF relocation mapping. Uses `EM_PENUMBRA` from `llvm/BinaryFormat/ELF.h` |
| `MCTargetDesc/PenumbraFixupKinds.h` | Fixup kinds (branch22, imm16, memoffset16, lo16, hi16, pcrel variants) and MCSpecifierExpr values (S\_Lo16, S\_Hi16, S\_PCRel) |
| `MCTargetDesc/PenumbraMCAsmInfo.{h,cpp}` | ELF-based, little-endian, `;` comments. `printSpecifierExpr` for `%lo16()`/`%hi16()`/`%pcrel()` |
| `AsmParser/PenumbraAsmParser.cpp` | Assembly text → MCInst. Pseudo expansion: LI→LLI/LLIS/LUI, LA→LLI+LUI, NOP→ADD R0,R0, RET→JMP R13. Regs, imms, mem operands |
| `TargetInfo/PenumbraTargetInfo.{h,cpp}` | Target registration (`Triple::penumbra`) |

## Triple Integration
`penumbra` added to `Triple.h` (arch enum), `Triple.cpp`
(name, prefix, parsing, 32-bit, little-endian, ELF format, DwarfCFI).
Also in `llvm/llvm/CMakeLists.txt` `LLVM_ALL_TARGETS`.
Note: `TargetDataLayout.cpp:computeDataLayout()` has a `-Wswitch`
warning for unhandled `penumbra` — harmless
(we provide our own data layout).

## Hex Output Pipeline
`llvm-mc` → ELF object → `llvm-objcopy -O binary`
→ `bin2hex.py` → `$readmemh` hex.
Same approach as ARM/RISC-V embedded.
`bin2hex.py` (`sw/tools/bin2hex.py`) reads flat LE binary,
emits one 32-bit word per line in uppercase hex.

## lld Support (`llvm/lld/ELF/Arch/Penumbra.cpp`)
Minimal ELF linker target.
Handles all 8 relocation types
(including PC-relative memoffset and imm16 for PIC).
Registered via `EM_PENUMBRA` (0xF0DA) in `llvm/BinaryFormat/ELF.h`.
Emulation string `elf32penumbra`, output format `elf32-penumbra`.
Triple mapping for `Triple::penumbra` in `InputFiles.cpp`.

## ELF Relocations
| Type | Value | Description | Field |
|------|-------|-------------|-------|
| `R_PENUMBRA_NONE` | 0 | No relocation | — |
| `R_PENUMBRA_32` | 1 | Absolute 32-bit (.word symbol) | Full word |
| `R_PENUMBRA_BRANCH22` | 2 | PC-relative 22-bit word offset | bits [25:4] |
| `R_PENUMBRA_IMM16` | 3 | 16-bit immediate | bits [15:0] |
| `R_PENUMBRA_LO16` | 4 | Low 16 bits of absolute address | bits [15:0] |
| `R_PENUMBRA_HI16` | 5 | High 16 bits of absolute address | bits [15:0] |
| `R_PENUMBRA_MEMOFFSET16_PCREL` | 6 | PC-relative 16-bit memory offset | bits [17:2] |
| `R_PENUMBRA_IMM16_PCREL` | 7 | PC-relative 16-bit immediate | bits [15:0] |

## Legalization (`GISel/PenumbraLegalizerInfo.{h,cpp}`)
- **Legal s32:** G_ADD, G_SUB, G_AND, G_OR, G_XOR,
  G_SHL/G_LSHR/G_ASHR (both operands clamped to s32),
  G_LOAD/G_STORE (s32/s16/s8),
  G_CONSTANT (s32/p0), G_FRAME_INDEX/G_GLOBAL_VALUE (p0),
  G_PTR_ADD/G_PTRMASK {p0,s32},
  G_INTTOPTR/G_PTRTOINT {p0,s32},
  G_ICMP {s1,s32}/{s1,p0}, G_SELECT {s32/p0,s1},
  G_PHI, G_BRCOND, G_FREEZE (no-op).
- **Extensions:** G_ZEXT/G_SEXT/G_ANYEXT sub-word→s32 legal,
  s32→s64 via narrowScalarIf. G_TRUNC legal.
  G_SEXT_INREG lowered.
- **MUL/DIV/REM:** Custom s32 (strength-reduce power-of-2 constants
  to shifts/logic, libcall fallback).
  G_SDIV/G_SREM libcall s32+s64. s64 all via libcalls.
- **Lowered:** G_ABS, G_CTTZ/G_CTLZ/G_CTPOP
  (and \_ZERO\_UNDEF variants) to shift/logic.
- **Libcall:** G_MEMCPY/G_MEMMOVE/G_MEMSET.
- **Custom:** G_VASTART, G_MUL, G_UDIV, G_UREM
  (via legalizeCustom() override). G_VAARG lowered.

## Key Implementation Notes
- **applyFixup Data pointer:** Pre-positioned at fixup location —
  do NOT add `Fixup.getOffset()`. Use `Data[i]` directly.
- **maybeAddReloc:** Must be called at the start of `applyFixup()`
  to generate ELF relocations for unresolved symbols.
  Without it, all symbol references silently resolve to zero.
- **PC-relativity:** Set on `MCFixup` itself
  (`PCRel=true` in `MCFixup::create`), not in `MCFixupKindInfo`.
- **Destructive 2-operand ops:** TableGen patterns use
  tied-operand constraints. Register allocator handles via COPY.
- **Global address materialization (static):**
  Instruction selector emits LLI+LUI with target flags
  (`S_Lo16`/`S_Hi16`).
  AsmPrinter converts flags to `MCSpecifierExpr` wrappers.
  MCCodeEmitter maps specifiers to
  `fixup_penumbra_lo16`/`fixup_penumbra_hi16`.
  LLI+LUI pairs use an intermediate vreg
  (`%tmp = LLI lo` → `%dst = LUI %tmp, hi`)
  for SSA correctness — required for `-O1+` passes
  like OptimizePHIs.
- **Global address materialization (PIC):**
  `MOV Rd, PC` + `ADDi Rd, %pcrel(sym+4)`.
  The +4 addend compensates for MOV capturing PC of itself
  (4 bytes before ADDi).
  Linker resolves `%pcrel(X)` as `X - addr_of_instruction`.
  Math: `MOV_addr + (sym + 4 - ADDi_addr)
  = MOV_addr + (sym + 4 - (MOV_addr + 4)) = sym`. ✓
  PC reads as current instruction address (no pipeline offset).
- **PIC jump tables:** `EK_LabelDifference32` entries
  (`.word target - JT_base`).
  BRJT expansion adds base back:
  `LDW offset,[entry_addr]` → `ADD offset, base` → `JMP`.
  JTI operands get +4 addend in AsmPrinter
  (same MOV+ADDi correction).
  TODO: optimize to 16-bit entries via EK_Inline
  when all offsets fit ±32KB.
- **Assembly text roundtrip:** `%lo16()`/`%hi16()`/`%pcrel()`
  syntax parsed by AsmParser's operand parser and emitted by
  `printSpecifierExpr`.
  Full `clang -S` → `llvm-mc` roundtrip works.
- **Register class constraining:** All instruction selector helpers
  must call `constrainSelectedInstRegOperands()` —
  vregs left with only a bank assignment (no regclass)
  cause assertions after selection.
