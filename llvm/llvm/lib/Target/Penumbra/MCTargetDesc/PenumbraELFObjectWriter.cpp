//===-- PenumbraELFObjectWriter.cpp - Penumbra ELF object writer ----------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraMCTargetDesc.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdint>

using namespace llvm;

namespace {

// Use a private ELF machine number (official numbers require registration).
enum { EM_PENUMBRA = 0xF0DA };

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
  // Minimal stub — return 0 (R_PENUMBRA_NONE) for now.
  // Real relocation types will be added with the linker.
  return 0;
}

std::unique_ptr<MCObjectTargetWriter>
llvm::createPenumbraELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<PenumbraELFObjectWriter>(OSABI);
}
