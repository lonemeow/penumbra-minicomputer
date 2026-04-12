//===-- PenumbraInstPrinter.cpp - Penumbra MCInst to asm ------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraInstPrinter.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define PRINT_ALIAS_INSTR
#include "PenumbraGenAsmWriter.inc"

void PenumbraInstPrinter::printRegName(raw_ostream &OS, MCRegister Reg) {
  OS << getRegisterName(Reg);
}

void PenumbraInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                    StringRef Annot,
                                    const MCSubtargetInfo &STI,
                                    raw_ostream &O) {
  if (!printAliasInstr(MI, Address, O))
    printInstruction(MI, Address, O);
  printAnnotation(O, Annot);
}

void PenumbraInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                       raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isReg())
    O << getRegisterName(MO.getReg());
  else if (MO.isImm())
    O << MO.getImm();
  else if (MO.isExpr())
    MAI.printExpr(O, *MO.getExpr());
}

void PenumbraInstPrinter::printBranchTarget(const MCInst *MI, unsigned OpNo,
                                            raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isImm())
    O << "0x" << utohexstr(static_cast<uint64_t>(MO.getImm()));
  else if (MO.isExpr())
    MAI.printExpr(O, *MO.getExpr());
}
