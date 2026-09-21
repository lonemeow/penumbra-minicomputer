//===-- PenumbraInstrInfo.cpp - Penumbra Instruction Info ------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraInstrInfo.h"
#include "MCTargetDesc/PenumbraFixupKinds.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "PenumbraSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineBasicBlock.h"

#define GET_INSTRINFO_CTOR_DTOR
#include "PenumbraGenInstrInfo.inc"

using namespace llvm;

PenumbraInstrInfo::PenumbraInstrInfo(const TargetSubtargetInfo &STI,
                                     const TargetRegisterInfo &TRI)
    : PenumbraGenInstrInfo(STI, TRI, Penumbra::ADJCALLSTACKDOWN,
                           Penumbra::ADJCALLSTACKUP, ~0u, Penumbra::RET) {}

void PenumbraInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                     MachineBasicBlock::iterator MI,
                                     const DebugLoc &DL, Register DestReg,
                                     Register SrcReg, bool KillSrc,
                                     bool RenamableDest,
                                     bool RenamableSrc) const {
  BuildMI(MBB, MI, DL, get(Penumbra::MOV), DestReg)
      .addReg(SrcReg, getKillRegState(KillSrc));
}

bool PenumbraInstrInfo::isAsCheapAsAMove(const MachineInstr &MI) const {
  switch (MI.getOpcode()) {
  default:
    return MI.isAsCheapAsAMove();
  case Penumbra::PseudoLI:
    return MI.getMF()
        ->getSubtarget<PenumbraSubtarget>()
        .preferRecomputingWideConstants();
  }
}

bool PenumbraInstrInfo::expandPostRAPseudo(MachineInstr &MI) const {
  MachineBasicBlock &MBB = *MI.getParent();
  const DebugLoc &DL = MI.getDebugLoc();

  switch (MI.getOpcode()) {
  default:
    return false;

  case Penumbra::LEAfi: {
    // LEAfi Rd, R14, #offset  →  MOV Rd, R14  [+ ADDi Rd, #offset if nonzero]
    Register DstReg = MI.getOperand(0).getReg();
    Register BaseReg = MI.getOperand(1).getReg();
    int64_t Offset = MI.getOperand(2).getImm();

    BuildMI(MBB, MI, DL, get(Penumbra::MOV), DstReg)
        .addReg(BaseReg);

    if (Offset != 0) {
      BuildMI(MBB, MI, DL, get(Penumbra::ADDi), DstReg)
          .addReg(DstReg)
          .addImm(Offset);
    }

    MI.eraseFromParent();
    return true;
  }

  case Penumbra::PseudoLI: {
    // PseudoLI Rd, val  →  LLI Rd, lo16(val)  ;  LUI Rd, Rd, hi16(val).
    // After register allocation both halves write Rd directly.
    Register DstReg = MI.getOperand(0).getReg();
    MachineOperand Lo = MI.getOperand(1);
    MachineOperand Hi = MI.getOperand(1);
    if (Lo.isImm()) {
      Lo.setImm(Lo.getImm() & 0xFFFF);
      Hi.setImm((Hi.getImm() >> 16) & 0xFFFF);
    } else {
      bool IsTLS = Lo.getTargetFlags() == Penumbra::S_TLSgd_Lo16;
      assert((IsTLS || Lo.getTargetFlags() == 0) &&
             "PseudoLI symbol with an unexpected relocation");
      Lo.setTargetFlags(IsTLS ? Penumbra::S_TLSgd_Lo16 : Penumbra::S_Lo16);
      Hi.setTargetFlags(IsTLS ? Penumbra::S_TLSgd_Hi16 : Penumbra::S_Hi16);
    }

    BuildMI(MBB, MI, DL, get(Penumbra::LLI), DstReg).add(Lo);
    BuildMI(MBB, MI, DL, get(Penumbra::LUI), DstReg).addReg(DstReg).add(Hi);

    MI.eraseFromParent();
    return true;
  }
  }
}

