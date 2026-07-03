//===-- PenumbraISelLowering.cpp - Penumbra TargetLowering hooks -----------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraISelLowering.h"
#include "PenumbraInstrInfo.h"
#include "PenumbraRegisterInfo.h"
#include "PenumbraSubtarget.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

// TableGen-generated CC assignment functions (CC_Penumbra, RetCC_Penumbra).
// Must be included after `using namespace llvm` — the generated code uses
// MVT, CCValAssign, CCState etc. without namespace qualification.
#include "PenumbraGenCallingConv.inc"

PenumbraISelLowering::PenumbraISelLowering(const TargetMachine &TM,
                                            const PenumbraSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &Penumbra::GPR_AllocatableRegClass);

  // SP register — needed by G_DYN_STACKALLOC lowering and G_STACKSAVE/RESTORE.
  setStackPointerRegisterToSaveRestore(Penumbra::R14);

  // The ISD operation-action tables are deliberately left at their
  // defaults.  Penumbra selects exclusively through GlobalISel —
  // instruction legality lives in PenumbraLegalizerInfo, and no
  // SelectionDAG selector exists to consume the tables.  Their only
  // remaining readers are IR-level heuristics (BasicTTI cost queries,
  // CodeGenPrepare, DivRemPairs), which see the default answer "Legal":
  // true for the i32 integer core including the divmul unit's fused
  // divrem and high-half multiply, optimistic for the bit-manipulation
  // ops GISel lowers to shift/logic sequences (BSWAP, ROTL/ROTR,
  // CTLZ/CTTZ/CTPOP) — an error that can only skew cost heuristics,
  // never correctness.  Capability or cost corrections for those passes
  // belong in PenumbraTTIImpl, beside getNumberOfRegisters and
  // isLSRCostLess.
  //
  // Atomics likewise ride the base default (MaxAtomicSizeInBitsSupported
  // = 0): AtomicExpand rewrites every atomic to an __atomic_* libcall,
  // and the library provides the synchronization (interrupt-disable CAS
  // now, RAS or LL/SC in future).

  computeRegisterProperties(STI.getRegisterInfo());
}

unsigned PenumbraISelLowering::getJumpTableEncoding() const {
  // Always use label-difference entries (target - JT_base).  This avoids
  // dynamic relocations, works at any load address (static, PIC, PIE),
  // and enables future optimization to 16-bit entries when offsets fit
  // in ±32KB.  The instruction selector adds the JT base back at runtime.
  return MachineJumpTableInfo::EK_LabelDifference32;
}

CCAssignFn *PenumbraISelLowering::getCCAssignFn(CallingConv::ID CC,
                                                 bool Return,
                                                 bool IsVarArg) const {
  return Return ? RetCC_Penumbra : CC_Penumbra;
}

//===----------------------------------------------------------------------===//
// Exception handling
//===----------------------------------------------------------------------===//

Register PenumbraISelLowering::getExceptionPointerRegister(
    const Constant *PersonalityFn) const {
  return Penumbra::R1;
}

Register PenumbraISelLowering::getExceptionSelectorRegister(
    const Constant *PersonalityFn) const {
  return Penumbra::R2;
}

//===----------------------------------------------------------------------===//
// Inline assembly support
//===----------------------------------------------------------------------===//

TargetLowering::ConstraintType
PenumbraISelLowering::getConstraintType(StringRef Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':
      return C_RegisterClass;
    default:
      break;
    }
  }
  return TargetLowering::getConstraintType(Constraint);
}

std::pair<unsigned, const TargetRegisterClass *>
PenumbraISelLowering::getRegForInlineAsmConstraint(
    const TargetRegisterInfo *TRI, StringRef Constraint, MVT VT) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':
      if (VT == MVT::i32 || VT == MVT::Other)
        return {0U, &Penumbra::GPR_AllocatableRegClass};
      break;
    }
  }

  // Physical register name in braces: {r1}, {sp}, {cc}, etc.
  if (StringRef(Constraint).starts_with("{") &&
      StringRef(Constraint).ends_with("}")) {
    StringRef RegName = Constraint.substr(1, Constraint.size() - 2);
    // Map "cc" to the status register (condition code clobber).
    if (RegName == "cc")
      return {Penumbra::SR, &Penumbra::CCRRegClass};
  }

  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

//===----------------------------------------------------------------------===//
// Named global register variables
//===----------------------------------------------------------------------===//

