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
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define PRINT_ALIAS_INSTR
#include "PenumbraGenAsmWriter.inc"

// Print physical r<N> register names instead of the semantic aliases
// (zero/tp/lr/sp/pc).  Off by default — semantic names make disassembly far
// easier to read.  Reachable from llc/llvm-mc as -penumbra-numeric-reg-names,
// and from llvm-objdump as `-M numeric` (routed via applyTargetSpecificCLOption,
// matching GNU objdump's -M convention).
static cl::opt<bool> NumericRegNames(
    "penumbra-numeric-reg-names",
    cl::desc("Print physical r<N> register names instead of semantic aliases"),
    cl::init(false), cl::Hidden);

bool PenumbraInstPrinter::applyTargetSpecificCLOption(StringRef Option) {
  if (Option == "numeric") {
    NumericRegNames = true;
    return true;
  }
  return false;
}

const char *PenumbraInstPrinter::getRegisterName(MCRegister Reg) {
  // Default to the semantic aliases (zero/tp/lr/sp/pc); SemanticRegName falls
  // back to the physical r<N> name for every other register.  NumericRegNames
  // forces the physical names everywhere.
  return getRegisterName(Reg, NumericRegNames ? Penumbra::NoRegAltName
                                              : Penumbra::SemanticRegName);
}

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
