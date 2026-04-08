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

namespace {
class Penumbra final : public TargetInfo {
public:
  Penumbra(Ctx &);
  RelExpr getRelExpr(RelType type, const Symbol &s,
                     const uint8_t *loc) const override;
  RelType getDynRel(RelType type) const override;
  int64_t getImplicitAddend(const uint8_t *buf,
                            RelType type) const override;
  void relocate(uint8_t *loc, const Relocation &rel,
                uint64_t val) const override;
};
} // namespace

Penumbra::Penumbra(Ctx &ctx) : TargetInfo(ctx) {
  // BREAK instruction = 0x2A000000
  trapInstr = {0x00, 0x00, 0x00, 0x2A};
  relativeRel = R_PENUMBRA_RELATIVE;
  symbolicRel = R_PENUMBRA_32;
}

RelExpr Penumbra::getRelExpr(RelType type, const Symbol &s,
                             const uint8_t *loc) const {
  switch (type) {
  case R_PENUMBRA_BRANCH22:
  case R_PENUMBRA_MEMOFFSET16_PCREL:
  case R_PENUMBRA_IMM16_PCREL:
    return R_PC;
  case R_PENUMBRA_TLS_GD_LO16:
  case R_PENUMBRA_TLS_GD_HI16:
    // For static linking, resolve GD directly as TP-relative offset
    // (implicit GD→LE relaxation).  For dynamic linking, this will need
    // to become R_TLSGD_GOT to create GOT entries.
    return R_TPREL;
  default:
    return R_ABS;
  }
}

RelType Penumbra::getDynRel(RelType type) const {
  if (type == R_PENUMBRA_32)
    return type;
  return R_PENUMBRA_NONE;
}

int64_t Penumbra::getImplicitAddend(const uint8_t *buf,
                                    RelType type) const {
  switch (type) {
  case R_PENUMBRA_32:
  case R_PENUMBRA_RELATIVE:
    return SignExtend64<32>(read32le(buf));
  case R_PENUMBRA_NONE:
    return 0;
  default:
    InternalErr(ctx, buf) << "cannot read addend for relocation " << type;
    return 0;
  }
}

void Penumbra::relocate(uint8_t *loc, const Relocation &rel,
                        uint64_t val) const {
  switch (rel.type) {
  case R_PENUMBRA_32:
  case R_PENUMBRA_RELATIVE:
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
  case R_PENUMBRA_TLS_GD_LO16:
    // 16-bit immediate / low 16 bits of address / TLS offset, into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | (val & 0xFFFF));
    break;
  case R_PENUMBRA_HI16:
  case R_PENUMBRA_TLS_GD_HI16:
    // High 16 bits of address / TLS offset, into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | ((val >> 16) & 0xFFFF));
    break;
  case R_PENUMBRA_MEMOFFSET16_PCREL: {
    // PC-relative 16-bit offset, into bits [17:2] (Format M).
    uint32_t insn = read32le(loc);
    insn = (insn & ~(0xFFFF << 2)) | ((static_cast<uint32_t>(val) & 0xFFFF) << 2);
    write32le(loc, insn);
    break;
  }
  case R_PENUMBRA_IMM16_PCREL: {
    // PC-relative 16-bit immediate, into bits [15:0] (Format L).
    // The instruction is ADDi (INC, opcode 0011) which zero-extends.
    // If the offset is negative, flip to SUBi (DEC, opcode 0100) and
    // negate the value so the unsigned immediate works correctly.
    uint32_t insn = read32le(loc);
    int64_t sval = static_cast<int64_t>(val);
    if (sval < 0) {
      // Change ADDi (op=0011) to SUBi (op=0100) in bits [29:26]
      insn = (insn & ~(0xFu << 26)) | (0x4u << 26);
      sval = -sval;
    }
    insn = (insn & 0xFFFF0000) | (static_cast<uint32_t>(sval) & 0xFFFF);
    write32le(loc, insn);
    break;
  }
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << rel.type;
  }
}

void elf::setPenumbraTargetInfo(Ctx &ctx) {
  ctx.target.reset(new class Penumbra(ctx));
}
