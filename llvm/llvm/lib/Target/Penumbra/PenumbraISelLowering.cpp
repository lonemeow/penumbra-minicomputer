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
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/KnownBits.h"

using namespace llvm;

// TableGen-generated CC assignment functions (CC_Penumbra, RetCC_Penumbra).
// Must be included after `using namespace llvm` — the generated code uses
// MVT, CCValAssign, CCState etc. without namespace qualification.
#include "PenumbraGenCallingConv.inc"

PenumbraISelLowering::PenumbraISelLowering(const TargetMachine &TM,
                                            const PenumbraSubtarget &STI)
    : TargetLowering(TM, STI), Subtarget(STI) {
  addRegisterClass(MVT::i32, &Penumbra::GPR_AllocatableRegClass);

  // SP register — needed by G_DYN_STACKALLOC lowering and G_STACKSAVE/RESTORE.
  setStackPointerRegisterToSaveRestore(Penumbra::R14);

  // Comparisons produce a word that is exactly 0 or 1.  GISelValueTracking
  // only marks bits 31:1 of a comparison known-zero when this is declared,
  // and it is gated on the result being wider than one bit — so it is inert
  // unless the legalizer also gives G_ICMP a register-width result.
  setBooleanContents(ZeroOrOneBooleanContent);

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

  // An inline memcpy, memmove or memset trades a call for its loads and
  // stores.  On a small instruction cache the code that adds costs more than
  // the call, past the four stores LLVM allows when optimizing for size.
  if (STI.hasSmallICache())
    MaxStoresPerMemcpy = MaxStoresPerMemmove = MaxStoresPerMemset = 4;

  computeRegisterProperties(STI.getRegisterInfo());
}

bool PenumbraISelLowering::preferSelectsOverBooleanArithmetic(EVT) const {
  return true;
}

void PenumbraISelLowering::computeKnownBitsForTargetInstr(
    GISelValueTracking &Analysis, Register R, KnownBits &Known,
    const APInt &DemandedElts, const MachineRegisterInfo &MRI,
    unsigned Depth) const {
  const MachineInstr *MI = MRI.getVRegDef(R);
  if (MI && MI->getOpcode() == Penumbra::G_ICMP_PAIR)
    Known.Zero.setBitsFrom(1);
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
  // value.
  const TargetRegisterInfo *TRI = Subtarget.getRegisterInfo();
  if (!TRI->getReservedRegs(MF).test(Reg))
    reportFatalUsageError(Twine("named global register \"") + RegName +
                          "\" is allocatable; only reserved registers may be "
                          "pinned");

  return Reg;
}

// Returning true makes the GISel combiner leave a divide or remainder by a
// constant as a DIV instead of applying the `*_by_const` magic-number
// rewrite, whose sequence is a high-half multiply (G_UMULH/G_SMULH), a
// multiply back for a remainder, and shift/add fixups.
bool PenumbraISelLowering::isIntDivCheap(EVT VT, AttributeList Attr) const {
  if (Attr.hasFnAttr(Attribute::MinSize))
    return true;

  // A signed high-half multiply wider than the divmul unit's operands has no
  // legalization, and nothing here says which signedness is being asked about,
  // so wider divides keep their libcall.
  if (VT.bitsGT(MVT::i32))
    return true;

  // The worst-case rewrite is two multiplies and a few ALU ops.
  unsigned RewriteCycles =
      Subtarget.getMulCycles() * 2 + Subtarget.getALUCycles() * 5;
  return Subtarget.getDivCycles() <= RewriteCycles;
}

// The default refuses G_PTR_ADD, and a global or frame address whose only
// user is such an add is then pinned in the entry block beside it: computed on
// every path and held live to its uses in a register or stack slot.
bool PenumbraISelLowering::shouldLocalize(
    const MachineInstr &MI, const TargetTransformInfo *TTI) const {
  if (MI.getOpcode() == TargetOpcode::G_PTR_ADD) {
    const MachineRegisterInfo &MRI = MI.getMF()->getRegInfo();
    auto BaseDef = MRI.getVRegDef(MI.getOperand(1).getReg());
    auto OffsetDef = MRI.getVRegDef(MI.getOperand(2).getReg());
    return shouldLocalize(*BaseDef, TTI) && shouldLocalize(*OffsetDef, TTI);
  }
  return TargetLowering::shouldLocalize(MI, TTI);
}

// Expand a SELECT_* pseudo into a conditional-branch diamond.
//
// After expansion (three blocks):
//   thisMBB:
//     ... preceding instructions ...
//     TEST/CMP (set flags; none for SELECT_FLAGS_GPR, whose are already set)
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
          Opc == Penumbra::SELECT_CCi_GPR ||
          Opc == Penumbra::SELECT_FLAGS_GPR) &&
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
  } else if (Opc == Penumbra::SELECT_FLAGS_GPR) {
    // SELECT_FLAGS_GPR: operands are dst, trueval, falseval, cc.  The flags
    // were set by the instructions ahead of it, which stay in thisMBB.
    BuildMI(MBB, DL, TII.get(MI.getOperand(3).getImm())).addMBB(TailMBB);
  } else {
    // SELECT_GPR: operands are dst, trueval, falseval, cond.  Only bit 0 of
    // cond is meaningful — a boolean reaching here through `G_ANYEXT` of a
    // `G_TRUNC s32→s1` leaves the upper bits of a same-bank GPR copy
    // unspecified — so test bit 0 rather than the whole word.
    auto CondReg = MI.getOperand(3).getReg();
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

// Give every SR definition in MBB the dead flag its uses call for.  Selection
// keeps each flag value within one block, so the uses that follow a definition
// in its block are all of them.  Flags live into a block that does not name SR
// as a live-in break that: their producer, in another block, would be marked
// dead.
static void markDeadFlagDefs(MachineBasicBlock &MBB) {
  if (Penumbra::flagsLiveBefore(MBB, MBB.begin()) &&
      !MBB.isLiveIn(Penumbra::SR))
    report_fatal_error("flags live into a block without SR as a live-in");

  // Each query stops at the next definition, so the block is walked once.
  for (MachineInstr &MI : MBB) {
    for (MachineOperand &MO : MI.all_defs()) {
      if (MO.getReg() != Penumbra::SR)
        continue;
      MO.setIsDead(
          !Penumbra::flagUsesFrom(MBB, std::next(MI.getIterator())).live());
    }
  }
}

// An instruction selected through a TableGen pattern has its unused SR
// definition marked dead; one built by hand, in the selector or the select
// expansion above, has not.  The machine passes that hoist, merge and sink
// instructions while the function is still in SSA form leave one with a live
// physical-register definition where selection put it, though it has no side
// effect, and they run before liveness is next computed.
void PenumbraISelLowering::finalizeLowering(MachineFunction &MF) const {
  for (MachineBasicBlock &MBB : MF)
    markDeadFlagDefs(MBB);
  TargetLowering::finalizeLowering(MF);
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

// The generic expansion splits the operation into accesses of this type and
// leaves a library call past MaxStoresPerMem* of them.  Its default starts
// from s64 and, Penumbra having no misaligned access, drops from there
// straight to s8 whenever the alignment is smaller.
LLT PenumbraISelLowering::getOptimalMemOpLLT(
    const MemOp &Op, const AttributeList &FuncAttributes) const {
  if (Op.isAligned(Align(4)))
    return LLT::scalar(32);
  if (Op.isAligned(Align(2)))
    return LLT::scalar(16);
  return LLT::scalar(8);
}
