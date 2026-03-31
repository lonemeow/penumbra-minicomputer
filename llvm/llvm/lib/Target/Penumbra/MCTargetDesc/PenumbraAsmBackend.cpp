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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>

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

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override;

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;
};

} // anonymous namespace

void PenumbraAsmBackend::applyFixup(const MCFragment &, const MCFixup &Fixup,
                                    const MCValue &Target, uint8_t *Data,
                                    uint64_t Value, bool IsResolved) {
  // Minimal: only handle standard fixups for now.
  // Target-specific fixups (branch22, imm16) will be added when needed.
  unsigned Size = 4;
  if (Fixup.getKind() >= FirstTargetFixupKind)
    return; // TODO: implement target fixup application

  for (unsigned i = 0; i < Size; ++i) {
    Data[Fixup.getOffset() + i] |= uint8_t(Value & 0xFF);
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