void PenumbraInstrInfo::storeRegToStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
    bool IsKill, int FrameIdx, const TargetRegisterClass *RC, Register VReg,
    MachineInstr::MIFlag Flags) const {
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIdx),
      MachineMemOperand::MOStore,
      MFI.getObjectSize(FrameIdx), MFI.getObjectAlign(FrameIdx));

  BuildMI(MBB, MI, DebugLoc(), get(Penumbra::STW))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addFrameIndex(FrameIdx)
      .addImm(0)
      .addMemOperand(MMO);
}

void PenumbraInstrInfo::loadRegFromStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register DestReg,
    int FrameIdx, const TargetRegisterClass *RC, Register VReg,
    unsigned SubReg, MachineInstr::MIFlag Flags) const {
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();

  MachineMemOperand *MMO = MF.getMachineMemOperand(
      MachinePointerInfo::getFixedStack(MF, FrameIdx),
      MachineMemOperand::MOLoad,
      MFI.getObjectSize(FrameIdx), MFI.getObjectAlign(FrameIdx));

  BuildMI(MBB, MI, DebugLoc(), get(Penumbra::LDW), DestReg)
      .addFrameIndex(FrameIdx)
      .addImm(0)
      .addMemOperand(MMO);
}

// ===----------------------------------------------------------------------===//
// Branch analysis / insertion / removal
// ===----------------------------------------------------------------------===//

static bool isCondBranch(unsigned Opc) {
  switch (Opc) {
  case Penumbra::BEQ: case Penumbra::BNE:
  case Penumbra::BCS: case Penumbra::BCC:
  case Penumbra::BMI: case Penumbra::BPL:
  case Penumbra::BVS: case Penumbra::BVC:
  case Penumbra::BHI: case Penumbra::BLS:
  case Penumbra::BGE: case Penumbra::BLT:
  case Penumbra::BGT: case Penumbra::BLE:
    return true;
  default:
    return false;
  }
}

static bool isUncondBranch(unsigned Opc) {
  return Opc == Penumbra::B;
}

unsigned PenumbraInstrInfo::getOppositeBranchOpcode(unsigned Opc) {
  switch (Opc) {
  case Penumbra::BEQ: return Penumbra::BNE;
  case Penumbra::BNE: return Penumbra::BEQ;
  case Penumbra::BCS: return Penumbra::BCC;
  case Penumbra::BCC: return Penumbra::BCS;
  case Penumbra::BMI: return Penumbra::BPL;
  case Penumbra::BPL: return Penumbra::BMI;
  case Penumbra::BVS: return Penumbra::BVC;
  case Penumbra::BVC: return Penumbra::BVS;
  case Penumbra::BHI: return Penumbra::BLS;
  case Penumbra::BLS: return Penumbra::BHI;
  case Penumbra::BGE: return Penumbra::BLT;
  case Penumbra::BLT: return Penumbra::BGE;
  case Penumbra::BGT: return Penumbra::BLE;
  case Penumbra::BLE: return Penumbra::BGT;
  default: llvm_unreachable("not a conditional branch");
  }
}

bool PenumbraInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                       MachineBasicBlock *&TBB,
                                       MachineBasicBlock *&FBB,
                                       SmallVectorImpl<MachineOperand> &Cond,
                                       bool AllowModify) const {
  TBB = nullptr;
  FBB = nullptr;
  Cond.clear();

  // Scan backwards past non-terminators and debug values.
  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (!I->isTerminator())
      break;
  }
  // Now advance past the non-terminator to the first terminator.
  if (I != MBB.end() && !I->isTerminator())
    ++I;

  // No terminators — block falls through.
  if (I == MBB.end())
    return false;

  MachineInstr &FirstTerm = *I;
  unsigned FirstOpc = FirstTerm.getOpcode();

  // Unanalyzable terminators (indirect branches, returns, etc.).
  if (!isCondBranch(FirstOpc) && !isUncondBranch(FirstOpc))
    return true;

  // Check for a second terminator.
  MachineInstr *SecondTerm = nullptr;
  MachineBasicBlock::iterator Next = std::next(I);
  if (Next != MBB.end() && Next->isTerminator()) {
    SecondTerm = &*Next;
    // More than two terminators — unanalyzable.
    MachineBasicBlock::iterator Third = std::next(Next);
    if (Third != MBB.end() && Third->isTerminator())
      return true;
  }

  // Single unconditional branch.
  if (isUncondBranch(FirstOpc) && !SecondTerm) {
    TBB = FirstTerm.getOperand(0).getMBB();
    return false;
  }

  // Single conditional branch (fallthrough on false).
  if (isCondBranch(FirstOpc) && !SecondTerm) {
    TBB = FirstTerm.getOperand(0).getMBB();
    Cond.push_back(MachineOperand::CreateImm(FirstOpc));
    return false;
  }

  // Conditional followed by unconditional.
  if (isCondBranch(FirstOpc) && SecondTerm &&
      isUncondBranch(SecondTerm->getOpcode())) {
    TBB = FirstTerm.getOperand(0).getMBB();
    FBB = SecondTerm->getOperand(0).getMBB();
    Cond.push_back(MachineOperand::CreateImm(FirstOpc));
    return false;
  }

  // Anything else is unanalyzable.
  return true;
}

