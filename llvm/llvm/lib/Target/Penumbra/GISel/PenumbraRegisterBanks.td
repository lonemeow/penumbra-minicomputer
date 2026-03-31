//===-- PenumbraRegisterBanks.td - Penumbra Register Banks --*- tablegen -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

// Single register bank covering all allocatable GPRs.
// Penumbra has one register file; there are no FP or vector banks.
def GPRRegBank : RegisterBank<"GPR", [GPR_Allocatable]>;
