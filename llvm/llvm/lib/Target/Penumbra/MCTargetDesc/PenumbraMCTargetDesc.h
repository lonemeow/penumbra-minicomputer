//===-- PenumbraMCTargetDesc.h - Penumbra Target Descriptions --*- C++ -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAMCTARGETDESC_H
#define LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAMCTARGETDESC_H

#include <memory>

namespace llvm {

class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCObjectTargetWriter;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;
class Target;

} // namespace llvm

// Defines symbolic names for Penumbra registers.
#define GET_REGINFO_ENUM
#include "PenumbraGenRegisterInfo.inc"

// Defines symbolic names for Penumbra instructions.
#define GET_INSTRINFO_ENUM
#include "PenumbraGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "PenumbraGenSubtargetInfo.inc"

#endif // LLVM_LIB_TARGET_PENUMBRA_MCTARGETDESC_PENUMBRAMCTARGETDESC_H