unsigned PenumbraInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                          MachineBasicBlock *TBB,
                                          MachineBasicBlock *FBB,
                                          ArrayRef<MachineOperand> Cond,
                                          const DebugLoc &DL,
                                          int *BytesAdded) const {
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  unsigned Count = 0;

  if (Cond.empty()) {
    // Unconditional branch.
    BuildMI(&MBB, DL, get(Penumbra::B)).addMBB(TBB);
    if (BytesAdded)
      *BytesAdded = 4;
    return 1;
  }

  // Conditional branch.
  assert(Cond.size() == 1 && "Penumbra branch condition has one element");
  unsigned Opc = Cond[0].getImm();
  BuildMI(&MBB, DL, get(Opc)).addMBB(TBB);
  Count = 1;
  int Bytes = 4;

  if (FBB) {
    // Two-way: conditional to TBB, unconditional to FBB.
    BuildMI(&MBB, DL, get(Penumbra::B)).addMBB(FBB);
    Count = 2;
    Bytes += 4;
  }

  if (BytesAdded)
    *BytesAdded = Bytes;
  return Count;
}

unsigned PenumbraInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                          int *BytesRemoved) const {
  unsigned Count = 0;
  int Bytes = 0;

  MachineBasicBlock::iterator I = MBB.end();
  while (I != MBB.begin()) {
    --I;
    if (I->isDebugInstr())
      continue;
    unsigned Opc = I->getOpcode();
    if (!isCondBranch(Opc) && !isUncondBranch(Opc))
      break;
    I->eraseFromParent();
    I = MBB.end();
    ++Count;
    Bytes += 4;
  }

  if (BytesRemoved)
    *BytesRemoved = Bytes;
  return Count;
}

bool PenumbraInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 1 && "Penumbra branch condition has one element");
  Cond[0].setImm(getOppositeBranchOpcode(Cond[0].getImm()));
  return false;
}

