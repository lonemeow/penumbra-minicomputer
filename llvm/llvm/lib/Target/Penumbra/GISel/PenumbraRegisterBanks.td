//===-- PenumbraRegisterBanks.td - Penumbra Register Banks --*- tablegen -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//

// Single register bank covering the full GPR class (all 16 registers).
// The bank must cover the GPR register class used in instruction operand
// definitions so that constrainRegClass() can succeed during instruction
// selection. The register allocator uses GPR_Allocatable (the subclass
// excluding reserved regs) when choosing physical registers.
def GPRRegBank : RegisterBank<"GPRBank", [GPR]>;
