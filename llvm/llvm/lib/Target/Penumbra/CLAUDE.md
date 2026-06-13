# Penumbra LLVM Backend — Claude Code Context

Navigation aid for work under `llvm/`. The root `CLAUDE.md` has
project-wide conventions; `doc/system/abi.md` is the authoritative
spec for calling convention, ELF format, and relocation table.

**Read first:**
- `doc/system/abi.md` — full ABI: registers, calling convention,
  ELF object format, the complete `R_PENUMBRA_*` relocation table
  (§4), DWARF mapping, TLS model.
- `doc/system/architecture.md`, `doc/system/instruction-encoding.md`
  — ISA-visible behavior and bit-level instruction encoding.
- `doc/system/toolchain.md` — LLVM backend strategy and rationale.

This file documents what's specific to the *implementation* (where
code lives, how passes are wired, non-obvious gotchas). Do not look
here for spec facts — those live in `doc/`.

## Build

Source: `llvm/llvm/`. Build: `build/llvm/` (override with
`LLVM_PREFIX`).

Initial cmake (one-time):
```sh
cmake -G Ninja -S llvm/llvm -B build/llvm \
  -DLLVM_TARGETS_TO_BUILD=Penumbra \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  -DLLVM_INCLUDE_TESTS=ON -DLLVM_BUILD_TESTS=ON \
  -DLLVM_PARALLEL_LINK_JOBS=2
```

Incremental rebuild — target only what's needed (a bare `ninja -C
build/llvm` also builds upstream unit tests):
```sh
ninja -C build/llvm -j10 llc clang lld \
  llvm-mc llvm-ar llvm-nm llvm-objcopy llvm-objdump \
  llvm-readobj llvm-size llvm-strings
```
`-j10` for compilation, `-j2` link jobs (set in cmake) to keep
15 GB WSL2 from OOM-ing on the linker steps.

Codegen regression tests (Lit + FileCheck):
```sh
build/llvm/bin/llvm-lit llvm/llvm/test/CodeGen/Penumbra/       # all
build/llvm/bin/llvm-lit -v llvm/llvm/test/CodeGen/Penumbra/alu.ll  # one
```

Regenerate CHECK lines after a codegen change:
```sh
python3 llvm/llvm/utils/update_llc_test_checks.py \
  --llc-binary build/llvm/bin/llc \
  llvm/llvm/test/CodeGen/Penumbra/<test>.ll
