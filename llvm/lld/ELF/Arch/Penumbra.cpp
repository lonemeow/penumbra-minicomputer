//===- Penumbra.cpp -------------------------------------------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Penumbra is a 32-bit little-endian RISC minicomputer.  This file implements
// the lld target support needed to link Penumbra ELF objects, including
// GOT/PLT for shared libraries and TLS (General-Dynamic model).
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"
#include "SyntheticSections.h"
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
  uint32_t getThunkSectionSpacing() const override;
  bool needsThunk(RelExpr expr, RelType type, const InputFile *file,
                  uint64_t branchAddr, const Symbol &s,
                  int64_t a) const override;
  bool inBranchRange(RelType type, uint64_t src,
                     uint64_t dst) const override;
  void relocate(uint8_t *loc, const Relocation &rel,
                uint64_t val) const override;
  void writePltHeader(uint8_t *buf) const override;
  void writePlt(uint8_t *buf, const Symbol &sym,
                uint64_t pltEntryAddr) const override;
  void writeGotPlt(uint8_t *buf, const Symbol &s) const override;
  RelExpr adjustTlsExpr(RelType type, RelExpr expr) const override;
};
} // namespace

// Instruction encodings (little-endian):
//   LLI R11, imm16:          0x42C00000 | (imm16 & 0xFFFF)
//   LUI R11, imm16:          0x4AC00000 | (imm16 & 0xFFFF)
//   ADD R11, PC (R15):       0x017E0000
//   LDW R11, [R11 + off16]:  0xB2EC0000 | (off16 & 0xFFFF)
//   JMP R11:                 0x6EC00000
// R11 is the ABI scratch register — not callee-saved, safe to clobber.
static constexpr uint32_t LLI_R11 = 0x42C00000;
static constexpr uint32_t LUI_R11 = 0x4AC00000;
static constexpr uint32_t ADD_R11_PC = 0x017E0000;
static constexpr uint32_t LDW_R11_R11 = 0xB2EC0000;
static constexpr uint32_t JMP_R11 = 0x6EC00000;

Penumbra::Penumbra(Ctx &ctx) : TargetInfo(ctx) {
  // BREAK instruction = 0x2A000000
  trapInstr = {0x00, 0x00, 0x00, 0x2A};
  relativeRel = R_PENUMBRA_RELATIVE;
  symbolicRel = R_PENUMBRA_32;

  // GOT/PLT
  gotRel = R_PENUMBRA_GLOB_DAT;
  pltRel = R_PENUMBRA_JUMP_SLOT;
  // Position-independent output needs the PC-relative stub's extra
  // instruction; fixed-address output uses the absolute four-instruction
  // form.
  pltHeaderSize = ctx.arg.isPic ? 20 : 16;
  pltEntrySize = pltHeaderSize;
  ipltEntrySize = pltHeaderSize;

  needsThunks = true;
  copyRel = R_PENUMBRA_COPY;
  iRelativeRel = R_PENUMBRA_IRELATIVE;

  // TLS
  tlsGotRel = R_PENUMBRA_TLS_TPOFF32;
  tlsModuleIndexRel = R_PENUMBRA_TLS_DTPMOD32;
  tlsOffsetRel = R_PENUMBRA_TLS_DTPOFF32;
}

