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
#include "llvm/MC/MCSymbolELF.h"
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
  R_PENUMBRA_PC32 = 17,           // PC-relative 32-bit (.eh_frame, etc.)
  R_PENUMBRA_GOT_PCREL_LO16 = 18, // GOT PC-relative: low 16 bits
  R_PENUMBRA_GOT_PCREL_HI16 = 19, // GOT PC-relative: high 16 bits
  R_PENUMBRA_TLS_GD_GOT_PCREL_LO16 = 20, // TLS GD GOT PC-relative: low 16
  R_PENUMBRA_TLS_GD_GOT_PCREL_HI16 = 21, // TLS GD GOT PC-relative: high 16
  R_PENUMBRA_COPY = 22,                   // Copy relocation
  R_PENUMBRA_IRELATIVE = 23,              // Indirect function (IFUNC)
};

class PenumbraELFObjectWriter : public MCELFObjectTargetWriter {
public:
  PenumbraELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_PENUMBRA,
                                /*HasRelocationAddend=*/true) {}

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override;
  bool needsRelocateWithSymbol(const MCValue &, unsigned Type) const override;
};

} // anonymous namespace

unsigned PenumbraELFObjectWriter::getRelocType(const MCFixup &Fixup,
                                               const MCValue &Target,
                                               bool IsPCRel) const {
  unsigned Kind = Fixup.getKind();

  // Mark symbols referenced by TLS fixups as STT_TLS.  Without this,
  // undefined extern __thread symbols get STT_NOTYPE and lld's
  // sym.isTls() check fails, skipping TLS GOT entry allocation.
  if (Kind == Penumbra::fixup_penumbra_tls_gd_lo16 ||
      Kind == Penumbra::fixup_penumbra_tls_gd_hi16 ||
      Kind == Penumbra::fixup_penumbra_tls_gd_pcrel ||
      Kind == Penumbra::fixup_penumbra_tls_gd_got_pcrel_lo16 ||
      Kind == Penumbra::fixup_penumbra_tls_gd_got_pcrel_hi16) {
    if (auto *SA = const_cast<MCSymbol *>(Target.getAddSym()))
      static_cast<MCSymbolELF *>(SA)->setType(ELF::STT_TLS);
  }

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
  if (Kind == Penumbra::fixup_penumbra_got_pcrel_lo16)
    return R_PENUMBRA_GOT_PCREL_LO16;
  if (Kind == Penumbra::fixup_penumbra_got_pcrel_hi16)
    return R_PENUMBRA_GOT_PCREL_HI16;
  if (Kind == Penumbra::fixup_penumbra_tls_gd_got_pcrel_lo16)
    return R_PENUMBRA_TLS_GD_GOT_PCREL_LO16;
  if (Kind == Penumbra::fixup_penumbra_tls_gd_got_pcrel_hi16)
    return R_PENUMBRA_TLS_GD_GOT_PCREL_HI16;
  // Standard data fixups (FK_Data_4 from .word directives).
  if (Kind == FK_Data_4)
    return IsPCRel ? R_PENUMBRA_PC32 : R_PENUMBRA_32;
  return R_PENUMBRA_NONE;
}

bool PenumbraELFObjectWriter::needsRelocateWithSymbol(
    const MCValue &, unsigned Type) const {
  // GOT and TLS relocations must keep the symbol reference — if the
  // assembler folds local symbols to section+offset, the GV offset
  // contaminates the GOT entry address computation (the linker
  // applies addend to GOT_entry_VA, not to the loaded value).
  switch (Type) {
  case R_PENUMBRA_GOT_PCREL_LO16:
  case R_PENUMBRA_GOT_PCREL_HI16:
  case R_PENUMBRA_TLS_GD_LO16:
  case R_PENUMBRA_TLS_GD_HI16:
  case R_PENUMBRA_TLS_GD_PCREL:
  case R_PENUMBRA_TLS_GD_GOT_PCREL_LO16:
  case R_PENUMBRA_TLS_GD_GOT_PCREL_HI16:
    return true;
  default:
    return false;
  }
}

std::unique_ptr<MCObjectTargetWriter>
llvm::createPenumbraELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<PenumbraELFObjectWriter>(OSABI);
}
