//===-- PenumbraELFObjectWriter.cpp - Penumbra ELF object writer ----------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraFixupKinds.h"
#include "PenumbraMCTargetDesc.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdint>

using namespace llvm;

namespace {

// Use a private ELF machine number (official numbers require registration).
enum { EM_PENUMBRA = 0xF0DA };

// ELF relocation types for Penumbra.
enum {
  R_PENUMBRA_NONE = 0,
  R_PENUMBRA_32 = 1,       // Absolute 32-bit (.word symbol)
  R_PENUMBRA_BRANCH22 = 2, // PC-relative 22-bit word offset (Format B)
  R_PENUMBRA_IMM16 = 3,    // 16-bit immediate (Format L)
};

class PenumbraELFObjectWriter : public MCELFObjectTargetWriter {
public:
  PenumbraELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, EM_PENUMBRA,
                                /*HasRelocationAddend=*/true) {}

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override;
};

} // anonymous namespace

unsigned PenumbraELFObjectWriter::getRelocType(const MCFixup &Fixup,
                                               const MCValue &Target,
                                               bool IsPCRel) const {
  unsigned Kind = Fixup.getKind();
  if (Kind == Penumbra::fixup_penumbra_branch22)
    return R_PENUMBRA_BRANCH22;
  if (Kind == Penumbra::fixup_penumbra_imm16)
    return R_PENUMBRA_IMM16;
  // Standard data fixups (FK_Data_4 from .word directives).
  if (Kind == FK_Data_4)
    return R_PENUMBRA_32;
  return R_PENUMBRA_NONE;
}

std::unique_ptr<MCObjectTargetWriter>
llvm::createPenumbraELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<PenumbraELFObjectWriter>(OSABI);
}