//===----------------------------------------------------------------------===//
// Compare-elimination peephole.
//
// Penumbra has flag-setting ALU instructions (Defs = [SR]) and a separate
// CMP/CMPi/TEST/TESTi family that *only* sets flags.  After GISel folds
// G_ICMP+G_BRCOND into CMPi+Bcc, code sequences like
//
//     SUBi  Rx, 1
//     CMPi  Rx, 0    ;  redundant — SUBi already set Z/N
//     BNE   loop
//
// are common.  The generic LLVM `PeepholeOptimizer` (added at -O1+ by
// `addMachineSSAOptimization`) drives compare-elimination via two virtual
// hooks on `TargetInstrInfo`: `analyzeCompare` (recognise the CMP) and
// `optimizeCompareInstr` (decide whether to delete it).
//
// Pattern handled here: `CMPi Rx, 0` whose Z/N can be supplied by an
// earlier flag-setting ALU op that writes Rx.  The producer may be
// separated from the CMP by intervening SR clobbers (e.g. unrelated
// pointer-increment `ADDi`s in a memcpy loop) — we walk back through
// them as long as Rx is neither read nor re-defined in the gap.  When
// the producer is found, we splice it down to be immediately before
// the CMP and erase the CMP; this re-establishes the producer's flags
// as the most recent SR write before any subsequent flag reader.  The
// splice is mandatory: without it, intervening SR-clobbering ALU ops
// would leave SR holding their flags, not the producer's, and a
// downstream BNE would branch on the wrong condition.
//
// Forward-direction safety: SR readers between the CMP and the next SR
// redefinition must consume only Z and N (BEQ/BNE/BMI/BPL).  C and V
// differ between `SUBi Rx, k` and `CMP Rx, 0` whenever k != 0, so any
// reader of C or V must keep the explicit CMP.
//
// Second pattern: a wrap check, `CMP %sum, %a` after `%sum = ADD %a, %b`
// (or `CMPi` against an `ADDi`'s own immediate).  Here only C carries
// over, inverted, so every reader must consume C alone and has its
// condition swapped.
//
// Future extensions (left as TODOs):
//   - `CMP Rx, Ry` after `SUB Rx_dst, Ry`.
//   - `TESTi Rx, mask` after `ANDi Rx, mask`.
//   - Looking through trivial COPY chains so SUB-then-COPY-then-CMP elides.
//   - Cross-MBB analysis (currently scoped to one block).
//===----------------------------------------------------------------------===//

namespace {

// Bitmask of SR flags read by each Bcc opcode.  Bit 0 = Z, 1 = N, 2 = C,
// 3 = V.  Branches outside this table conservatively report "all flags."
constexpr unsigned FZ = 0x1;
constexpr unsigned FN = 0x2;
constexpr unsigned FC = 0x4;
constexpr unsigned FV = 0x8;

unsigned getBranchFlagsRead(unsigned Opcode) {
  switch (Opcode) {
  case Penumbra::BEQ:
  case Penumbra::BNE:
    return FZ;
  case Penumbra::BMI:
  case Penumbra::BPL:
    return FN;
  case Penumbra::BCS:
  case Penumbra::BCC:
    return FC;
  case Penumbra::BVS:
  case Penumbra::BVC:
    return FV;
  case Penumbra::BHI:
  case Penumbra::BLS:
    return FZ | FC;
  case Penumbra::BGE:
  case Penumbra::BLT:
    return FN | FV;
  case Penumbra::BGT:
  case Penumbra::BLE:
    return FZ | FN | FV;
  default:
    return FZ | FN | FC | FV;
  }
}

// True if `Opcode` is a Penumbra ALU op that sets SR.{Z,N} from the value
// it writes back into operand 0.  Excludes ADC/SBC because they also *use*
// SR (carry-in dependency), so relocating them past intervening SR clobbers
// would change the carry-in they observe.  Also excludes MOV (no flags),
// LLI/LLIS/LUI (no flags), CMP/TEST family (no GPR result), and any
// non-arithmetic instruction.
bool definesFlagsFromResult(unsigned Opcode) {
  switch (Opcode) {
  case Penumbra::ADD:
  case Penumbra::ADDi:
  case Penumbra::SUB:
  case Penumbra::SUBi:
  case Penumbra::AND:
  case Penumbra::ANDi:
  case Penumbra::OR:
  case Penumbra::XOR:
  case Penumbra::SHL:
  case Penumbra::SHLi:
  case Penumbra::SHR:
  case Penumbra::SHRi:
  case Penumbra::SAR:
  case Penumbra::SARi:
  case Penumbra::NOT:
    return true;
  default:
    return false;
  }
}

// True when `Producer` already left SR holding the Z and N that a compare
// masking its result with `CmpMask` would set — so the compare is redundant.
// `CmpMask` is all-ones for `CMPi Rx, 0`, the immediate for `TESTi Rx, mask`.
// The producer's mask names the bits its result may leave set; masking again
// changes nothing exactly when those bits are a subset of `CmpMask`.
//
// One answer covers Z and N: `ANDi`/`TESTi` take a zero-extended uimm16, so
// bit 31 is always clear and N is 0 after either.  A register-form
// `TEST Rx, Ry` would keep bit 31 and need N judged separately.
bool producesNZFlagsForMask(const MachineInstr &Producer, int64_t CmpMask) {
  int64_t ProducerMask;
  switch (Producer.getOpcode()) {
  case Penumbra::ANDi:
    ProducerMask = Producer.getOperand(2).getImm();
    break;
  default:
    ProducerMask = ~static_cast<int64_t>(0);
    break;
  }
  return (ProducerMask & CmpMask) == ProducerMask;
}

// True when Cmp tests whether Add wrapped: it compares Add's sum against one
// of Add's addends, which the sum is below exactly when the add wrapped.
// `CMP %sum, %a` or `CMP %sum, %b` after `%sum = ADD %a, %b`, and
// `CMPi %sum, C` after `%sum = ADDi %a, C`.
bool isWrapCheck(const MachineInstr &Add, const MachineInstr &Cmp) {
  bool IsReg =
      Add.getOpcode() == Penumbra::ADD && Cmp.getOpcode() == Penumbra::CMP;
  bool IsImm =
      Add.getOpcode() == Penumbra::ADDi && Cmp.getOpcode() == Penumbra::CMPi;
  if (!IsReg && !IsImm)
    return false;
  if (Cmp.getOperand(0).getReg() != Add.getOperand(0).getReg())
    return false;
  if (IsImm)
    return Cmp.getOperand(1).getImm() == Add.getOperand(2).getImm();
  Register CmpReg = Cmp.getOperand(1).getReg();
  return CmpReg == Add.getOperand(1).getReg() ||
         CmpReg == Add.getOperand(2).getReg();
}

} // namespace

