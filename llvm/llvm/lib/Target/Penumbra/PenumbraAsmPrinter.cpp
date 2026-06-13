//===-- PenumbraAsmPrinter.cpp - Penumbra Assembly Printer ----------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/PenumbraFixupKinds.h"
#include "MCTargetDesc/PenumbraInstPrinter.h"
#include "MCTargetDesc/PenumbraMCTargetDesc.h"
#include "TargetInfo/PenumbraTargetInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define DEBUG_TYPE "penumbra-asm-printer"

namespace {

class PenumbraAsmPrinter : public AsmPrinter {
public:
  explicit PenumbraAsmPrinter(TargetMachine &TM,
                               std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer)) {}

  StringRef getPassName() const override {
    return "Penumbra Assembly Printer";
  }

  void emitInstruction(const MachineInstr *MI) override;
  void emitJumpTableEntry(const MachineJumpTableInfo &MJTI,
                          const MachineBasicBlock *MBB,
                          unsigned uid) const override;

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &OS) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                             const char *ExtraCode, raw_ostream &OS) override;

  /// The PC-anchor label .LPC<function>_<id> pairing a PC-anchored
  /// address sequence's immediate carriers with its anchor instruction.
  MCSymbol *getPICLabel(unsigned Id) const {
    return OutContext.getOrCreateSymbol(Twine(MAI->getPrivateLabelPrefix()) +
                                        "PC" + Twine(getFunctionNumber()) +
                                        "_" + Twine(Id));
  }
};

} // end anonymous namespace

// Lower a MachineOperand to an MCOperand. Returns false for operands that
// should be skipped (implicit defs/uses, dead regs, etc.).
static bool lowerOperand(const MachineOperand &MO, MCOperand &MCOp,
                          AsmPrinter &AP) {
  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    // Skip implicit operands (e.g. SR flags def on ALU instructions).
    if (MO.isImplicit())
      return false;
    MCOp = MCOperand::createReg(MO.getReg());
    return true;
  case MachineOperand::MO_Immediate:
    MCOp = MCOperand::createImm(MO.getImm());
    return true;
  case MachineOperand::MO_MachineBasicBlock:
    MCOp = MCOperand::createExpr(
        MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), AP.OutContext));
    return true;
  case MachineOperand::MO_JumpTableIndex: {
    const MCExpr *Expr =
        MCSymbolRefExpr::create(AP.GetJTISymbol(MO.getIndex()), AP.OutContext);
    unsigned TF = MO.getTargetFlags();
    if (TF == Penumbra::S_Lo16 || TF == Penumbra::S_Hi16)
      Expr = MCSpecifierExpr::create(Expr, TF, AP.OutContext);
    MCOp = MCOperand::createExpr(Expr);
    return true;
  }
  case MachineOperand::MO_ExternalSymbol:
    MCOp = MCOperand::createExpr(
        MCSymbolRefExpr::create(AP.GetExternalSymbolSymbol(MO.getSymbolName()),
                                AP.OutContext));
    return true;
  case MachineOperand::MO_GlobalAddress:
  case MachineOperand::MO_BlockAddress: {
    const MCExpr *Expr;
    if (MO.isGlobal())
      Expr = MCSymbolRefExpr::create(AP.getSymbol(MO.getGlobal()),
                                     AP.OutContext);
    else
      Expr = MCSymbolRefExpr::create(
          AP.GetBlockAddressSymbol(MO.getBlockAddress()), AP.OutContext);
    if (MO.getOffset())
      Expr = MCBinaryExpr::createAdd(
          Expr, MCConstantExpr::create(MO.getOffset(), AP.OutContext),
          AP.OutContext);
    unsigned TF = MO.getTargetFlags();
    if (TF == Penumbra::S_Lo16 || TF == Penumbra::S_Hi16 ||
        TF == Penumbra::S_PCRel ||
        TF == Penumbra::S_TLSgd_Lo16 || TF == Penumbra::S_TLSgd_Hi16 ||
        TF == Penumbra::S_TLSgd_PCRel ||
        TF == Penumbra::S_GOT_PCRel_Lo16 ||
        TF == Penumbra::S_GOT_PCRel_Hi16 ||
        TF == Penumbra::S_TLSgd_GOT_PCRel_Lo16 ||
        TF == Penumbra::S_TLSgd_GOT_PCRel_Hi16)
      Expr = MCSpecifierExpr::create(Expr, TF, AP.OutContext);
    MCOp = MCOperand::createExpr(Expr);
    return true;
  }
  default:
    return false;
  }
}