RelExpr Penumbra::getRelExpr(RelType type, const Symbol &s,
                             const uint8_t *loc) const {
  switch (type) {
  case R_PENUMBRA_PC32:
    return R_PC;
  case R_PENUMBRA_BRANCH22:
  case R_PENUMBRA_MEMOFFSET16_PCREL:
  case R_PENUMBRA_IMM16_PCREL:
    return R_PLT_PC;
  case R_PENUMBRA_TLS_GD_LO16:
  case R_PENUMBRA_TLS_GD_HI16:
    // Absolute TLS GD — used by non-PIC code. For static linking,
    // lld relaxes GD→LE automatically (R_TPREL fallback).
    return R_TLSGD_PC;
  case R_PENUMBRA_TLS_GD_PCREL:
    // PIC TLS GD — PC-relative offset to GOT tls_index entry.
    return R_TLSGD_PC;
  case R_PENUMBRA_GOT_PCREL_LO16:
  case R_PENUMBRA_GOT_PCREL_HI16:
    // GOT PC-relative — PC-relative offset to GOT entry for symbol.
    // lld allocates a GOT entry and computes (GOT[sym] - P + A).
    return R_GOT_PC;
  case R_PENUMBRA_PCREL_LO16:
  case R_PENUMBRA_PCREL_HI16:
    // PC-relative to symbol — non-preemptible globals.  Resolves to
    // (sym - P + A) directly: no GOT entry, and no dynamic relocation,
    // since a PC-relative distance within an object is load-invariant.
    return R_PC;
  case R_PENUMBRA_TLS_GD_GOT_PCREL_LO16:
  case R_PENUMBRA_TLS_GD_GOT_PCREL_HI16:
    // TLS GD GOT PC-relative — PC-relative offset to GOT tls_index pair.
    // lld allocates a TLS GD GOT pair and computes (GOT[sym] - P + A).
    return R_TLSGD_PC;
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
  case R_PENUMBRA_PC32:
  case R_PENUMBRA_RELATIVE:
  case R_PENUMBRA_GLOB_DAT:
  case R_PENUMBRA_JUMP_SLOT:
  case R_PENUMBRA_TLS_TPOFF32:
  case R_PENUMBRA_TLS_DTPMOD32:
  case R_PENUMBRA_TLS_DTPOFF32:
  case R_PENUMBRA_COPY:
  case R_PENUMBRA_IRELATIVE:
    return SignExtend64<32>(read32le(buf));
  case R_PENUMBRA_NONE:
    return 0;
  default:
    InternalErr(ctx, buf) << "cannot read addend for relocation " << type;
    return 0;
  }
}

void Penumbra::writeGotPlt(uint8_t *buf, const Symbol &s) const {
  // RELA: addend is in the relocation entry, not in-place. Write 0.
  write32le(buf, 0);
}

// Encode LDW memory offset: field is at instruction bits [17:2],
// so the byte offset is shifted left by 2 in the encoding.
static uint32_t encodeLdwOffset(int16_t byteOffset) {
  return ((static_cast<uint32_t>(byteOffset) & 0xFFFF) << 2) & 0x3FFFC;
}

// PLT stub: load the resolved function address from the .got.plt slot
// and tail-jump through R11, with full 32-bit reach to a slot at any
// distance.  Two forms, chosen per link:
//
// Fixed-address output addresses the slot absolutely — four
// instructions, the minimum this ISA's immediates allow:
//   LLI  R11, lo16(slot)
//   LUI  R11, hi16(slot)       ; LUI keeps the low half
//   LDW  R11, [R11 + 0]
//   JMP  R11
//
// Position-independent output cannot know the slot address, so the
// stub builds the displacement from the ADD's own address (which is
// what ADD Rd, PC reads), one instruction longer — the cost of the
// ISA having no add-upper-to-PC form:
//   LLI  R11, lo16(disp)
//   LUI  R11, hi16(disp)
//   ADD  R11, PC               ; R11 = slot address
//   LDW  R11, [R11 + 0]
//   JMP  R11
static void writePltStub(Ctx &ctx, uint8_t *buf, uint64_t slotAddr,
                         uint64_t stubAddr) {
  if (ctx.arg.isPic) {
    uint32_t disp = static_cast<uint32_t>(slotAddr - (stubAddr + 8));
    write32le(buf + 0, LLI_R11 | (disp & 0xFFFF));
    write32le(buf + 4, LUI_R11 | (disp >> 16));
    write32le(buf + 8, ADD_R11_PC);
    write32le(buf + 12, LDW_R11_R11 | encodeLdwOffset(0));
    write32le(buf + 16, JMP_R11);
  } else {
    uint32_t addr = static_cast<uint32_t>(slotAddr);
    write32le(buf + 0, LLI_R11 | (addr & 0xFFFF));
    write32le(buf + 4, LUI_R11 | (addr >> 16));
    write32le(buf + 8, LDW_R11_R11 | encodeLdwOffset(0));
    write32le(buf + 12, JMP_R11);
  }
}

