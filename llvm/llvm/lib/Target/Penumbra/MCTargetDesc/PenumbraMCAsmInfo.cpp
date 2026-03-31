//===-- PenumbraMCAsmInfo.cpp - Penumbra Asm Properties -------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraMCAsmInfo.h"

using namespace llvm;

PenumbraMCAsmInfo::PenumbraMCAsmInfo(const Triple &TT) {
  // Penumbra is little-endian, 32-bit
  IsLittleEndian = true;
  CodePointerSize = 4;
  CalleeSaveStackSlotSize = 4;

  // Assembly syntax
  CommentString = ";";
  SupportsDebugInformation = true;

  // Data directives
  Data32bitsDirective = "\t.word\t";
  Data16bitsDirective = "\t.half\t";
  Data8bitsDirective = "\t.byte\t";
  ZeroDirective = "\t.zero\t";
}