```
Penumbra is registered in `utils/UpdateTestChecks/asm.py` (reuses
AVR's scrubber/function-RE).

Compiler-correctness suite (thousands of C tests on ISS in `+hosted`
mode; requires `compiler-rt` — build via
`sw/tools/setup-compiler-rt.sh`):
```sh
make test-compiler                # all tests at -O2
make test-compiler OPT="-Os"      # override optimization
make test-compiler COMPILER_TESTS="path/to/test.c"
```

The default suite is bare-metal non-PIC, so it never exercises
GOT-indirect codegen.  `make test-compiler-pic` runs a separate
curated set (`test/compiler/penumbra-pic/`) compiled `-fPIC` and linked
static at a fixed address (lld resolves the GOT at link time — no
runtime relocator), covering the GOT global / jump-table / block-address
materialization paths.  Disjoint from the main suite.

## File map (`llvm/llvm/lib/Target/Penumbra/`)

This table is the primary navigation aid for finding code. For *what
each pass does*, read the source — descriptions here state purpose,
not behavior.

| File | Purpose |
|------|---------|
| `Penumbra.td` | Top-level TableGen: includes, ProcessorModel, AsmWriter, Target, pointer remap |
| `PenumbraRegisterInfo.td` | 16 GPRs (R0=zero, R12=TP, R13=LR, R14=SP, R15=PC), GPR/GPR_Allocatable/CCR classes, HWEncoding |
| `PenumbraInstrInfo.td` | All 4 formats (R/L/M/B) with bit-accurate encoding. Tied-operand constraints for 2-addr ops. ADC/SBC `Uses=[SR]`. All operand slots use full `GPR` (allocator honors R0's reserved+`isConstant`). Pseudos: RET, LEAfi, SELECT_GPR, SELECT_CC_GPR, ADJCALLSTACK. `Penumbra1Model`: IssueWidth=1, MicroOpBufferSize=0, LoadLatency=1 |
| `PenumbraGISel.td` | TableGen `Pat<>` rules (simple 1:1 selections). Includes `PenumbraCombine.td`. ImmLeaf predicates: uimm16, simm16, simm16neg, uimm5 |
| `PenumbraCombine.td` | GlobalISel combiner rule groups (pre-/post-/-O0 lists) and custom rule decls. Custom matchers live in `PenumbraPostLegalizerCombiner.cpp` |
| `PenumbraCallingConv.td` | CC_Penumbra (R1–R4 args, stack overflow), RetCC_Penumbra (R1, R2 for i64), CSR_Penumbra (R5–R10, R13) |
| `PenumbraRegisterInfo.{h,cpp}` | Reserved regs (R0/R12/R14/R15), callee-saved list, `getFrameRegister(R14)`. `eliminateFrameIndex` folds small offsets directly; large offsets expand to LLI+LUI+ADD via a fresh virtual register (RegScavenger picks the physical reg) |
| `PenumbraFrameLowering.{h,cpp}` | StackGrowsDown, Align(4), `hasFPImpl()=true` when alloca present. `adjustSP` helper used by both prologue and epilogue (small frames use 16-bit imm, large frames materialize size in R11). `processFunctionBeforeFrameFinalized` adds an emergency spill slot for RegScavenger only when frame > 15-bit signed |
| `PenumbraISelLowering.{h,cpp}` | TargetLowering: JT encoding (EK_LabelDifference32), SELECT diamond expansion, inline-asm constraint mapping, `setStackPointerRegisterToSaveRestore(R14)`, `isIntDivCheap()=true`, `isLegalAddressingMode` override |
| `PenumbraTargetTransformInfo.h` | Header-only `PenumbraTTIImpl`. Routes LSR/cost-model queries to our `TargetLowering`. `getNumberOfRegisters` returns 12 for GPRRC, 0 for FPR/VR (disables vectorizers). `isLSRCostLess` makes `Insns` the primary sort key (PowerPC pattern) — without this, LSR rewrites `*d++ = *s++` loops into base+index form |
| `PenumbraSubtarget.{h,cpp}` | Owns InstrInfo, FrameLowering, TLInfo, all GlobalISel objects |
| `PenumbraTargetMachine.{h,cpp}` | Data layout `e-m:e-p:32:32-i32:32-i64:64-n32-S32`, GlobalISel pipeline, `setGlobalISel(true)`. `PenumbraTargetObjectFile` (local class): always inlines jump tables in `.text`. `PenumbraLowerTLS` IR pass: lowers `@llvm.threadlocal.address` (GD → `__tls_get_addr`; LE/IE → inline TP+offset) |
| `PenumbraAsmPrinter.cpp` | MachineInstr → MCInst. Expands RET→JMP R13. Wraps globals/JTI with lo16/hi16/pcrel MCSpecifierExpr. `emitJumpTableEntry`: always label-difference. PrintAsmOperand for inline asm |
| `PenumbraMachineFunctionInfo.h` | Per-function state (e.g. `VarArgsFrameIndex` for variadic R1–R4 save area) |
| `GISel/PenumbraCallLowering.{h,cpp}` | lowerFormalArguments (R1–R4 → vregs + variadic save), lowerReturn, lowerCall |
| `GISel/PenumbraLegalizerInfo.{h,cpp}` | Type/op legality. See "Legalization at a glance" below |
| `GISel/PenumbraRegisterBankInfo.{h,cpp}` | Single GPR bank covering all 16 registers |
| `GISel/PenumbraRegisterBanks.td` | `GPRRegBank` definition |
| `GISel/PenumbraInstructionSelector.cpp` | Hybrid: `selectImpl()` for TableGen patterns, manual C++ for complex cases (LLI+LUI pairs, GOT-PCREL globals, frame-index folding, jump tables, ICMP+BRCOND fold, varargs, return-address capture, …) |
| `GISel/PenumbraPreLegalizerCombiner.cpp` | Pre-legalizer combiner (-O1+). Boilerplate wrapper around `selectImpl` for rules in `PenumbraCombine.td` |
| `GISel/PenumbraO0PreLegalizerCombiner.cpp` | Same shape with the smaller `optnone_combines` rule set (-O0 only) |
| `GISel/PenumbraPostLegalizerCombiner.cpp` | Post-legalizer combiner (-O1+). C++ match/apply for target-specific rules (currently `matchNegImmToOpposite`/`applyNegImmToOpposite`) |
| `Disassembler/PenumbraDisassembler.{h,cpp}` | Binary → MCInst. Custom decoders for branch targets (symbolic lookup), signed immediates (LLIS), signed mem offsets |
| `MCTargetDesc/PenumbraMCTargetDesc.{h,cpp}` | Registers all MC components. `PenumbraMCInstrAnalysis`: branch-target evaluation + GPR state tracking for LLI/LUI address annotations |
| `MCTargetDesc/PenumbraInstPrinter.{h,cpp}` | MCInst → assembly text |
| `MCTargetDesc/PenumbraMCCodeEmitter.cpp` | MCInst → binary. Custom `encodeBranchTarget`/`encodeImm16` create fixups |
| `MCTargetDesc/PenumbraAsmBackend.cpp` | Fixup resolution (branch22, imm16, lo16, hi16). `maybeAddReloc` for ELF relocs. NOP = `0x00000000` (ADD R0,R0) |
| `MCTargetDesc/PenumbraELFObjectWriter.cpp` | ELF reloc mapping. Uses `EM_PENUMBRA` from `llvm/BinaryFormat/ELF.h` |
| `MCTargetDesc/PenumbraFixupKinds.h` | Fixup kinds + MCSpecifierExpr values (S_Lo16, S_Hi16, S_PCRel) |
| `MCTargetDesc/PenumbraMCAsmInfo.{h,cpp}` | ELF, little-endian, `//` comments, `;` statement separator. `printSpecifierExpr` for `%lo16()`/`%hi16()`/`%pcrel()`/`%tlsgd_*()` |
| `AsmParser/PenumbraAsmParser.cpp` | Assembly text → MCInst. Pseudo expansion (LI/LA/NOP/RET), register/immediate/memory parsing |
| `TargetInfo/PenumbraTargetInfo.{h,cpp}` | Target registration (`Triple::penumbra`) |

