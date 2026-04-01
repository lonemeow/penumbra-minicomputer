//===-- PenumbraLegalizerInfo.cpp ---------------------------------------===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

#include "PenumbraLegalizerInfo.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"

using namespace llvm;

PenumbraLegalizerInfo::PenumbraLegalizerInfo(const PenumbraSubtarget &ST) {
  using namespace TargetOpcode;

  const LLT s8  = LLT::scalar(8);
  const LLT s16 = LLT::scalar(16);
  const LLT s32 = LLT::scalar(32);
  const LLT p0  = LLT::pointer(0, 32);

  getActionDefinitionsBuilder({G_ADD, G_SUB, G_AND, G_OR, G_XOR, G_SHL, G_LSHR, G_ASHR})
      .legalFor({s32}) 
      .clampScalar(0, s32, s32);

  getActionDefinitionsBuilder(G_CONSTANT)
      .legalFor({s32, p0})
      .clampScalar(0, s32, s32);

  getActionDefinitionsBuilder({G_FRAME_INDEX, G_GLOBAL_VALUE})
      .legalFor({p0});

  getActionDefinitionsBuilder({G_STORE, G_LOAD})
      .legalForTypesWithMemDesc({
        {s32, p0, s32, 4},  // LDW/STW
        {s32, p0, s16, 2},  // STH (store) / plain LDH (zero-extend, handled by G_ZEXTLOAD too)
        {s32, p0, s8,  1},  // STB / plain LDB
        {p0,  p0, s32, 4},  // pointer load/store
      });

  getActionDefinitionsBuilder({G_SEXTLOAD, G_ZEXTLOAD})
      .legalForTypesWithMemDesc({
        {s32, p0, s16, 2},
        {s32, p0, s8,  1},
      });

  getActionDefinitionsBuilder(G_PTR_ADD)
      .legalFor({{p0, s32}});

  // Comparisons: G_ICMP produces s1 result, compares s32 operands.
  const LLT s1 = LLT::scalar(1);
  getActionDefinitionsBuilder(G_ICMP)
      .legalFor({{s1, s32}})
      .clampScalar(1, s32, s32);

  // PHI nodes at control-flow joins.
  getActionDefinitionsBuilder(G_PHI)
      .legalFor({s32, p0})
      .clampScalar(0, s32, s32);

  // Branches.
  getActionDefinitionsBuilder(G_BRCOND)
      .legalFor({s1});

  // Select (ternary): result s32/p0, condition s1.
  getActionDefinitionsBuilder(G_SELECT)
      .legalFor({{s32, s1}, {p0, s1}})
      .clampScalar(0, s32, s32);

  getLegacyLegalizerInfo().computeTables();
}
