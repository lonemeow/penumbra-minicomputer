//===-- PenumbraELFObjectWriter.cpp - Penumbra ELF object writer ----------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraFixupKinds.h"
#include "PenumbraMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdint>

using namespace llvm;

namespace {

// EM_PENUMBRA is defined in llvm/BinaryFormat/ELF.h

// ELF relocation types for Penumbra.
enum {
  R_PENUMBRA_NONE = 0,
  R_PENUMBRA_32 = 1,       // Absolute 32-bit (.word symbol)
  R_PENUMBRA_BRANCH22 = 2, // PC-relative 22-bit word offset (Format B)
  R_PENUMBRA_IMM16 = 3,    // 16-bit immediate (Format L)
  R_PENUMBRA_LO16 = 4,     // Low 16 bits of absolute address
  R_PENUMBRA_HI16 = 5,     // High 16 bits of absolute address
  R_PENUMBRA_MEMOFFSET16_PCREL = 6, // PC-relative 16-bit memory offset
  R_PENUMBRA_IMM16_PCREL = 7,      // PC-relative 16-bit immediate
  // R_PENUMBRA_RELATIVE = 8 — defined in Penumbra.def, not used here.
  R_PENUMBRA_TLS_GD_LO16 = 9,     // TLS GD: low 16 bits
  R_PENUMBRA_TLS_GD_HI16 = 10,    // TLS GD: high 16 bits
  // 11-15: GOT/PLT/TLS dynamic relocs (linker-only, not emitted by MC)
  R_PENUMBRA_TLS_GD_PCREL = 16,   // TLS GD: PC-relative to GOT entry (PIC)
};

class PenumbraELFObjectWriter : public MCELFObjectTargetWriter {
public:
  PenumbraELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_PENUMBRA,
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
  if (Kind == Penumbra::fixup_penumbra_lo16)
    return R_PENUMBRA_LO16;
  if (Kind == Penumbra::fixup_penumbra_hi16)
    return R_PENUMBRA_HI16;
  if (Kind == Penumbra::fixup_penumbra_memoffset16_pcrel)
    return R_PENUMBRA_MEMOFFSET16_PCREL;
  if (Kind == Penumbra::fixup_penumbra_imm16_pcrel)
    return R_PENUMBRA_IMM16_PCREL;
  if (Kind == Penumbra::fixup_penumbra_tls_gd_lo16)
    return R_PENUMBRA_TLS_GD_LO16;
  if (Kind == Penumbra::fixup_penumbra_tls_gd_hi16)
    return R_PENUMBRA_TLS_GD_HI16;
  if (Kind == Penumbra::fixup_penumbra_tls_gd_pcrel)
    return R_PENUMBRA_TLS_GD_PCREL;
  // Standard data fixups (FK_Data_4 from .word directives).
  if (Kind == FK_Data_4)
    return R_PENUMBRA_32;
  return R_PENUMBRA_NONE;
}

std::unique_ptr<MCObjectTargetWriter>
llvm::createPenumbraELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<PenumbraELFObjectWriter>(OSABI);
}