// Pull in the TableGen-generated register matchers: MatchRegisterName
// (canonical r0–r15, sr) and MatchRegisterAltName (the ABI aliases, emitted
// because PenumbraAsmParser sets ShouldEmitMatchRegisterAltName).
#define GET_REGISTER_MATCHER
#include "PenumbraGenAsmMatcher.inc"

// Resolve a `register T x __asm("name")` global register variable to its
// physical register.  The kernel pins curlwp in R12 (the thread pointer)
// this way, mirroring the RISC-V port's `register struct lwp *foo __asm("tp")`.
Register
PenumbraISelLowering::getRegisterByName(const char *RegName, LLT /*Ty*/,
                                        const MachineFunction &MF) const {
  // Accept the canonical rN spelling and the ABI aliases (the register
  // AltNames in PenumbraRegisterInfo.td).  Both matchers are TableGen-
  // generated and shared with the assembler, so the canonical name and its
  // alias spelling pin the same register.
  Register Reg = MatchRegisterName(RegName);
  if (!Reg)
    Reg = MatchRegisterAltName(RegName);
  if (!Reg)
    reportFatalUsageError(Twine("invalid register name \"") + RegName + "\"");

  // A named global register must land on a register the allocator never
  // touches; otherwise codegen would reuse it and silently clobber the pinned
  // value.  (This class keeps no Subtarget member — reach the register info
  // through the MachineFunction.)
  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
  if (!TRI->getReservedRegs(MF).test(Reg))
    reportFatalUsageError(Twine("named global register \"") + RegName +
                          "\" is allocatable; only reserved registers may be "
                          "pinned");

  return Reg;
}

// Returning true makes the GISel combiner leave divide-by-constant as a
// divide instead of applying the `udiv_by_const`/`sdiv_by_const`
// magic-multiply rewrite.  On the divmul unit the rewrite's high-half
// multiply (G_UMULH/G_SMULH) costs the same iteration latency as the DIV
// it replaces, so the rewrite saves no time and adds shift/fixup
// instructions — pure I-footprint loss on a machine whose hot loops are
// footprint-bound.
bool PenumbraISelLowering::isIntDivCheap(EVT VT, AttributeList Attr) const {
  return true;
}

