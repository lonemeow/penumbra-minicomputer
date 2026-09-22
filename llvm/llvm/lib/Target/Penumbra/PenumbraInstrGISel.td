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

// Relational compare of two-word values given as their low and high words,
// selected as CMP on the low words and CMPC on the high ones.  GISel has no
// counterpart to SelectionDAG's SETCCCARRY, so the legalizer's s64 G_ICMP
// rule emits this in place of the generic select chain.  The predicate is
// always a less-than or greater-or-equal: those alone read the chain's N, C
// and V, since its Z reflects the high words only.
def G_ICMP_PAIR : PenumbraGenericInstruction {
  let OutOperandList = (outs type0:$dst);
  let InOperandList = (ins unknown:$tst, type1:$lhs_lo, type1:$lhs_hi,
                           type1:$rhs_lo, type1:$rhs_hi);
  let hasSideEffects = false;
}
