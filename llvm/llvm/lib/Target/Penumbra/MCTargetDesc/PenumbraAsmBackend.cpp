//===-- PenumbraAsmBackend.cpp - Penumbra assembler backend ---------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraFixupKinds.h"
#include "PenumbraMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <optional>

using namespace llvm;

namespace {

class PenumbraAsmBackend : public MCAsmBackend {
  Triple::OSType OSType;

public:
  PenumbraAsmBackend(const Target &T, Triple::OSType OST)
      : MCAsmBackend(llvm::endianness::little), OSType(OST) {}

  void applyFixup(const MCFragment &, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  std::optional<MCFixupKind> getFixupKind(StringRef Name) const override;

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override;

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;
};

} // anonymous namespace

std::optional<MCFixupKind>
PenumbraAsmBackend::getFixupKind(StringRef Name) const {
  return std::nullopt;
}

MCFixupKindInfo
PenumbraAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  // {Name, BitOffset, BitSize, Flags}
  // BitOffset/BitSize are in the *target byte order* instruction word.
  // Penumbra is little-endian: bit 0 = LSB of byte 0.
  static const MCFixupKindInfo Infos[Penumbra::NumTargetFixupKinds -
                                     FirstTargetFixupKind] = {
      // branch22: bits [25:4] of the 32-bit word = bit offset 4, size 22
      {"fixup_penumbra_branch22", 4, 22, 0},
      // imm16: bits [15:0] of the 32-bit word = bit offset 0, size 16
      {"fixup_penumbra_imm16", 0, 16, 0},
      // lo16: low 16 bits of absolute address, into bits [15:0]
      {"fixup_penumbra_lo16", 0, 16, 0},
      // hi16: high 16 bits of absolute address, into bits [15:0]
      {"fixup_penumbra_hi16", 0, 16, 0},
  };

  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);

  assert(unsigned(Kind - FirstTargetFixupKind) < Penumbra::NumTargetFixupKinds -
                                                     FirstTargetFixupKind);
  return Infos[Kind - FirstTargetFixupKind];
}

void PenumbraAsmBackend::applyFixup(const MCFragment &, const MCFixup &Fixup,
                                    const MCValue &Target, uint8_t *Data,
                                    uint64_t Value, bool IsResolved) {
  if (!Value)
    return; // Nothing to patch.

  // NOTE: Data already points to the fixup location (Contents + Offset),
  // so we access Data[0..3] directly — do NOT add Fixup.getOffset() again.
  MCFixupKind Kind = Fixup.getKind();

  if (Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_branch22)) {
    // Value is a PC-relative byte offset.  Convert to word offset.
    int64_t WordOffset = static_cast<int64_t>(Value) >> 2;
    // Mask to 22 bits and shift into position [25:4].
    uint32_t Encoded = (static_cast<uint32_t>(WordOffset) & 0x3FFFFF) << 4;
    // OR into the little-endian instruction word.
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  if (Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_imm16)) {
    // Value is a 16-bit immediate, goes into bits [15:0].
    uint32_t Encoded = static_cast<uint32_t>(Value) & 0xFFFF;
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  if (Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_lo16)) {
    // Low 16 bits of absolute address, into bits [15:0].
    uint32_t Encoded = static_cast<uint32_t>(Value) & 0xFFFF;
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  if (Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_hi16)) {
    // High 16 bits of absolute address, into bits [15:0].
    uint32_t Encoded = (static_cast<uint32_t>(Value) >> 16) & 0xFFFF;
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  // Standard LLVM fixups (e.g. FK_Data_4 for .word references).
  for (unsigned i = 0; i < 4; ++i) {
    Data[i] |= uint8_t(Value & 0xFF);
    Value >>= 8;
  }
}

std::unique_ptr<MCObjectTargetWriter>
PenumbraAsmBackend::createObjectTargetWriter() const {
  uint8_t OSABI = MCELFObjectTargetWriter::getOSABI(OSType);
  return createPenumbraELFObjectWriter(OSABI);
}

bool PenumbraAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                      const MCSubtargetInfo *STI) const {
  // NOP = ADD R0, R0 = 0x00000000
  while (Count >= 4) {
    OS.write("\0\0\0\0", 4);
    Count -= 4;
  }
  return Count == 0;
}

MCAsmBackend *llvm::createPenumbraAsmBackend(const Target &T,
                                             const MCSubtargetInfo &STI,
                                             const MCRegisterInfo &MRI,
                                             const MCTargetOptions &Options) {
  return new PenumbraAsmBackend(T, STI.getTargetTriple().getOS());
}
