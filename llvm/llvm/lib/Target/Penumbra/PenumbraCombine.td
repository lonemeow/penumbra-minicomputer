//=- PenumbraCombine.td - Define Penumbra Combine Rules ------*- tablegen -*-=//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// GlobalISel combiner rule groups for Penumbra.  The generic rules and groups
// referenced here are defined in llvm/Target/GlobalISel/Combine.td.
//
// Both passes run only at -O1+; at -O0 the combiner is skipped entirely.
//
// Rules to AVOID for Penumbra:
//   sub_to_add — turns G_SUB with a constant into G_ADD of a negated
//     constant.  We have a real SUBi with uimm16; after sub_to_add the
//     negated constant falls out of uimm16 range and has to be materialized
//     in a register.  Net regression.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/GlobalISel/Combine.td"

// Pre-legalizer: canonicalization / DCE on pre-legalized generic MIR.
// Kept minimal for now; upstream canonicalizations need review against our
// uimm16-only ALU immediate forms before being enabled here.
def PenumbraPreLegalizerCombiner
    : GICombiner<"PenumbraPreLegalizerCombinerImpl", [copy_prop]>;

// Post-legalizer: optimizations on legalized MIR.  Must preserve instruction
// legality.
//
//   commute_constant_to_rhs — puts constants on the RHS so (add GPR, uimm16)
//     / (and GPR, uimm16) patterns fire when the source had the const on LHS.
//   ptr_add_immed_chain     — merges (G_PTR_ADD (G_PTR_ADD p, c1), c2) into
//     G_PTR_ADD p, (c1+c2).  Prerequisite for the AddrRegImm ComplexPattern
//     that will fold constant offsets into LDW/STW.
//   combines_for_extload    — folds G_ZEXT/G_SEXT of a narrower load into a
//     single extload, matched by the LDH/LDHS/LDB/LDBS patterns.
def PenumbraPostLegalizerCombiner
    : GICombiner<"PenumbraPostLegalizerCombinerImpl", [
      commute_constant_to_rhs,
      ptr_add_immed_chain,
      combines_for_extload
    ]>;