## Triple integration (across the tree)

`penumbra` is registered in upstream LLVM:
- `Triple.h` (arch enum), `Triple.cpp` (name, prefix, parsing,
  32-bit, little-endian, ELF, DwarfCFI).
- `llvm/llvm/CMakeLists.txt` `LLVM_ALL_TARGETS`.
- `ELFObjectFile.h` maps `EM_PENUMBRA` → `elf32-penumbra` /
  `Triple::penumbra` for binary utilities.
- `EM_PENUMBRA` (0xF0DA) defined centrally in
  `llvm/BinaryFormat/ELF.h`.
- `utils/UpdateTestChecks/asm.py` (reuses AVR scrubber).

`TargetDataLayout.cpp:computeDataLayout()` has a `-Wswitch` warning
for unhandled `penumbra` — harmless (we provide our own data layout
via `PenumbraTargetMachine`).

## Combiner pipeline (where the codegen tricks live)

The pipeline is defined by group lists in `PenumbraCombine.td`:

- **Pre-legalizer at -O1+:** `[all_combines]` — the full upstream
  canonicalization/DCE set, matching AArch64/RISC-V. The
  `*_by_const` magic-multiply rules (in `intdiv_combines` /
  `intrem_combines`) are gated off by `isIntDivCheap()=true`
  in `PenumbraISelLowering.cpp` — without that override
  `udiv x, 3` rewrites into a 64-bit `__muldi3` libcall heavier
  than the original `__udivsi3`. `sub_to_add` flips
  (`G_SUB x, c` → `G_ADD x, -c`) are re-canonicalized after
  legalization by `penumbra_neg_imm_to_opposite` when `|-c|` fits
  uimm16.