// PLT header: resolver stub — loads GOT[2] (resolver address) and jumps.
void Penumbra::writePltHeader(uint8_t *buf) const {
  writePltStub(ctx, buf, ctx.in.gotPlt->getVA() + 8, ctx.in.plt->getVA());
}

void Penumbra::writePlt(uint8_t *buf, const Symbol &sym,
                        uint64_t pltEntryAddr) const {
  writePltStub(ctx, buf, sym.getGotPltVA(ctx), pltEntryAddr);
}

RelExpr Penumbra::adjustTlsExpr(RelType type, RelExpr expr) const {
  return expr;
}

uint32_t Penumbra::getThunkSectionSpacing() const {
  // Branch range is ±8 MiB (22-bit signed word offset).
  // Subtract margin for thunk section overhead.
  return (8 * 1024 * 1024) - 0x30000;
}

bool Penumbra::needsThunk(RelExpr expr, RelType type, const InputFile *file,
                          uint64_t branchAddr, const Symbol &s,
                          int64_t a) const {
  if (type != R_PENUMBRA_BRANCH22)
    return false;
  uint64_t dst = expr == R_PLT_PC ? s.getPltVA(ctx) : s.getVA(ctx, a);
  return !inBranchRange(type, branchAddr, dst);
}

bool Penumbra::inBranchRange(RelType type, uint64_t src, uint64_t dst) const {
  if (type != R_PENUMBRA_BRANCH22)
    return true;
  // 22-bit signed word offset: ±8 MB.
  uint64_t range = 8 * 1024 * 1024;
  if (dst > src) {
    range -= 4;
    return dst - src <= range;
  }
  return src - dst <= range;
}

