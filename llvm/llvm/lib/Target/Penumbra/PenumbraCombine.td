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

// Target-specific: rewrite G_ADD/G_SUB by a negative constant whose negation
// fits uimm16 to the opposite opcode with the positive constant, e.g.
// `G_ADD x, -1 → G_SUB x, 1`.  Our ADDi/SUBi take uimm16 (0–65535) and do
// not sign-extend, so `n--` without this rule falls back to materializing
// -1 via LLIS + reg-reg ADD.  After the rewrite, our existing
// (sub GPR, uimm16) → SUBi pattern fires.
def penumbra_neg_imm_to_opposite_matchdata : GIDefMatchData<"int64_t">;
def penumbra_neg_imm_to_opposite : GICombineRule<
  (defs root:$root, penumbra_neg_imm_to_opposite_matchdata:$matchinfo),
  (match (wip_match_opcode G_ADD, G_SUB):$root,
         [{ return matchNegImmToOpposite(*${root}, MRI, ${matchinfo}); }]),
  (apply [{ applyNegImmToOpposite(*${root}, MRI, B, ${matchinfo}); }])>;

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
//   penumbra_neg_imm_to_opposite — see above; flips ADD↔SUB when the
//     constant is negative but -c fits uimm16.
def PenumbraPostLegalizerCombiner
    : GICombiner<"PenumbraPostLegalizerCombinerImpl", [
      commute_constant_to_rhs,
      ptr_add_immed_chain,
      combines_for_extload,
      penumbra_neg_imm_to_opposite
    ]>;