// Expand SELECT_GPR / SELECT_CC_GPR pseudo into a conditional-branch diamond.
//
// After expansion (three blocks):
//   thisMBB:
//     ... preceding instructions ...
//     TEST/CMP (set flags)
//     Bcc tailMBB           ; condition true → trueval wins
//   falseMBB:               ; fall-through (condition false)
//     B tailMBB
//   tailMBB:
//     %dst = PHI(%trueval, thisMBB, %falseval, falseMBB)
//     ... following instructions ...
MachineBasicBlock *
PenumbraISelLowering::EmitInstrWithCustomInserter(MachineInstr &MI,
                                                   MachineBasicBlock *MBB) const {
  unsigned Opc = MI.getOpcode();
  assert((Opc == Penumbra::SELECT_GPR || Opc == Penumbra::SELECT_CC_GPR ||
          Opc == Penumbra::SELECT_CCi_GPR) &&
         "Unexpected custom inserter instruction");

  MachineFunction *MF = MBB->getParent();
  const auto &TII = *MF->getSubtarget().getInstrInfo();
  const auto *BB = MBB->getBasicBlock();
  const auto DL = MI.getDebugLoc();

  auto I = ++MBB->getIterator();

  auto *FalseMBB = MF->CreateMachineBasicBlock(BB);
  auto *TailMBB = MF->CreateMachineBasicBlock(BB);
  MF->insert(I, FalseMBB);
  MF->insert(I, TailMBB);

  FalseMBB->setCallFrameSize(MBB->getCallFrameSize());
  TailMBB->setCallFrameSize(MBB->getCallFrameSize());

  // Move tail instructions and transfer original successors BEFORE adding
  // new CFG edges (transferSuccessors moves ALL successors from MBB).
  TailMBB->splice(TailMBB->end(), MBB, std::next(MI.getIterator()), MBB->end());
  TailMBB->transferSuccessorsAndUpdatePHIs(MBB);

  MBB->addSuccessor(FalseMBB);
  MBB->addSuccessor(TailMBB);
  FalseMBB->addSuccessor(TailMBB);

  // PHI in tailMBB picks trueval or falseval depending on which path was taken.
  BuildMI(*TailMBB, TailMBB->begin(), DL, TII.get(Penumbra::PHI),
          MI.getOperand(0).getReg())
      .addReg(MI.getOperand(1).getReg())
      .addMBB(MBB)
      .addReg(MI.getOperand(2).getReg())
      .addMBB(FalseMBB);

  // Emit the flag-setting instruction + conditional branch in thisMBB.
  if (Opc == Penumbra::SELECT_CC_GPR || Opc == Penumbra::SELECT_CCi_GPR) {
    // SELECT_CC_GPR:  operands are dst, trueval, falseval, lhs, rhs, cc.
    // SELECT_CCi_GPR: operands are dst, trueval, falseval, lhs, imm, cc.
    if (Opc == Penumbra::SELECT_CC_GPR)
      BuildMI(MBB, DL, TII.get(Penumbra::CMP))
          .addReg(MI.getOperand(3).getReg())
          .addReg(MI.getOperand(4).getReg());
    else
      BuildMI(MBB, DL, TII.get(Penumbra::CMPi))
          .addReg(MI.getOperand(3).getReg())
          .addImm(MI.getOperand(4).getImm());
    unsigned BrOpc = MI.getOperand(5).getImm();
    BuildMI(MBB, DL, TII.get(BrOpc)).addMBB(TailMBB);
  } else {
    // SELECT_GPR: operands are dst, trueval, falseval, cond.  The cond is
    // an s1 boolean; only bit 0 is meaningful (upper bits of a same-bank
    // GPR copy are unspecified for `G_TRUNC s32→s1` etc.), so test bit 0
    // explicitly with TESTi 1 rather than the whole word.  The legalizer's
    // `G_SELECT legalFor({{s32, s1}, {p0, s1}})` rule guarantees s1 here.
    auto CondReg = MI.getOperand(3).getReg();
    assert(MF->getRegInfo().getType(CondReg) == LLT::scalar(1) &&
           "Penumbra SELECT_GPR condition must be legalized to s1");
    BuildMI(MBB, DL, TII.get(Penumbra::TESTi))
        .addReg(CondReg)
        .addImm(1);
    BuildMI(MBB, DL, TII.get(Penumbra::BNE)).addMBB(TailMBB);
  }

  // FalseMBB: unconditional jump to tailMBB.
  BuildMI(FalseMBB, DL, TII.get(Penumbra::B)).addMBB(TailMBB);

  MI.eraseFromParent();
  return TailMBB;
}

// ── LSR / CodeGenPrepare addressing-mode cost ───────────────────────────────
//
// The `AddrMode` struct expresses the address as
//
//     BaseGV + BaseOffs + (HasBaseReg ? Reg : 0) + Scale * Index
//
// (plus ScalableOffset for vector targets, which we don't have).  We must
// return true when the entire expression can be encoded as a single
// load/store operand on Penumbra, false otherwise.
//
// Penumbra's M-format load/store encodes exactly `[Rb + simm16_offset]`
// where the offset is a signed 16-bit immediate (with a size-dependent
// implicit shift handled at encode time, but for cost-model purposes
// treat it as a signed 16-bit value).  Anything else — globals folded
// into the address, scaled or unscaled index registers, displacements
// outside ±32K — must be materialised separately and is therefore not a
// "free" addressing mode for LSR's purposes.
bool PenumbraISelLowering::isLegalAddressingMode(const DataLayout &DL,
                                                  const AddrMode &AM,
                                                  Type *Ty, unsigned AS,
                                                  Instruction *I) const {
  if (AM.BaseGV != nullptr)
    return false;

  if (AM.Scale != 0)
    return false;

  if (AM.ScalableOffset != 0)
    return false;

  if (!isInt<16>(AM.BaseOffs))
    return false;

  // No alignment check on Ty: LSR is queried O(formulae × uses × loop)
  // times during cost-model search, and `DL.getTypeStoreSize(Ty)` hits
  // `TargetExtType::getLayoutType` for some types — a registry walk that
  // compounds into a multi-second hang on inputs like gcc-c-torture's
  // pr65401.c.  LSR derives candidate offsets from valid GEPs, so the
  // offsets it proposes are already naturally aligned in practice; if
  // something hand-crafted slips through, the load/store legalizer's
  // unaligned-access path catches it at codegen time.

  if (!AM.HasBaseReg)
    return false;

  return true;
}
