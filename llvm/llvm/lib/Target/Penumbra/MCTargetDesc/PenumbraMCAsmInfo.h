//===-- PenumbraMCAsmInfo.h - Penumbra Asm Info -----------------*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAMCASMINFO_H
#define LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"

namespace llvm {

class MCSpecifierExpr;
class Triple;

class PenumbraMCAsmInfo : public MCAsmInfoELF {
public:
  explicit PenumbraMCAsmInfo(const Triple &TT);

  void printSpecifierExpr(raw_ostream &OS,
                          const MCSpecifierExpr &Expr) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAMCASMINFO_H