bool PenumbraInstrInfo::analyzeCompare(const MachineInstr &MI, Register &SrcReg,
                                       Register &SrcReg2, int64_t &CmpMask,
                                       int64_t &CmpValue) const {
  switch (MI.getOpcode()) {
  case Penumbra::CMPi:
    SrcReg = MI.getOperand(0).getReg();
    SrcReg2 = Register();
    CmpMask = ~0;
    CmpValue = MI.getOperand(1).getImm();
    return true;
  // TESTi is the masked form: SR comes from `SrcReg & CmpMask` rather than a
  // subtraction, so the immediate is the mask and CmpValue is 0.
  case Penumbra::TESTi:
    SrcReg = MI.getOperand(0).getReg();
    SrcReg2 = Register();
    CmpMask = MI.getOperand(1).getImm();
    CmpValue = 0;
    return true;
  case Penumbra::CMP:
    SrcReg = MI.getOperand(0).getReg();
    SrcReg2 = MI.getOperand(1).getReg();
    CmpMask = ~0;
    CmpValue = 0;
    return true;
  // TODO: TEST when its elision pattern is added below.
  default:
    return false;
  }
}

// A wrap check on an add leaves C = NOT wrapped where the add left
// C = wrapped.  Readers of C alone take the add's flags with their condition
// swapped.  Z and N differ between the two, so any other reader keeps the
// compare.
bool PenumbraInstrInfo::optimizeWrapCheck(
    MachineInstr &CmpInstr, Register SumReg,
    const MachineRegisterInfo &MRI) const {
  MachineInstr *Add = MRI.getUniqueVRegDef(SumReg);
  MachineBasicBlock *MBB = CmpInstr.getParent();
  if (!Add || Add->getParent() != MBB || !isWrapCheck(*Add, CmpInstr))
    return false;

  // SSA puts the add before its compare; its flags must still be in SR there.
  const TargetRegisterInfo &TRI = getRegisterInfo();
  for (auto It = std::next(Add->getIterator()); It != CmpInstr.getIterator();
       ++It)
    if (It->modifiesRegister(Penumbra::SR, &TRI))
      return false;

  SmallVector<MachineInstr *, 2> Readers;
  for (auto It = std::next(CmpInstr.getIterator()); It != MBB->end(); ++It) {
    if (It->readsRegister(Penumbra::SR, &TRI)) {
      if (getBranchFlagsRead(It->getOpcode()) != FC)
        return false;
      Readers.push_back(&*It);
    }
    if (It->modifiesRegister(Penumbra::SR, &TRI))
      break;
  }
  if (Readers.empty())
    return false;

  for (MachineInstr *Reader : Readers)
    Reader->setDesc(get(getOppositeBranchOpcode(Reader->getOpcode())));
  Add->clearRegisterDeads(Penumbra::SR);
  CmpInstr.eraseFromParent();
  return true;
}

