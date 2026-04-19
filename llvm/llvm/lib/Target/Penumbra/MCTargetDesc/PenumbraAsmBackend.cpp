//===-- PenumbraAsmBackend.cpp - Penumbra assembler backend ---------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraFixupKinds.h"
#include "PenumbraMCTargetDesc.h"
#include "llvm/ADT/SmallString.h"
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

  std::optional<bool> evaluateFixup(const MCFragment &, MCFixup &,
                                    MCValue &, uint64_t &) override;

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

std::optional<bool>
PenumbraAsmBackend::evaluateFixup(const MCFragment &, MCFixup &Fixup,
                                  MCValue &, uint64_t &) {
  // GOT-related fixups must never be resolved at assembly time — the GOT
  // entry doesn't exist until the linker creates it.  Force a relocation
  // so the linker sees R_PENUMBRA_GOT_PCREL_* and allocates a GOT slot.
  // Without this, same-section local symbols (e.g. static functions)
  // get their PC-relative offset resolved by the assembler, but the
  // code still does an LDW dereference expecting a GOT entry.
  switch (static_cast<unsigned>(Fixup.getKind())) {
  case Penumbra::fixup_penumbra_got_pcrel_lo16:
  case Penumbra::fixup_penumbra_got_pcrel_hi16:
  case Penumbra::fixup_penumbra_tls_gd_got_pcrel_lo16:
  case Penumbra::fixup_penumbra_tls_gd_got_pcrel_hi16:
    return false; // Never resolved — always emit a relocation.
  default:
    return {};    // Use default evaluation for all other fixups.
  }
}

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
      // memoffset16: bits [17:2] of the 32-bit word = bit offset 2, size 16
      {"fixup_penumbra_memoffset16", 2, 16, 0},
      // lo16: low 16 bits of absolute address, into bits [15:0]
      {"fixup_penumbra_lo16", 0, 16, 0},
      // hi16: high 16 bits of absolute address, into bits [15:0]
      {"fixup_penumbra_hi16", 0, 16, 0},
      // memoffset16_pcrel: PC-relative 16-bit offset, into bits [17:2]
      {"fixup_penumbra_memoffset16_pcrel", 2, 16, 0},
      // imm16_pcrel: PC-relative 16-bit immediate, into bits [15:0]
      {"fixup_penumbra_imm16_pcrel", 0, 16, 0},
      // tls_gd_lo16: TLS GD low 16 bits, into bits [15:0]
      {"fixup_penumbra_tls_gd_lo16", 0, 16, 0},
      // tls_gd_hi16: TLS GD high 16 bits, into bits [15:0]
      {"fixup_penumbra_tls_gd_hi16", 0, 16, 0},
      // tls_gd_pcrel: TLS GD PC-relative, into bits [15:0]
      {"fixup_penumbra_tls_gd_pcrel", 0, 16, 0},
      // got_pcrel_lo16: GOT PC-relative low 16, into bits [15:0]
      {"fixup_penumbra_got_pcrel_lo16", 0, 16, 0},
      // got_pcrel_hi16: GOT PC-relative high 16, into bits [15:0]
      {"fixup_penumbra_got_pcrel_hi16", 0, 16, 0},
      // tls_gd_got_pcrel_lo16: TLS GD GOT PC-relative low 16, into bits [15:0]
      {"fixup_penumbra_tls_gd_got_pcrel_lo16", 0, 16, 0},
      // tls_gd_got_pcrel_hi16: TLS GD GOT PC-relative high 16, into bits [15:0]
      {"fixup_penumbra_tls_gd_got_pcrel_hi16", 0, 16, 0},
  };

  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);

  assert(unsigned(Kind - FirstTargetFixupKind) < Penumbra::NumTargetFixupKinds -
                                                     FirstTargetFixupKind);
  return Infos[Kind - FirstTargetFixupKind];
}

// Range-check a fixup value and return the low `Bits` of it.  Hard-aborts
// on overflow when `Check` is true (only resolved fixups are checked —
// unresolved fixups carry a linker placeholder that's meaningless to check).
// Doing the range check and the low-bits mask in the same helper keeps them
// tied together: you can't accidentally skip the check while still producing
// an encoded value.
static uint32_t checkedMask(const MCFixup &Fixup, int64_t Value, int64_t Min,
                            int64_t Max, unsigned Bits, bool Check,
                            const char *Name) {
  if (Check && (Value < Min || Value > Max)) {
    SmallString<128> Msg;
    raw_svector_ostream OS(Msg);
    OS << "fixup value " << Value << " out of range [" << Min << ", " << Max
       << "] for " << Name;
    report_fatal_error(OS.str().str().c_str());
  }
  uint32_t Mask = (Bits >= 32) ? 0xFFFFFFFFu : ((1u << Bits) - 1);
  return static_cast<uint32_t>(Value) & Mask;
}

// Signed `Bits`-wide field, e.g. 16-bit signed memory offset.
static uint32_t checkedSigned(const MCFixup &Fixup, int64_t Value,
                              unsigned Bits, bool Check, const char *Name) {
  int64_t Min = -(int64_t(1) << (Bits - 1));
  int64_t Max = (int64_t(1) << (Bits - 1)) - 1;
  return checkedMask(Fixup, Value, Min, Max, Bits, Check, Name);
}

// Format L imm16: the field is 16 bits but source instructions interpret it
// as either signed (LLIS) or unsigned (LLI/ADDi/...), so accept the union
// [-32768, 65535].
static uint32_t checkedMixed16(const MCFixup &Fixup, int64_t Value, bool Check,
                               const char *Name) {
  return checkedMask(Fixup, Value, -(int64_t(1) << 15),
                     (int64_t(1) << 16) - 1, 16, Check, Name);
}

void PenumbraAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                    const MCValue &Target, uint8_t *Data,
                                    uint64_t Value, bool IsResolved) {
  maybeAddReloc(F, Fixup, Target, Value, IsResolved);
  if (!Value)
    return; // Nothing to patch.

  // Only validate when the fixup is fully resolved at assembly time.
  // Unresolved fixups carry a placeholder Value that the linker will
  // overwrite — range-checking the placeholder is meaningless.
  bool CheckRange = IsResolved;

  // NOTE: Data already points to the fixup location (Contents + Offset),
  // so we access Data[0..3] directly — do NOT add Fixup.getOffset() again.
  MCFixupKind Kind = Fixup.getKind();

  if (Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_branch22)) {
    // Value is a PC-relative byte offset.  Convert to word offset.
    int64_t ByteOffset = static_cast<int64_t>(Value);
    if (CheckRange && (ByteOffset & 3) != 0)
      report_fatal_error("branch target is not 4-byte aligned");
    // 22-bit signed word offset: ±8 MB; then shift into position [25:4].
    uint32_t Encoded = checkedSigned(Fixup, ByteOffset >> 2, 22, CheckRange,
                                     "branch22")
                       << 4;
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  if (Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_imm16)) {
    // 16-bit immediate (signed LLIS or unsigned LLI) into bits [15:0].
    uint32_t Encoded = checkedMixed16(Fixup, Value, CheckRange, "imm16");
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  if (Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_memoffset16)) {
    // 16-bit signed memory offset, into bits [17:2].
    uint32_t Encoded = checkedSigned(Fixup, Value, 16, CheckRange,
                                     "memoffset16")
                       << 2;
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  if (Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_lo16) ||
      Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_lo16)) {
    // Low 16 bits into bits [15:0] (absolute address or TLS GD offset).
    uint32_t Encoded = static_cast<uint32_t>(Value) & 0xFFFF;
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  if (Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_hi16) ||
      Kind == static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_hi16)) {
    // High 16 bits into bits [15:0] (absolute address or TLS GD offset).
    uint32_t Encoded = (static_cast<uint32_t>(Value) >> 16) & 0xFFFF;
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  if (Kind ==
      static_cast<MCFixupKind>(Penumbra::fixup_penumbra_memoffset16_pcrel)) {
    // PC-relative 16-bit signed offset, into bits [17:2].
    uint32_t Encoded = checkedSigned(Fixup, Value, 16, CheckRange,
                                     "memoffset16_pcrel")
                       << 2;
    support::endian::write32le(
        Data, support::endian::read32le(Data) | Encoded);
    return;
  }

  if (Kind ==
          static_cast<MCFixupKind>(Penumbra::fixup_penumbra_imm16_pcrel) ||
      Kind ==
          static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_pcrel)) {
    // PC-relative 16-bit immediate, into bits [15:0] (Format L).
    // ADDi (INC) zero-extends the immediate, so negative offsets don't
    // work.  Flip to SUBi (DEC) and negate the value when negative.
    // Both ADDi and SUBi take a uimm16, so the pre-flip range is the
    // symmetric [-65535, 65535]: any |V| that fits in 16 bits can be
    // reached in one instruction via the right opcode choice.
    int64_t SVal = static_cast<int64_t>(Value);
    uint32_t Insn = support::endian::read32le(Data);
    if (SVal < 0) {
      // Change ADDi (op=0011) to SUBi (op=0100) in bits [29:26]
      Insn = (Insn & ~(0xFu << 26)) | (0x4u << 26);
      SVal = -SVal;
    }
    // Post-flip value must fit in uimm16.
    uint32_t Encoded = checkedMask(Fixup, SVal, 0, (int64_t(1) << 16) - 1,
                                   16, CheckRange, "imm16_pcrel");
    Insn = (Insn & 0xFFFF0000) | Encoded;
    support::endian::write32le(Data, Insn);
    return;
  }

  if (Kind ==
          static_cast<MCFixupKind>(Penumbra::fixup_penumbra_got_pcrel_lo16)) {
    // GOT PC-relative low 16 bits, into bits [15:0].
    // Always emitted as a relocation (linker computes GOT entry address).
    uint32_t Encoded = static_cast<uint32_t>(Value) & 0xFFFF;
    support::endian::write32le(
        Data, (support::endian::read32le(Data) & 0xFFFF0000) | Encoded);
    return;
  }

  if (Kind ==
          static_cast<MCFixupKind>(Penumbra::fixup_penumbra_got_pcrel_hi16) ||
      Kind ==
          static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_got_pcrel_hi16)) {
    // GOT/TLS-GD GOT PC-relative high 16 bits, into bits [15:0].
    uint32_t Encoded = (static_cast<uint32_t>(Value) >> 16) & 0xFFFF;
    support::endian::write32le(
        Data, (support::endian::read32le(Data) & 0xFFFF0000) | Encoded);
    return;
  }

  if (Kind ==
          static_cast<MCFixupKind>(Penumbra::fixup_penumbra_tls_gd_got_pcrel_lo16)) {
    // TLS GD GOT PC-relative low 16 bits, into bits [15:0].
    uint32_t Encoded = static_cast<uint32_t>(Value) & 0xFFFF;
    support::endian::write32le(
        Data, (support::endian::read32le(Data) & 0xFFFF0000) | Encoded);
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