- **Pre-legalizer at -O0:** `[optnone_combines]` — small upstream
  set (`trivial_combines`, `ptr_add_immed_chain`,
  `combines_for_extload`, `not_cmp_fold`,
  `opt_brcond_by_inverting_cond`, `combine_concat_vector`).
  Cleans IRTranslator artefacts without paying full canonicalization
  cost. Lives in `PenumbraO0PreLegalizerCombiner`.
- **Post-legalizer at -O1+:** `[commute_constant_to_rhs,
  ptr_add_immed_chain, combines_for_extload, penumbra_zextload_promote,
  known_bits_simplifications, penumbra_neg_imm_to_opposite,
  penumbra_sink_ptr_add_past_use]`.

The three target-specific custom rules
(`penumbra_zextload_promote`, `penumbra_neg_imm_to_opposite`,
`penumbra_sink_ptr_add_past_use`) each have rationale comments in
`PenumbraPostLegalizerCombiner.cpp` and a regression test under
`test/CodeGen/Penumbra/`. MIR-level unit tests use
`-run-pass=penumbra-postlegalizer-combiner` to isolate individual
rules (regenerate via `update_mir_test_checks.py`).

## Legalization at a glance

Full definitions live in `GISel/PenumbraLegalizerInfo.cpp`. Notable
shapes:

- Legal s32 for the usual integer ops (ADD/SUB/AND/OR/XOR, shifts,
  loads/stores, ICMP, SELECT, PHI, BRCOND, FREEZE, …).
- Extensions: G_ZEXT/G_SEXT/G_ANYEXT sub-word→s32 legal; s32→s64 via
  `narrowScalarIf`. G_TRUNC legal. G_SEXT_INREG lowered.
- MUL/DIV/REM: s32 strength-reduces power-of-2 constants
  (MUL by -1→SUB from zero, ×2ⁿ→SHL, ×(2ⁿ±1)→SHL+ADD/SUB with
  shift<bit-width guard, ÷2ⁿ→LSHR, %2ⁿ→AND); otherwise libcall.
  s64 always libcall. G_SDIVREM/G_UDIVREM lower to separate
  div+rem then take the libcall path.
- Min/max/abs/popcount/ctlz/cttz/bswap/bitreverse: lowered to
  shift/logic. G_BSWAP at s64 narrows to two s32 first, then
  lowers — see `doc/llvm-lowerBswap-bug.md` for why we don't lower
  directly at s64.
