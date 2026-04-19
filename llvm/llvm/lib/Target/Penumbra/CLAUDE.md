# Penumbra LLVM Backend — Claude Code Context

This file provides LLVM backend context for work under `llvm/`. The root `CLAUDE.md` has project-wide conventions.

## Build
Source dir: `llvm/llvm/`, build dir: `build/llvm/`
(overridable via `LLVM_PREFIX` in Makefile).

**Initial cmake (one-time):**
```sh
cmake -G Ninja -S llvm/llvm -B build/llvm \
  -DLLVM_TARGETS_TO_BUILD=Penumbra \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  -DLLVM_INCLUDE_TESTS=ON -DLLVM_BUILD_TESTS=ON \
  -DLLVM_PARALLEL_LINK_JOBS=2
```

**Incremental rebuild (target only what's needed):**
```sh
ninja -C build/llvm -j10 llc clang lld \
  llvm-mc llvm-ar llvm-nm llvm-objcopy llvm-objdump \
  llvm-readobj llvm-size llvm-strings
```
A full `ninja -C build/llvm` builds everything including unit tests —
much slower; only needed if running `llvm-lit` for the first time.

- Uses ccache and Ninja
- `-j10` for compilation, `-j2` link jobs (set in cmake) to avoid
  OOM on 15 GB WSL2
- Target triple: `penumbra-unknown-none` (bare-metal),
  `penumbra-unknown-netbsd` (NetBSD userland/kernel)

**Tests:**
```sh
build/llvm/bin/llvm-lit llvm/llvm/test/CodeGen/Penumbra/       # all tests
build/llvm/bin/llvm-lit -v llvm/llvm/test/CodeGen/Penumbra/alu.ll  # one test
```

**Regenerate CHECK lines:**
```sh
python3 llvm/llvm/utils/update_llc_test_checks.py \
  --llc-binary build/llvm/bin/llc \
  llvm/llvm/test/CodeGen/Penumbra/<test>.ll
```
- Penumbra registered in `utils/UpdateTestChecks/asm.py`
  (reuses AVR scrubber/function-RE)

## Current State
**End-to-end functional.** C boot ROM compiles with clang,
links with lld, and runs on the simulated Penumbra CPU
(prints "Penumbra/1" via UART).

**MC-layer assembler:** `llvm-mc -triple=penumbra` encodes all 4
instruction formats.
- Eleven fixup/relocation types: branch22, imm16, memoffset16,
  lo16, hi16, memoffset16_pcrel, imm16_pcrel, 32, none,
  tls_gd_lo16, tls_gd_hi16.
- Pseudo-instructions LI, LA, NOP, RET expanded in the AsmParser.
- `%lo16()`/`%hi16()`/`%pcrel()`/`%tlsgd_lo16()`/`%tlsgd_hi16()`
  MCSpecifierExpr modifiers parsed and printed
  (full `clang -S` → `llvm-mc` roundtrip works).
- Register aliases (pc, sp, lr, zero, tp),
  SPR names (epc, esr, usp, sr — context-sensitive, sr parsed
  as SPR index only in RDSPR/WRSPR context since it's also a register),
  `[Rb]` without offset,
  and expression offsets (`[pc + label - .]`) all supported.
- RDSPR/WRSPR/RDSYS/WRSYS instructions fully encoded.
- Relocation type names registered in `ELFRelocs/Penumbra.def`
  for `llvm-readobj`/`llvm-objdump`.

**Disassembler:** `llvm-mc -disassemble -triple penumbra` and
`llvm-objdump -d` decode all instruction formats.
- Auto-generated decoder tables from TableGen `Inst` bit fields.
- Custom decoders: `decodeBranchTarget` (22-bit signed word offset
  → absolute address with symbolic lookup),
  `decodeSimm16` (sign-extended for LLIS),
  `decodeMemOffset16` (sign-extended for load/store offsets).
- `ELFObjectFile.h` maps `EM_PENUMBRA` → `elf32-penumbra` / `Triple::penumbra`
  for all LLVM binary utilities.
- **MCInstrAnalysis** (in `PenumbraMCTargetDesc.cpp`):
  `evaluateBranch` resolves direct branch/call targets for
  `<symbol>` annotations in `llvm-objdump`.
  `updateState`/`evaluateMemoryOperandAddress` track GPR values
  across LLI/LUI instruction pairs, annotating LUI with the
  reconstructed 32-bit address and symbol name (printed as
  `// 0xADDR <symbol>` comment).  State is invalidated on
  terminators, calls, and any non-tracked GPR write.
  Branch targets printed as hex addresses via `printBranchTarget`.
- Lit tests: `test/MC/Disassembler/Penumbra/penumbra.txt`
  (vectors from `pasm.py` reference assembler),
  `test/MC/Penumbra/disasm-annotations.s` (branch target and
  LLI/LUI address annotations on linked binary, relocation
  display on .o file, register invalidation).

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
PIC/PIE: GOT-indirect via MOV PC + LLI/LUI got_pcrel + ADD + LDW,
5 instructions, full 32-bit reach),
frame-index folding into load/store + LEAfi for escaped addresses,
G_PTR_ADD (→ADD), G_PTRMASK (→AND),
G_INTTOPTR/G_PTRTOINT/G_FREEZE (→COPY),
G_IMPLICIT_DEF (→IMPLICIT_DEF),
G_ICMP+G_BRCOND fold (CMP+Bcc with pointer compare),
G_BR, G_PHI,
G_SELECT (ICMP fold into SELECT_CC_GPR),
G_ZEXT/G_SEXT,
jump tables (always EK_LabelDifference32 entries placed
inline in .text; base materialized via LLI+LUI (static)
or MOV PC + ADDi (PIC); BRJT always adds base back).

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
  (MUL by -1→SUB from zero, constant power-of-2 MUL→SHL,
  power-of-2±1 MUL→SHL+ADD/SUB with shift < bit_width guard,
  power-of-2 UDIV→LSHR, power-of-2 UREM→AND),
  otherwise libcalls;
  s64 all via libcalls (__muldi3/__udivdi3/__umoddi3/etc.).
- G_MEMCPY/G_MEMMOVE/G_MEMSET via libcalls.
- G_PREFETCH: custom-lowered to no-op (erased) —
  Penumbra has no cache hint instructions.
- Varargs: G_VASTART custom-lowered, G_VAARG generic lowering
  (s32/s64/p0), va_copy selected to LDW+STW pair,
  R1-R4 save area in variadic prologues.
  va_start offset uses ArgAssigner.StackSize (not SplitArgs.size())
  so it correctly handles >4 named args and wide types (e.g. i64)
  that occupy multiple stack slots.
- G_DYN_STACKALLOC: lowered by framework to SP subtract + alignment
  (VLAs, runtime-sized `alloca`).
  `setStackPointerRegisterToSaveRestore(R14)` in ISelLowering.
- G_STACKSAVE/G_STACKRESTORE: selected to MOV SP (R14).
- `@llvm.returnaddress(0)` → MOV from a vreg that captures R13 at
  the top of the entry block.  Reading R13 directly at the use site
  is wrong in non-leaf functions: every BL/JALR clobbers R13 with
  its own call-site return address, so the live R13 no longer
  holds the caller's return address.  The capture is lazy
  (only materialized when the intrinsic is actually used) and the
  vreg handle lives in `PenumbraMachineFunctionInfo` so repeated
  references share one capture.
  `@llvm.frameaddress(0)` → MOV from R14 (SP).
- **TLS (thread-local storage):** IR pass (`PenumbraLowerTLS` in
  `PenumbraTargetMachine.cpp`) checks TLS model via
  `TargetMachine::getTLSModel()`:
  **Local-Exec/Initial-Exec** (static binaries): emits inline
  `mov r,r12` (read TP) + `ptrtoint @tls_var` + `add` —
  no function call, instruction selector emits LLI+LUI with
  TLS GD relocs that lld resolves as `R_TPREL`.
  **General-Dynamic** (PIC/shared): replaces with
  `call @__tls_get_addr`; `selectGlobalValue` emits LLI+LUI
  or MOV PC + ADDi with `S_TLSgd_*` target flags.
  GOT-based dynamic resolution to be added with `ld.elf_so`.
- No SelectionDAG — GlobalISel only.
- **Branch analysis:** `analyzeBranch`/`insertBranch`/`removeBranch`/
  `reverseBranchCondition` implemented in `PenumbraInstrInfo.cpp`.
  Cond vector is single element (branch opcode).  All 14 conditional
  branch opcodes have opposite-pairs (BEQ↔BNE, BCS↔BCC, etc.).
  Enables `-O1`/`-Os`/`-O2` (branch folding, block placement).
- **G_FENCE:** Legalized as always-legal, selected to `MEMBARRIER`
  pseudo (compiler barrier, no hardware instruction — uniprocessor).
- **G_BRINDIRECT:** Legalized for p0, selected to BRIND (JMP Rd).
  Used by computed gotos (Lua VM dispatch).

**Inline assembly:**
`asm volatile("..." : "=r"(out) : "r"(in) : "cc", "memory")` works.
Supported constraints: `r` (GPR), `i` (immediate),
`~{cc}` (condition flags clobber → SR),
`~{memory}` (compiler memory fence).
InlineAsmLowering wired into GlobalISel via subtarget.

**Clang:** `clang --target=penumbra-unknown-none -c file.c` works
at `-O0` through `-O2` (bare-metal).
`clang --target=penumbra-unknown-netbsd` for NetBSD (defines `__NetBSD__`
via `NetBSDTargetInfo<>` wrapper).
Boot ROM compiles and runs correctly at all three levels.
`-fPIC`/`-fPIE` supported: GOT-indirect addressing for globals
(MOV PC + LLI/LUI got_pcrel + ADD + LDW from GOT, 32-bit reach).
TLS GD PIC uses GOT-indirect to tls_index pair
(MOV PC + LLI/LUI tlsgd_got_pcrel + ADD, 4 instructions).
Label-difference jump table entries.
`needsRelocateWithSymbol` prevents section+offset folding for
GOT/TLS relocs (addend must only contain PC adjustment).
`PenumbraToolChain` (`clang/lib/Driver/ToolChains/Penumbra.{h,cpp}`)
is the bare-metal toolchain (ROM, hw tests) — uses `ld.lld` directly.
For `penumbra-unknown-netbsd`, the stock `toolchains::NetBSD`
(`clang/lib/Driver/ToolChains/NetBSD.cpp`) is used with Penumbra
additions: `elf32penumbra` emulation, `useLibgcc=false`,
`--allow-shlib-undefined`, `--undefined-version`.
`build.sh distribution` compiles nearly all of NetBSD userland.

**lld:** `ld.lld -T rom.ld` links Penumbra ELF objects.
Supports all 21 relocation types including GOT/PLT and TLS GD.
**GOT/PLT:** PLT entries are 16 bytes (LLI+LUI+LDW+JMP using R11
scratch register).  `R_PENUMBRA_GLOB_DAT` for GOT, `R_PENUMBRA_JUMP_SLOT`
for PLT.  Shared libraries (`-shared`) work with PIC code.
**TLS GD:** PIC uses `R_PENUMBRA_TLS_GD_GOT_PCREL_LO16/HI16`
(GOT-indirect PC-relative to tls_index pair, 32-bit reach);
lld creates GOT pairs with
`R_PENUMBRA_TLS_DTPMOD32`/`R_PENUMBRA_TLS_DTPOFF32` dynamic relocs.
Static uses `R_TPREL` (Variant 1, no TCB gap, like RISC-V).
`EM_PENUMBRA` to `getTlsTpOffset` mapping in `InputSection.cpp`.
EM_PENUMBRA (0xF0DA) defined in central `llvm/BinaryFormat/ELF.h`.

## File Map (`llvm/llvm/lib/Target/Penumbra/`)

| File | Description |
|------|-------------|
| `Penumbra.td` | Top-level TableGen: includes, ProcessorModel, AsmWriter, Target, pointer remap |
| `PenumbraRegisterInfo.td` | 16 GPRs (R0=zero, R12=TP, R13=LR, R14=SP, R15=PC), alt names, GPR/GPR\_Allocatable/CCR classes, HWEncoding |
| `PenumbraInstrInfo.td` | All 4 formats (R/L/M/B) with bit-accurate encoding. Tied-operand constraints for 2-addr ops. ADC/SBC Uses=[SR]. `GPRz` with GIZeroRegister=R0. Pseudos: RET, LEAfi, SELECT\_GPR, SELECT\_CC\_GPR, ADJCALLSTACK |
| `PenumbraGISel.td` | ~28 TableGen `Pat<>` rules: ALU reg-reg/reg-imm, shifts, NOT, constants (LLI/LLIS), all load/store (i32/p0). ImmLeaf predicates: uimm16, simm16, simm16neg, uimm5 |
| `PenumbraCallingConv.td` | CC\_Penumbra (R1-R4 args, stack overflow), RetCC\_Penumbra (R1, R2 for i64), CSR\_Penumbra (R5-R10, R13) |
| `PenumbraRegisterInfo.{h,cpp}` | Reserved regs (R0, R12, R14, R15), callee-saved, getFrameRegister(R14). `eliminateFrameIndex` folds small offsets directly, expands large offsets to `LLI+LUI+ADD` via a fresh virtual register (rewritten later by the scavenger, not a fixed scratch — RA may have live values in any particular reg). `requiresRegisterScavenging`/`requiresFrameIndexScavenging` both true |
| `PenumbraFrameLowering.{h,cpp}` | StackGrowsDown, Align(4), hasFPImpl()=true when alloca present. `adjustSP` helper used by both prologue (SUB/SUBi) and epilogue (ADD/ADDi): small frames use the 16-bit immediate form, large frames (StackSize > 65535) materialize the size in R11 via LLI+LUI and use the reg-reg form. `processFunctionBeforeFrameFinalized` adds an emergency spill slot for RegScavenger when the estimated frame exceeds 15-bit signed — otherwise leaf functions don't pay for it |
| `PenumbraISelLowering.{h,cpp}` | TargetLowering: JT encoding (EK\_LabelDifference32), SELECT diamond expansion, inline asm (`r`→GPR\_Allocatable, `{cc}`→SR/CCR), `setStackPointerRegisterToSaveRestore(R14)` |
| `PenumbraSubtarget.{h,cpp}` | Central hub: owns InstrInfo, FrameLowering, TLInfo, and all GlobalISel objects |
| `PenumbraTargetMachine.{h,cpp}` | Data layout `e-m:e-p:32:32-i32:32-i64:64-n32-S32`, GlobalISel pipeline, `setGlobalISel(true)`. PIC via `-fPIC`. `PenumbraTargetObjectFile` (local class): always inlines jump tables in `.text`. `PenumbraLowerTLS` IR pass: lowers `@llvm.threadlocal.address` (GD → `__tls_get_addr` call; LE/IE → inline TP+offset) |
| `PenumbraAsmPrinter.cpp` | MachineInstr → MCInst. Expands RET→JMP R13. Wraps globals/JTI with lo16/hi16/pcrel MCSpecifierExpr. `emitJumpTableEntry` override: always emits label-difference entries. PrintAsmOperand for inline asm |
| `PenumbraMachineFunctionInfo.h` | Per-function state: VarArgsFrameIndex for variadic R1-R4 save area |
| `GISel/PenumbraCallLowering.{h,cpp}` | lowerFormalArguments (R1-R4→vregs, variadic save area), lowerReturn (vreg→R1+RET), lowerCall |
| `GISel/PenumbraLegalizerInfo.{h,cpp}` | See "Legalization" section below |
| `GISel/PenumbraRegisterBankInfo.{h,cpp}` | Single GPR bank covering all 16 registers. Maps all ops to GPR |
| `GISel/PenumbraRegisterBanks.td` | `def GPRRegBank : RegisterBank<"GPRBank", [GPR]>` |
| `GISel/PenumbraInstructionSelector.cpp` | Hybrid: `selectImpl()` for TableGen patterns, manual C++ for complex cases. See "Manual C++ handles" above for full list. LLI+LUI pairs use SSA-correct intermediate vregs. |
| `Disassembler/PenumbraDisassembler.{h,cpp}` | Binary → MCInst. Custom decoders for branch targets (symbolic lookup), signed immediates (LLIS), signed memory offsets |
| `MCTargetDesc/PenumbraMCTargetDesc.{h,cpp}` | Registers all MC components. `PenumbraMCInstrAnalysis`: branch target evaluation + GPR state tracking for LLI/LUI address annotation |
| `MCTargetDesc/PenumbraInstPrinter.{h,cpp}` | MCInst → assembly text |
| `MCTargetDesc/PenumbraMCCodeEmitter.cpp` | MCInst → binary bytes. Custom `encodeBranchTarget` and `encodeImm16` create fixups |
| `MCTargetDesc/PenumbraAsmBackend.cpp` | Fixup resolution (branch22, imm16, lo16, hi16), `maybeAddReloc` for ELF relocs, NOP = `0x00000000` (ADD R0,R0) |
| `MCTargetDesc/PenumbraELFObjectWriter.cpp` | ELF relocation mapping. Uses `EM_PENUMBRA` from `llvm/BinaryFormat/ELF.h` |
| `MCTargetDesc/PenumbraFixupKinds.h` | Fixup kinds (branch22, imm16, memoffset16, lo16, hi16, pcrel variants) and MCSpecifierExpr values (S\_Lo16, S\_Hi16, S\_PCRel) |
| `MCTargetDesc/PenumbraMCAsmInfo.{h,cpp}` | ELF-based, little-endian, `//` comments, `;` statement separator. `printSpecifierExpr` for `%lo16()`/`%hi16()`/`%pcrel()`/`%tlsgd_lo16()`/`%tlsgd_hi16()` |
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
ELF linker target with PIE support.
Handles all 9 relocation types
(including PC-relative memoffset and imm16 for PIC).
**RELA format:** `EM_PENUMBRA` is in lld's RELA architecture list
(`Driver.cpp:getIsRela`), so dynamic relocations use explicit addends
(12-byte `Elf32_Rela` entries with `DT_RELA`/`DT_RELASZ`).
Self-relocating PIE code (bootloader, ld.elf_so) needs
`--apply-dynamic-relocs` so lld writes addends to the data sections
(the bias computation reads pre-relocation values before the
relocator runs).
PIE support: `relativeRel = R_PENUMBRA_RELATIVE`,
`symbolicRel = R_PENUMBRA_32`, `getDynRel()` maps
`R_PENUMBRA_32` to dynamic, `getImplicitAddend()` reads
32-bit values for verification.
**Negative pcrel fix:** `R_PENUMBRA_IMM16_PCREL` handler
detects negative offsets and flips ADDi (INC, op=0011) to
SUBi (DEC, op=0100) with negated value, since ADDi
zero-extends its immediate.  Same fix in `PenumbraAsmBackend`.
Registered via `EM_PENUMBRA` (0xF0DA) in `llvm/BinaryFormat/ELF.h`.
Emulation string `elf32penumbra`, output format `elf32-penumbra`.
Triple mapping for `Triple::penumbra` in `InputFiles.cpp`.
**Duplicate absolute symbol fix** (`llvm/lld/ELF/Symbols.cpp`):
upstream lld's GNU ld compatibility check for duplicate absolute
symbols accidentally excluded value 0 (C++ truthiness).
Fixed locally — needed for NetBSD kernel option tracking symbols
(`_KERNEL_OPT_N*`) which have value 0 for unconfigured devices.

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
| `R_PENUMBRA_RELATIVE` | 8 | PIE dynamic relocation (bias adjust) | Full word |
| `R_PENUMBRA_TLS_GD_LO16` | 9 | TLS GD: low 16 bits (static: TP offset) | bits [15:0] |
| `R_PENUMBRA_TLS_GD_HI16` | 10 | TLS GD: high 16 bits (static: TP offset) | bits [15:0] |
| `R_PENUMBRA_GLOB_DAT` | 11 | GOT entry (absolute address) | Full word |
| `R_PENUMBRA_JUMP_SLOT` | 12 | PLT GOT entry | Full word |
| `R_PENUMBRA_TLS_TPOFF32` | 13 | TLS IE: TP-relative offset in GOT | Full word |
| `R_PENUMBRA_TLS_DTPMOD32` | 14 | TLS GD: module index in GOT | Full word |
| `R_PENUMBRA_TLS_DTPOFF32` | 15 | TLS GD: module offset in GOT | Full word |
| `R_PENUMBRA_TLS_GD_PCREL` | 16 | TLS GD: PC-relative to GOT entry (PIC) | bits [15:0] |
| `R_PENUMBRA_PC32` | 17 | PC-relative 32-bit (.eh_frame FDE pointers) | Full word |
| `R_PENUMBRA_GOT_PCREL_LO16` | 18 | GOT PC-relative: low 16 bits | bits [15:0] |
| `R_PENUMBRA_GOT_PCREL_HI16` | 19 | GOT PC-relative: high 16 bits | bits [15:0] |
| `R_PENUMBRA_TLS_GD_GOT_PCREL_LO16` | 20 | TLS GD GOT PC-relative: low 16 | bits [15:0] |
| `R_PENUMBRA_TLS_GD_GOT_PCREL_HI16` | 21 | TLS GD GOT PC-relative: high 16 | bits [15:0] |

## Legalization (`GISel/PenumbraLegalizerInfo.{h,cpp}`)
- **Legal s32:** G_ADD, G_SUB, G_AND, G_OR, G_XOR,
  G_SHL/G_LSHR/G_ASHR (both operands clamped to s32),
  G_LOAD/G_STORE (s32/s16/s8),
  G_CONSTANT (s32/p0), G_FRAME_INDEX/G_GLOBAL_VALUE (p0),
  G_PTR_ADD/G_PTRMASK {p0,s32},
  G_INTTOPTR/G_PTRTOINT {p0,s32} (sub-word widened to s32),
  G_ICMP {s1,s32}/{s1,p0}, G_SELECT {s32/p0,s1},
  G_PHI, G_BRCOND, G_FREEZE (no-op).
- **Extensions:** G_ZEXT/G_SEXT/G_ANYEXT sub-word→s32 legal,
  s32→s64 via narrowScalarIf. G_TRUNC legal.
  G_SEXT_INREG lowered.
- **MUL/DIV/REM:** Custom s32 (strength-reduce power-of-2 constants
  to shifts/logic, libcall fallback).
  G_SDIV/G_SREM libcall s32+s64. s64 all via libcalls.
- **Lowered:** G_ABS, G_CTTZ/G_CTLZ/G_CTPOP
  (and \_ZERO\_UNDEF variants) to shift/logic,
  G_FSHL/G_FSHR (s32+s64),
  G_BSWAP/G_BITREVERSE (sub-word widened to s32, then lowered for s32+s64),
  G_UADDO/G_USUBO/G_UADDE/G_USUBE/G_SADDO/G_SSUBO/G_SADDE/G_SSUBE
  (s64 narrowed to s32),
  G_SMIN/G_SMAX/G_UMIN/G_UMAX (any width, lowered to icmp+select),
  G_FNEG/G_FABS/G_FCOPYSIGN (integer bit manipulation, no libcall),
  G_IS_FPCLASS (exponent/mantissa bit inspection).
- **Libcall:** G_MEMCPY/G_MEMMOVE/G_MEMSET.
- **Legal:** G_STACKSAVE/G_STACKRESTORE (p0),
  G_FENCE (always legal — compiler barrier only, no hardware instruction),
  G_BRINDIRECT (p0 — computed goto).
- **Lowered:** G_DYN_STACKALLOC (framework: SP subtract + alignment).
- **Custom:** G_VASTART, G_MUL, G_UDIV, G_UREM, G_PREFETCH (no-op),
  G_GET_ROUNDING (constant 1 = round-to-nearest, no FPU)
  (via legalizeCustom() override). G_VAARG lowered (s32/s64/p0).

## Key Implementation Notes
- **Range-checked encoders.** Both the MC code emitter
  (`encodeImm16`/`encodeMemOffset16`/`encodeBranchTarget`) and the
  asm backend `applyFixup` hard-error on out-of-range immediates
  rather than silently truncating.  Emitter uses
  `Ctx.reportError(Inst.getLoc(), ...)` (llc/clang exit non-zero
  without writing an object); backend uses
  `report_fatal_error` for resolved fixups.  `imm16_pcrel` accepts
  the symmetric `[-65535, 65535]` range because ADDi↔SUBi flip
  lets either sign reach the full uimm16.
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
- **Global address materialization (PIC/PIE):**
  GOT-indirect with full 32-bit reach:
  `MOV Rd, PC` + `LLI Rt, %got_pcrel_lo16(sym+4)` +
  `LUI Rt, %got_pcrel_hi16(sym+8)` + `ADD Rd, Rt` +
  `LDW Rd, [Rd]`.
  Addends +4/+8 compensate for MOV-to-LLI/LUI distance.
  LLI+LUI reconstruct the 32-bit GOT-to-PC offset (unsigned
  lo16 | hi16<<16, no sign-extension issues).
  Works for both PIE (GOT entries get R_RELATIVE) and shared
  libraries (GOT entries get R_GLOB_DAT).
  `needsRelocateWithSymbol()` returns true for GOT/TLS relocs
  to prevent section+offset folding (the addend must only
  contain the PC adjustment, not the symbol's section offset).
  **TLS GD PIC:** Same pattern but 4 instructions (no LDW) —
  the GOT tls_index pair ADDRESS is the argument to
  `__tls_get_addr`, not its contents.
- **Jump tables:** Always `EK_LabelDifference32` entries
  (`.word target - JT_base`), regardless of PIC/static mode.
  Placed inline in `.text` via `PenumbraTargetObjectFile`
  (overrides `shouldPutJumpTableInFunctionSection`) so the
  assembler can resolve the label difference within one
  section — avoids cross-section relocations that would
  become absolute + RELATIVE in PIE, breaking the
  base-addition scheme.
  BRJT expansion always adds base back:
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
