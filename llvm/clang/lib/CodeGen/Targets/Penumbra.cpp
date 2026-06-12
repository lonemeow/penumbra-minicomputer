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

// Implements the slot-based aggregate convention of doc/system/abi.md
// ("Argument Passing" / "Return Values"): aggregates classify by size
// alone into one 4-byte slot, two slots, or a pointer to a
// caller-owned copy; returns mirror the same classes, with larger
// aggregates returned through a hidden sret pointer.
//
// Indirect aggregates are non-byval: clang materializes the temporary
// copy in IR at the call site, so the backend never has to synthesize
// a copy and sees only ordinary pointer arguments.
class PenumbraABIInfo : public DefaultABIInfo {
public:
  PenumbraABIInfo(CodeGen::CodeGenTypes &CGT) : DefaultABIInfo(CGT) {}

  ABIArgInfo classifyAggregate(QualType Ty) const;
  ABIArgInfo classifyArgumentType(QualType Ty) const;
  ABIArgInfo classifyReturnType(QualType RetTy) const;

  void computeInfo(CGFunctionInfo &FI) const override;

  RValue EmitVAArg(CodeGenFunction &CGF, Address VAListAddr, QualType Ty,
                   AggValueSlot Slot) const override;
};

// Classify an aggregate (struct or union) for argument passing and
// value return.  This function is the single encoding of the size
// classes in doc/system/abi.md "Argument Passing"; argument and
// return classification and va_arg's direct-vs-indirect decision all
// derive from it.
ABIArgInfo PenumbraABIInfo::classifyAggregate(QualType Ty) const {
  uint64_t Bits = getContext().getTypeSize(Ty);
  if (Bits == 0)
    return ABIArgInfo::getIgnore();
  else if (Bits <= 32)
    return ABIArgInfo::getDirect(llvm::Type::getInt32Ty(getVMContext()));
  else if (Bits <= 64)
    return ABIArgInfo::getDirect(
        llvm::ArrayType::get(llvm::Type::getInt32Ty(getVMContext()), 2));
  else
    return getNaturalAlignIndirect(Ty, getDataLayout().getAllocaAddrSpace(),
                                   /*ByVal=*/false);
}

ABIArgInfo PenumbraABIInfo::classifyArgumentType(QualType Ty) const {
  Ty = useFirstFieldIfTransparentUnion(Ty);

  if (isAggregateTypeForABI(Ty)) {
    // C++ records with non-trivial copy/move/destroy semantics cannot
    // be copied byte-wise; they always go indirect, regardless of size.
    if (CGCXXABI::RecordArgABI RAA = getRecordArgABI(Ty, getCXXABI()))
      return getNaturalAlignIndirect(Ty, getDataLayout().getAllocaAddrSpace(),
                                     RAA == CGCXXABI::RAA_DirectInMemory);
    return classifyAggregate(Ty);
  }

  return DefaultABIInfo::classifyArgumentType(Ty);
}

ABIArgInfo PenumbraABIInfo::classifyReturnType(QualType RetTy) const {
  if (RetTy->isVoidType())
    return ABIArgInfo::getIgnore();

  // Non-trivial C++ returns are forced indirect by the CXXABI hook in
  // computeInfo before this runs; aggregates reaching here are
  // byte-wise copyable.
  if (isAggregateTypeForABI(RetTy))
    return classifyAggregate(RetTy);

  return DefaultABIInfo::classifyReturnType(RetTy);
}

void PenumbraABIInfo::computeInfo(CGFunctionInfo &FI) const {
  if (!getCXXABI().classifyReturnType(FI))
    FI.getReturnInfo() = classifyReturnType(FI.getReturnType());
  for (auto &I : FI.arguments())
    I.info = classifyArgumentType(I.type);
}

RValue PenumbraABIInfo::EmitVAArg(CodeGenFunction &CGF, Address VAListAddr,
                                  QualType Ty, AggValueSlot Slot) const {
  // The va_list walks 4-byte slots; whether a slot holds the value or
  // a pointer to it follows the argument classification, so the size
  // classes live in exactly one place.
  bool IsIndirect = classifyArgumentType(Ty).isIndirect();
  return emitVoidPtrVAArg(CGF, VAListAddr, Ty, IsIndirect,
                          getContext().getTypeInfoInChars(Ty),
                          CharUnits::fromQuantity(4),
                          /*AllowHigherAlign=*/false, Slot);
}

class PenumbraTargetCodeGenInfo : public TargetCodeGenInfo {
public:
  PenumbraTargetCodeGenInfo(CodeGen::CodeGenTypes &CGT)
      : TargetCodeGenInfo(std::make_unique<PenumbraABIInfo>(CGT)) {}
};

} // namespace

std::unique_ptr<TargetCodeGenInfo>
CodeGen::createPenumbraTargetCodeGenInfo(CodeGenModule &CGM) {
  return std::make_unique<PenumbraTargetCodeGenInfo>(CGM.getTypes());
}