// Lower the symbol operand of a PC-anchored pseudo to
// %specifier((sym + offset) - .LPC<f>_<id>).  The subtracted anchor label
// is local and same-section, so the assembler folds it into the
// relocation addend (A = P - Q per the PC-anchored relocation pair
// convention in doc/system/abi.md).
static MCOperand lowerAnchoredOperand(const MachineOperand &MO,
                                      MCSymbol *PICLabel, AsmPrinter &AP) {
  const MCExpr *Expr;
  switch (MO.getType()) {
  case MachineOperand::MO_GlobalAddress:
    Expr = MCSymbolRefExpr::create(AP.getSymbol(MO.getGlobal()),
                                   AP.OutContext);
    break;
  case MachineOperand::MO_JumpTableIndex:
    Expr = MCSymbolRefExpr::create(AP.GetJTISymbol(MO.getIndex()),
                                   AP.OutContext);
    break;
  case MachineOperand::MO_BlockAddress:
    Expr = MCSymbolRefExpr::create(
        AP.GetBlockAddressSymbol(MO.getBlockAddress()), AP.OutContext);
    break;
  default:
    llvm_unreachable("unexpected symbol operand on PC-anchored pseudo");
  }
  if (MO.getOffset())
    Expr = MCBinaryExpr::createAdd(
        Expr, MCConstantExpr::create(MO.getOffset(), AP.OutContext),
        AP.OutContext);
  Expr = MCBinaryExpr::createSub(
      Expr, MCSymbolRefExpr::create(PICLabel, AP.OutContext), AP.OutContext);
  Expr = MCSpecifierExpr::create(Expr, MO.getTargetFlags(), AP.OutContext);
  return MCOperand::createExpr(Expr);
}

void PenumbraAsmPrinter::emitInstruction(const MachineInstr *MI) {
  unsigned Opc = MI->getOpcode();

  // ── Pseudo instructions ──────────────────────────────────────────────────

  // PC-anchored PIC address sequences: the anchor pseudos define the
  // .LPC label at the PC-reading instruction; the immediate carriers
  // reference it through label-difference operands.
  if (Opc == Penumbra::PICADDPC || Opc == Penumbra::PICMOVPC) {
    bool IsAdd = (Opc == Penumbra::PICADDPC);
    unsigned IdOpNo = IsAdd ? 2 : 1;
    OutStreamer->emitLabel(getPICLabel(MI->getOperand(IdOpNo).getImm()));
    MCInst Inst;
    Inst.setOpcode(IsAdd ? Penumbra::ADD : Penumbra::MOV);
    Inst.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
    if (IsAdd)
      Inst.addOperand(MCOperand::createReg(MI->getOperand(1).getReg()));
    Inst.addOperand(MCOperand::createReg(Penumbra::R15));
    EmitToStreamer(*OutStreamer, Inst);
    return;
  }
  if (Opc == Penumbra::PICLLI) {
    MCInst Inst;
    Inst.setOpcode(Penumbra::LLI);
    Inst.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
    Inst.addOperand(lowerAnchoredOperand(
        MI->getOperand(1), getPICLabel(MI->getOperand(2).getImm()), *this));
    EmitToStreamer(*OutStreamer, Inst);
    return;
  }
  if (Opc == Penumbra::PICLUI || Opc == Penumbra::PICADDi) {
    MCInst Inst;
    Inst.setOpcode(Opc == Penumbra::PICLUI ? Penumbra::LUI : Penumbra::ADDi);
    Inst.addOperand(MCOperand::createReg(MI->getOperand(0).getReg()));
    Inst.addOperand(MCOperand::createReg(MI->getOperand(1).getReg()));
    Inst.addOperand(lowerAnchoredOperand(
        MI->getOperand(2), getPICLabel(MI->getOperand(3).getImm()), *this));
    EmitToStreamer(*OutStreamer, Inst);
    return;
  }

  if (Opc == Penumbra::RET) {
    // RET pseudo → JMP R13 (return via link register)
    MCInst JMPInst;
    JMPInst.setOpcode(Penumbra::JMP);
    JMPInst.addOperand(MCOperand::createReg(Penumbra::R13));
    EmitToStreamer(*OutStreamer, JMPInst);
    return;
  }
  if (Opc == Penumbra::ADJCALLSTACKDOWN || Opc == Penumbra::ADJCALLSTACKUP) {
    // Stack adjustment pseudos: frame lowering handles these; nothing to emit.
    return;
  }
  if (Opc == TargetOpcode::COPY) {
    Register Dst = MI->getOperand(0).getReg();
    Register Src = MI->getOperand(1).getReg();
    if (Dst == Src)
      return; // no-op copy — elide
    MCInst MOVInst;
    MOVInst.setOpcode(Penumbra::MOV);
    MOVInst.addOperand(MCOperand::createReg(Dst));
    MOVInst.addOperand(MCOperand::createReg(Src));
    EmitToStreamer(*OutStreamer, MOVInst);
    return;
  }

  // ── Generic lowering ─────────────────────────────────────────────────────
  // Build MCInst by iterating the MachineInstr operands in order, skipping
  // implicit operands (e.g. flag defs like SR).
  MCInst Inst;
  Inst.setOpcode(Opc);
  for (const MachineOperand &MO : MI->operands()) {
    MCOperand MCOp;
    if (lowerOperand(MO, MCOp, *this))
      Inst.addOperand(MCOp);
  }
  EmitToStreamer(*OutStreamer, Inst);
}

