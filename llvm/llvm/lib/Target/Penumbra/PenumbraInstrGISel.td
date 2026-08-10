//===-- PenumbraInstrGISel.td - Penumbra GISel target pseudos -*- tablegen -*-===//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// Target generic instructions: opcodes that exist only in generic MIR,
// created by Penumbra combines and consumed by the instruction selector.
//
//===----------------------------------------------------------------------===//

class PenumbraGenericInstruction : GenericInstruction {
  let Namespace = "Penumbra";
}

// Widening multiply producing both product halves, mirroring the MUL_P /
// MULU_P divmul instructions.  GISel has no generic two-result multiply
// (no G_* analog of ISD::UMUL_LOHI), so the post-legalizer combiner
// fuses a same-operand G_MUL + G_[SU]MULH pair into these.
def G_UMUL_LOHI : PenumbraGenericInstruction {
  let OutOperandList = (outs type0:$lo, type0:$hi);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
  let isCommutable = true;
}

def G_SMUL_LOHI : PenumbraGenericInstruction {
  let OutOperandList = (outs type0:$lo, type0:$hi);
  let InOperandList = (ins type0:$src1, type0:$src2);
  let hasSideEffects = false;
  let isCommutable = true;
}
