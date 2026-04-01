//===- Penumbra.cpp -------------------------------------------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Penumbra is a 32-bit little-endian RISC minicomputer.  This file implements
// the minimal lld target support needed to link Penumbra ELF objects.
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"
#include "Target.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::support::endian;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

// Penumbra ELF relocation types (must match PenumbraELFObjectWriter.cpp).
enum {
  R_PENUMBRA_NONE = 0,
  R_PENUMBRA_32 = 1,
  R_PENUMBRA_BRANCH22 = 2,
  R_PENUMBRA_IMM16 = 3,
  R_PENUMBRA_LO16 = 4,
  R_PENUMBRA_HI16 = 5,
};

namespace {
class Penumbra final : public TargetInfo {
public:
  Penumbra(Ctx &);
  RelExpr getRelExpr(RelType type, const Symbol &s,
                     const uint8_t *loc) const override;
  void relocate(uint8_t *loc, const Relocation &rel,
                uint64_t val) const override;
};
} // namespace

Penumbra::Penumbra(Ctx &ctx) : TargetInfo(ctx) {
  // BREAK instruction = 0x2A000000
  trapInstr = {0x00, 0x00, 0x00, 0x2A};
}

RelExpr Penumbra::getRelExpr(RelType type, const Symbol &s,
                             const uint8_t *loc) const {
  switch (type) {
  case R_PENUMBRA_BRANCH22:
    return R_PC;
  default:
    return R_ABS;
  }
}

void Penumbra::relocate(uint8_t *loc, const Relocation &rel,
                        uint64_t val) const {
  switch (rel.type) {
  case R_PENUMBRA_32:
    write32le(loc, val);
    break;
  case R_PENUMBRA_BRANCH22: {
    // PC-relative 22-bit word offset in bits [25:4].
    int64_t wordOffset = static_cast<int64_t>(val) >> 2;
    uint32_t insn = read32le(loc);
    insn = (insn & ~(0x3FFFFF << 4)) |
           ((static_cast<uint32_t>(wordOffset) & 0x3FFFFF) << 4);
    write32le(loc, insn);
    break;
  }
  case R_PENUMBRA_IMM16:
  case R_PENUMBRA_LO16:
    // 16-bit immediate / low 16 bits of address, into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | (val & 0xFFFF));
    break;
  case R_PENUMBRA_HI16:
    // High 16 bits of address, into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | ((val >> 16) & 0xFFFF));
    break;
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << rel.type;
  }
}

void elf::setPenumbraTargetInfo(Ctx &ctx) {
  ctx.target.reset(new class Penumbra(ctx));
}