/// PrintAsmOperand - Print an inline asm operand for use in an asm template.
/// Called for each $0, $1, etc. in the template string. Returns false on
/// success, true on failure (LLVM convention).
bool PenumbraAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                          const char *ExtraCode,
                                          raw_ostream &OS) {
  if (ExtraCode && ExtraCode[0])
    return AsmPrinter::PrintAsmOperand(MI, OpNo, ExtraCode, OS);

  auto MO = MI->getOperand(OpNo);
  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    OS << PenumbraInstPrinter::getRegisterName(MO.getReg());
    break;
  case MachineOperand::MO_Immediate:
    OS << MO.getImm();
    break;
  case MachineOperand::MO_GlobalAddress:
  case MachineOperand::MO_BlockAddress:
    PrintSymbolOperand(MO, OS);
    break;
  default:
    return true;
  }

  return false;
}

bool PenumbraAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                                unsigned OpNo,
                                                const char *ExtraCode,
                                                raw_ostream &OS) {
  // No modifier support for memory operands.
  if (ExtraCode && ExtraCode[0])
    return true;

  const MachineOperand &MO = MI->getOperand(OpNo);
  if (!MO.isReg())
    return true;

  OS << "[" << PenumbraInstPrinter::getRegisterName(MO.getReg()) << "]";
  return false;
}

void PenumbraAsmPrinter::emitJumpTableEntry(const MachineJumpTableInfo &MJTI,
                                            const MachineBasicBlock *MBB,
                                            unsigned uid) const {
  // Always emit label-difference entries (.word target - JT_base),
  // regardless of the MachineJumpTableInfo encoding.  This avoids
  // dynamic relocations in PIE and matches the BRJT instruction
  // selection which always adds the base back.
  const MCExpr *Value = MCSymbolRefExpr::create(MBB->getSymbol(), OutContext);
  const TargetLowering *TLI = MF->getSubtarget().getTargetLowering();
  const MCExpr *Base = TLI->getPICJumpTableRelocBaseExpr(MF, uid, OutContext);
  Value = MCBinaryExpr::createSub(Value, Base, OutContext);
  OutStreamer->emitValue(Value, getDataLayout().getPointerSize());
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraAsmPrinter() {
  RegisterAsmPrinter<PenumbraAsmPrinter> X(getThePenumbraTarget());
}
