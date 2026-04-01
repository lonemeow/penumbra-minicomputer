//===- Penumbra.cpp - Penumbra ABI implementation -------------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "ABIInfoImpl.h"
#include "TargetInfo.h"

using namespace clang;
using namespace clang::CodeGen;

namespace {
class PenumbraTargetCodeGenInfo : public TargetCodeGenInfo {
public:
  PenumbraTargetCodeGenInfo(CodeGen::CodeGenTypes &CGT)
      : TargetCodeGenInfo(std::make_unique<DefaultABIInfo>(CGT)) {}
};
} // namespace

std::unique_ptr<TargetCodeGenInfo>
CodeGen::createPenumbraTargetCodeGenInfo(CodeGenModule &CGM) {
  return std::make_unique<PenumbraTargetCodeGenInfo>(CGM.getTypes());
}