bool PenumbraInstrInfo::optimizeCompareInstr(
    MachineInstr &CmpInstr, Register SrcReg, Register /*SrcReg2*/,
    int64_t CmpMask, int64_t CmpValue,
    const MachineRegisterInfo *MRI) const {
  if (optimizeWrapCheck(CmpInstr, SrcReg, *MRI))
    return true;

  // The remaining elidable forms compare against zero: `CMPi Rx, 0` over the
  // whole word, `TESTi Rx, mask` over the masked bits.
  if (CmpValue != 0)
    return false;
  if (CmpInstr.getOpcode() != Penumbra::CMPi &&
      CmpInstr.getOpcode() != Penumbra::TESTi)
    return false;

  MachineBasicBlock *MBB = CmpInstr.getParent();
  const TargetRegisterInfo &TRI = getRegisterInfo();

  // Walk back through unrelated SR clobbers, looking for a flag-setting ALU
  // op that defines SrcReg.  Bail if SrcReg is read or re-defined by any
  // intervening instruction, since those would either see the wrong value
  // after we splice the producer down (read), or shadow it (re-def).
  MachineInstr *Producer = nullptr;
  for (auto It = CmpInstr.getIterator(); It != MBB->begin();) {
    --It;
    if (It->isDebugInstr())
      continue;

    if (It->readsRegister(SrcReg, &TRI))
      return false;

    // The splice moves the producer's implicit SR def downward; any gap
    // instruction reading SR (an ADC/SBC consuming the producer's carry)
    // would then observe stale flags.
    if (It->readsRegister(Penumbra::SR, &TRI))
      return false;

    if (It->modifiesRegister(SrcReg, &TRI)) {
      if (definesFlagsFromResult(It->getOpcode()) &&
          It->getOperand(0).isReg() && It->getOperand(0).isDef() &&
          It->getOperand(0).getReg() == SrcReg)
        Producer = &*It;
      break;
    }
  }
  if (!Producer)
    return false;
  if (!producesNZFlagsForMask(*Producer, CmpMask))
    return false;

  // Walk forward from the CMP and verify every SR reader consumes only Z
  // and N before the next SR clobber.  C and V differ between the producer
  // (e.g. `SUBi Rx, k`) and the CMP it replaces (`CMPi Rx, 0`) whenever
  // k != 0, so a reader of C or V must keep the explicit CMP.
  for (auto It = std::next(CmpInstr.getIterator()); It != MBB->end(); ++It) {
    if (It->isDebugInstr())
      continue;
    if (It->readsRegister(Penumbra::SR, &TRI)) {
      unsigned Flags = getBranchFlagsRead(It->getOpcode());
      if (Flags & ~(FZ | FN))
        return false;
    }
    if (It->modifiesRegister(Penumbra::SR, &TRI))
      break;
  }

  // Splice the producer down to immediately before the CMP, then erase the
  // CMP.  Producer's SR def becomes the most recent flag write before any
  // subsequent reader, supplying the Z/N the original CMP would have set.
  // In pre-RA SSA form, the producer's vreg uses each have a single fixed
  // definition, so moving the producer later within the MBB cannot change
  // which value those operands read.
  if (std::next(Producer->getIterator()) != CmpInstr.getIterator())
    MBB->splice(CmpInstr.getIterator(), MBB, Producer->getIterator());

  // The producer's `implicit-def $sr` may have been marked dead by earlier
  // dead-flag analysis because the (now-erased) CMP clobbered SR before
  // any reader.  After erasure the producer's SR reaches the consuming
  // Bcc, so clear the dead flag to keep the MIR well-formed.
  Producer->clearRegisterDeads(Penumbra::SR);

  CmpInstr.eraseFromParent();
  return true;
}