void Penumbra::relocate(uint8_t *loc, const Relocation &rel,
                        uint64_t val) const {
  switch (rel.type) {
  case R_PENUMBRA_32:
  case R_PENUMBRA_PC32:
  case R_PENUMBRA_RELATIVE:
  case R_PENUMBRA_GLOB_DAT:
  case R_PENUMBRA_JUMP_SLOT:
  case R_PENUMBRA_TLS_TPOFF32:
  case R_PENUMBRA_TLS_DTPMOD32:
  case R_PENUMBRA_TLS_DTPOFF32:
  case R_PENUMBRA_COPY:
  case R_PENUMBRA_IRELATIVE:
    write32le(loc, val);
    break;
  case R_PENUMBRA_BRANCH22: {
    // PC-relative 22-bit word offset in bits [25:4].
    int64_t wordOffset = static_cast<int64_t>(val) >> 2;
    if (wordOffset < -(1 << 21) || wordOffset >= (1 << 21))
      Err(ctx) << getErrorLoc(ctx, loc)
               << "branch relocation out of range: " << val
               << " (signed: " << static_cast<int64_t>(val) << ")"
               << " to '" << rel.sym->getName() << "'"
               << " is not in [-8388608, 8388604]";
    uint32_t insn = read32le(loc);
    insn = (insn & ~(0x3FFFFF << 4)) |
           ((static_cast<uint32_t>(wordOffset) & 0x3FFFFF) << 4);
    write32le(loc, insn);
    break;
  }
  case R_PENUMBRA_IMM16:
  case R_PENUMBRA_LO16:
  case R_PENUMBRA_TLS_GD_LO16:
    // 16-bit immediate / low 16 bits, into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | (val & 0xFFFF));
    break;
  case R_PENUMBRA_HI16:
  case R_PENUMBRA_TLS_GD_HI16:
    // High 16 bits, into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | ((val >> 16) & 0xFFFF));
    break;
  case R_PENUMBRA_GOT_PCREL_LO16:
    // GOT PC-relative: low 16 bits of (GOT[sym] - P + A), into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | (val & 0xFFFF));
    break;
  case R_PENUMBRA_GOT_PCREL_HI16:
  case R_PENUMBRA_TLS_GD_GOT_PCREL_HI16:
    // GOT/TLS-GD GOT PC-relative: high 16 bits, into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | ((val >> 16) & 0xFFFF));
    break;
  case R_PENUMBRA_TLS_GD_GOT_PCREL_LO16:
    // TLS GD GOT PC-relative: low 16 bits, into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | (val & 0xFFFF));
    break;
  case R_PENUMBRA_PCREL_LO16:
    // PC-relative to symbol: low 16 bits of (sym - P + A), into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | (val & 0xFFFF));
    break;
  case R_PENUMBRA_PCREL_HI16:
    // PC-relative to symbol: high 16 bits, into bits [15:0].
    write32le(loc, (read32le(loc) & 0xFFFF0000) | ((val >> 16) & 0xFFFF));
    break;
  case R_PENUMBRA_MEMOFFSET16_PCREL: {
    // PC-relative 16-bit signed offset, into bits [17:2] (Format M).
    int64_t sval = static_cast<int64_t>(val);
    if (sval < -32768 || sval > 32767)
      Err(ctx) << getErrorLoc(ctx, loc)
               << "PC-relative memory offset out of range: " << val
               << " is not in [-32768, 32767]";
    uint32_t insn = read32le(loc);
    insn = (insn & ~(0xFFFF << 2)) |
           ((static_cast<uint32_t>(val) & 0xFFFF) << 2);
    write32le(loc, insn);
    break;
  }
  case R_PENUMBRA_IMM16_PCREL:
  case R_PENUMBRA_TLS_GD_PCREL: {
    // PC-relative 16-bit immediate, into bits [15:0] (Format L).
    // ADDi (INC) zero-extends, so negative offsets need SUBi (DEC).
    uint32_t insn = read32le(loc);
    int64_t sval = static_cast<int64_t>(val);
    if (sval < 0) {
      // Change ADDi (op=0011) to SUBi (op=0100) in bits [29:26]
      insn = (insn & ~(0xFu << 26)) | (0x4u << 26);
      sval = -sval;
    }
    if (sval > 0xFFFF)
      Err(ctx) << getErrorLoc(ctx, loc)
               << "PC-relative relocation out of range: " << val
               << " is not in [-65535, 65535]; recompile with -fno-PIC";
    insn = (insn & 0xFFFF0000) | (static_cast<uint32_t>(sval) & 0xFFFF);
    write32le(loc, insn);
    break;
  }
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << rel.type;
  }

  // TLS GD→LE relaxation for the 5-instruction PIC GOT-PCREL pattern.
  // The non-PIC TLS_GD_LO16/HI16 pattern is already handled by the
  // switch above (val = TPREL, written to LLI/LUI immediate field).
  //
  // PIC pattern:
  //   loc-4: MOV R1, PC(R15) → MOV R1, TP(R12)    [0x10380000]
  //   loc+0: LLI R2, gdlo    → LLI R2, tprel_lo16  (this reloc)
  //   loc+4: LUI R2, gdhi    → LUI R2, tprel_hi16  (next reloc)
  //   loc+8: ADD R1, R2       (unchanged)
  //  loc+12: BL __tls_get_addr → NOP (ADD R0,R0)    [0x00000000]
  if (rel.expr == R_RELAX_TLS_GD_TO_LE) {
    if (rel.type == R_PENUMBRA_TLS_GD_GOT_PCREL_LO16) {
      write32le(loc - 4, 0x10380000);  // MOV R1, R12
      write32le(loc, (read32le(loc) & 0xFFFF0000) | (val & 0xFFFF));
      write32le(loc + 12, 0x00000000); // NOP
    } else if (rel.type == R_PENUMBRA_TLS_GD_GOT_PCREL_HI16) {
      write32le(loc, (read32le(loc) & 0xFFFF0000) | ((val >> 16) & 0xFFFF));
    }
    // TLS_GD_LO16/HI16: switch above already wrote the correct value.
  }
}

void elf::setPenumbraTargetInfo(Ctx &ctx) {
  ctx.target.reset(new class Penumbra(ctx));
}