- Add/sub-with-carry: G_UADDO/G_UADDE/G_USUBO/G_USUBE legal at
  {s32,s1}; s64 add/sub narrows into them and the selector
  (`selectAddSubCarry`) maps them onto the hardware ADD/ADC,
  SUB/SBC chain, threading the carry through SR.C (no CCR bank —
  the flag is physical, carried by the ops' implicit SR operands).
- Saturating arithmetic: sub-word widened to s32; s64 lowered at
  native width via min/sub or USUBO+SELECT, then iteratively
  narrowed. (`lowerFor({s32, s64})` is the working idiom because
  `narrowScalarIf+changeTo` only relabels the type.)
- G_MEMCPY/MEMMOVE/MEMSET via libcalls. G_PREFETCH custom-lowered
  to no-op. G_FENCE always legal (compiler barrier; no hardware
  instruction).
- G_DYN_STACKALLOC lowered by framework. G_STACKSAVE/RESTORE
  selected to MOV SP. G_BRINDIRECT legal for p0 (computed goto).
- Custom: G_VASTART, G_MUL/UDIV/UREM, G_PREFETCH, G_GET_ROUNDING
  (returns constant 1).

## Key implementation notes (gotchas for editors of this code)

- **Range-checked encoders.** Both `PenumbraMCCodeEmitter`
  (`encodeImm16`/`encodeMemOffset16`/`encodeBranchTarget`) and
  `PenumbraAsmBackend::applyFixup` hard-error on out-of-range
  immediates rather than silently truncating. Emitter uses
  `Ctx.reportError(...)` (llc/clang exit non-zero); backend uses
  `report_fatal_error`. `imm16_pcrel` accepts symmetric
  `[-65535, 65535]` because ADDi↔SUBi flip lets either sign reach
  the full uimm16.
- **applyFixup Data pointer is pre-positioned at the fixup
  location** — do *not* add `Fixup.getOffset()`. Use `Data[i]`
  directly.
- **`maybeAddReloc` must be called at the start of `applyFixup()`**
  to generate ELF relocations for unresolved symbols. Without it,
  symbol references silently resolve to zero.
- **PC-relativity** is set on the `MCFixup` itself (`PCRel=true` in
  `MCFixup::create`), not in `MCFixupKindInfo`.
- **LLI+LUI pairs** use an intermediate vreg
  (`%tmp = LLI lo` → `%dst = LUI %tmp, hi`) for SSA correctness —
  required for `-O1+` passes like `OptimizePHIs`.
- **GOT-indirect PIC** uses the ADD's own PC as anchor:
  `LLI Rd, %got_pcrel_lo16(sym-.LPC0_0)` +
  `LUI Rd, %got_pcrel_hi16(sym-.LPC0_0)` + `.LPC0_0: ADD Rd, PC` +
  `LDW Rd, [Rd]`.  The sequence is selected as PICLLI/PICLUI/PICADDPC
  pseudos sharing a per-function pclabel id
  (`PenumbraMachineFunctionInfo::createPICLabelUId`); the AsmPrinter
  emits the `.LPC` label at the anchor and label-difference operands on
  the carriers, so relocation addends stay correct under any code
  motion — tail merging once fused the anchor ADDs of different switch
  cases and shifted two of three GOT reads onto wrong slots
  (`pic-anchor-tail-merge.ll`).  Anchor pseudos are `isNotDuplicable`
  (a clone would redefine the label).  LLI+LUI reconstruct the unsigned
  32-bit GOT-to-PC offset, no sign issues.
  `needsRelocateWithSymbol()` returns true for GOT/TLS relocs so the
  addend only carries the PC adjustment, not the symbol's section
  offset.  GOT fixups are *not* PCRel at the MC layer: their relocation
  types carry the `-P`, and the backend's `evaluateFixup` forces a
  relocation so same-section locals still get GOT slots.
- **TLS GD PIC** uses the same anchored pseudo shape but no LDW — the
  GOT tls_index pair *address* is the argument to `__tls_get_addr`,
  not its contents: PICLLI/PICLUI (`%tlsgd_got_pcrel_*`) + PICADDPC +
  `BL __tls_get_addr`.
- **Jump tables** are always `EK_LabelDifference32`
  (`.word target - JT_base`) placed inline in `.text` via
  `PenumbraTargetObjectFile::shouldPutJumpTableInFunctionSection`.
  Cross-section JT entries would become absolute+RELATIVE in PIE,
  breaking the base-addition scheme. BRJT expansion adds base back:
  `LDW offset,[entry_addr]` → `ADD offset, base` → `JMP`. JTI
  operands get +4 addend in AsmPrinter.
- **Register class constraining.** Every instruction-selector helper
  must call `constrainSelectedInstRegOperands()` — vregs left with
  only a bank assignment (no regclass) assert after selection.
- **Destructive 2-operand ops.** TableGen patterns use tied-operand
  constraints; RegisterAllocator handles via COPY.

## lld (`llvm/lld/ELF/Arch/Penumbra.cpp`)

ELF linker target with PIE support. Handles every Penumbra
relocation (full table in `doc/system/abi.md` §4).

- **RELA format.** `EM_PENUMBRA` is in lld's RELA architecture list
  (`Driver.cpp:getIsRela`), so dynamic relocations use explicit
  addends (12-byte `Elf32_Rela` entries with `DT_RELA`/`DT_RELASZ`).
  Self-relocating PIE code (bootloader, `ld.elf_so`) needs
  `--apply-dynamic-relocs` so lld writes addends to the data
  sections (the bias computation reads pre-relocation values).
- **PIE.** `relativeRel = R_PENUMBRA_RELATIVE`,
  `symbolicRel = R_PENUMBRA_32`. `getDynRel()` maps `R_PENUMBRA_32`
  to dynamic; `getImplicitAddend()` reads 32-bit values for
  verification.
- **GOT/PLT.** PLT entries are 16 bytes (LLI+LUI+LDW+JMP using R11
  scratch). `R_PENUMBRA_GLOB_DAT` for GOT,
  `R_PENUMBRA_JUMP_SLOT` for PLT. Shared libraries (`-shared`) work
  with PIC code.
- **TLS GD (PIC).** GOT-indirect PC-relative to tls_index pair via
  `R_PENUMBRA_TLS_GD_GOT_PCREL_LO16/HI16`. lld creates GOT pairs
  with `R_PENUMBRA_TLS_DTPMOD32` / `R_PENUMBRA_TLS_DTPOFF32`
  dynamic relocs. Static uses `R_TPREL` (Variant I, no TCB gap,
  like RISC-V). `EM_PENUMBRA` → `getTlsTpOffset` mapping in
  `InputSection.cpp`.
- **Negative pcrel fix.** `R_PENUMBRA_IMM16_PCREL` handler detects
  negative offsets and flips ADDi (INC, op=0011) to SUBi (DEC,
  op=0100) with negated value, since ADDi zero-extends its
  immediate. Same fix in `PenumbraAsmBackend`.
- **Duplicate absolute symbol fix** (`llvm/lld/ELF/Symbols.cpp`):
  upstream's GNU ld compatibility check for duplicate absolute
  symbols excluded value 0 (C++ truthiness bug). Fixed locally —
  needed for NetBSD kernel option-tracking symbols
  (`_KERNEL_OPT_N*`) which have value 0 for unconfigured devices.
- Emulation: `elf32penumbra`. Output format: `elf32-penumbra`.
  Triple mapping for `Triple::penumbra` in `InputFiles.cpp`.

## Clang driver

- `PenumbraToolChain` (`clang/lib/Driver/ToolChains/Penumbra.{h,cpp}`)
  is the **bare-metal** toolchain (ROM, hw tests) — uses `ld.lld`
  directly.
- For `penumbra-unknown-netbsd`, the stock `toolchains::NetBSD`
  (`clang/lib/Driver/ToolChains/NetBSD.cpp`) is used with Penumbra
  additions: `elf32penumbra` emulation, `useLibgcc=false`,
  `--allow-shlib-undefined`, `--undefined-version`.
- `__NetBSD__` is auto-defined for the netbsd triple via
  `NetBSDTargetInfo<>` wrapper.
- `-fPIC`/`-fPIE` supported. Boot ROM compiles and runs at
  `-O0`/`-O1`/`-O2`. `build.sh distribution` compiles full NetBSD
  userland.

## Hex output pipeline (host build → simulator)

`llvm-mc` → ELF object → `llvm-objcopy -O binary` → `bin2hex.py` →
`$readmemh` hex. Same approach as ARM/RISC-V embedded.
`bin2hex.py` (`sw/tools/bin2hex.py`) reads flat LE binary, emits one
32-bit word per line in uppercase hex.
