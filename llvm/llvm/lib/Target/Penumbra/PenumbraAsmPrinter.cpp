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

  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       const char *ExtraCode, raw_ostream &OS) override;
  bool PrintAsmMemoryOperand(const MachineInstr *MI, unsigned OpNo,
                             const char *ExtraCode, raw_ostream &OS) override;
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
  case MachineOperand::MO_GlobalAddress: {
    const MCExpr *Expr =
        MCSymbolRefExpr::create(AP.getSymbol(MO.getGlobal()), AP.OutContext);
    if (MO.getOffset())
      Expr = MCBinaryExpr::createAdd(
          Expr, MCConstantExpr::create(MO.getOffset(), AP.OutContext),
          AP.OutContext);
    unsigned TF = MO.getTargetFlags();
    if (TF == Penumbra::S_Lo16 || TF == Penumbra::S_Hi16 ||
        TF == Penumbra::S_PCRel)
      Expr = MCSpecifierExpr::create(Expr, TF, AP.OutContext);
    MCOp = MCOperand::createExpr(Expr);
    return true;
  }
  default:
    return false;
  }
}

void PenumbraAsmPrinter::emitInstruction(const MachineInstr *MI) {
  unsigned Opc = MI->getOpcode();

  // ── Pseudo instructions ──────────────────────────────────────────────────
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

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializePenumbraAsmPrinter() {
  RegisterAsmPrinter<PenumbraAsmPrinter> X(getThePenumbraTarget());
}
