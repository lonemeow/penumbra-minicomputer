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
// Round-trip invariant for ADD/SUB constants:
//   The upstream `sub_to_add` rule rewrites `G_SUB x, c` → `G_ADD x, -c`.
//   On targets with sign-extending ALU immediates this is a pure win, but
//   our ADDi/SUBi take a uimm16 (zero-extended).  Our post-legalizer rule
//   `penumbra_neg_imm_to_opposite` re-canonicalizes any `G_ADD x, neg_c`
//   (where -c fits uimm16) back to `G_SUB x, c`, restoring SUBi.  Pre- and
//   post-legalizer combiners therefore round-trip cleanly: enabling
//   `all_combines` pre-legalizer is safe.  For constants outside uimm16
//   the result is neutral (the operation has to be materialized either
//   way).
//
//===----------------------------------------------------------------------===//

include "llvm/Target/GlobalISel/Combine.td"

// Pre-legalizer: full upstream canonicalization / DCE on pre-legalized
// generic MIR, matching AArch64 / RISC-V.  The `*_by_const` magic-multiply
// rules in `intdiv_combines`/`intrem_combines` are gated by the target's
// `isIntDivCheap()` hook — see PenumbraISelLowering.cpp, which returns
// true so the divide stays a divide: the rewrite's high-half multiply
// costs the same divmul-unit latency as the divide it replaces, so it
// only adds shift/fixup instructions.  Penumbra-specific fixups for the
// zero-extending uimm16 form happen in the post-legalizer combiner.
def PenumbraPreLegalizerCombiner
    : GICombiner<"PenumbraPreLegalizerCombinerImpl", [all_combines]>;

// At -O0, run a small, hand-picked rule set that cleans up the most
// obvious IRTranslator artefacts (DCE, ptr_add chaining, extload folding,
// branch inversion) without paying the compile-time cost of full
// canonicalization.  Matches AArch64 / RISC-V.
def PenumbraO0PreLegalizerCombiner
    : GICombiner<"PenumbraO0PreLegalizerCombinerImpl", [optnone_combines]>;

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

// Target-specific: rewrite a plain `G_LOAD :: (load sX) -> sY` (sX < sY) into
// `G_ZEXTLOAD`.  Penumbra's LDB/LDH always zero-extend in hardware, so a
// sub-word plain load is *already* a zextload semantically; the IR-level
// extload form just doesn't tell the rest of the pipeline.  After this
// rewrite the known-bits tracker reports the high bits as zero, which lets
// `redundant_and` drop the `G_AND %, 0xFF` masks the legalizer emits when
// widening unsigned / eq / ne `G_ICMP` operands.  In a multi-use loop —
// e.g. strcmp's loop where the loaded byte flows through a `G_PHI` to both
// the comparison and a self-feeding edge — the upstream `load_and_mask`
// combine bails (it requires the AND to be a direct user of a single-use
// load), so this target rule is what unblocks the optimization.
//
// Sign-extending loads (G_SEXTLOAD / `LDBS`) are not handled here; see
// `doc/TODO.md` ("Compiler: signed sub-word loads through PHIs").
def penumbra_zextload_promote : GICombineRule<
  (defs root:$root),
  (match (wip_match_opcode G_LOAD):$root,
         [{ return matchZextloadPromote(*${root}, MRI); }]),
  (apply [{ applyZextloadPromote(*${root}, MRI, B); }])>;

// Target-specific: sink a `G_PTR_ADD %new, %old, K` past the immediately-
// following memory op when that op uses `%old` (the source) and not
// `%new` (the result).  Penumbra's ALU is destructive 2-operand
// (`add Rd, imm` writes back to Rd), so a pointer bump that's emitted
// *before* the load/store of the old pointer keeps both the old and new
// pointer simultaneously live across the memory op.  TwoAddress then
// inserts a `COPY old → new` to satisfy the tied-def constraint, and the
// register coalescer can't eliminate that COPY because the live ranges
// overlap.  The result is a trailing register-to-register MOV at the
// loop back-edge that's pure overhead.
//
// Sinking the G_PTR_ADD to *after* its source's last use lets the load
// kill `%old` first, freeing the same physical register for `%new` —
// which collapses to a destructive `ADDi Rd, 1` in place, and the back-
// edge MOV vanishes.  For strcpy/memcpy shapes this drops the inner loop
// from 8 to 6 instructions per iteration.
//
// The combine rewrites one pair at a time and the combiner loop handles
// the cascade: `[GEP_s; GEP_d; LDB; STB]` becomes `[LDB; GEP_s; GEP_d;
// STB]` after one iteration, then `[LDB; GEP_s; STB; GEP_d]` after the
// next.  See `doc/TODO.md` for the historical context (Cause 2 of the
// pointer-bump-loop suboptimality).
def penumbra_sink_ptr_add_past_use : GICombineRule<
  (defs root:$root),
  (match (wip_match_opcode G_PTR_ADD):$root,
         [{ return matchSinkPtrAddPastUse(*${root}, MRI); }]),
  (apply [{ applySinkPtrAddPastUse(*${root}, MRI, B); }])>;

// Target-specific: fuse a same-operand G_MUL + G_[SU]MULH pair into the
// two-result G_UMUL_LOHI / G_SMUL_LOHI (PenumbraInstrGISel.td), which
// selects to a single MUL_P / MULU_P.  One multiply yields the full 64-bit
// product either way, so the separate low-half MUL the unfused pair selects
// to is a second divmul operation recomputing a half MUL_P already produced.
// The pair is born adjacent from the legalizer's s64-multiply lowering; the
// rule roots at the (rarer) MULH and searches its operands' uses for the
// sibling.
def penumbra_mul_lohi_matchdata : GIDefMatchData<"MachineInstr *">;
def penumbra_mul_to_mul_lohi : GICombineRule<
  (defs root:$root, penumbra_mul_lohi_matchdata:$matchinfo),
  (match (wip_match_opcode G_UMULH, G_SMULH):$root,
         [{ return matchMulToMulLoHi(*${root}, MRI, ${matchinfo}); }]),
  (apply [{ applyMulToMulLoHi(*${root}, MRI, B, ${matchinfo}); }])>;

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
//   penumbra_zextload_promote — target-specific; see rule definition above.
//   known_bits_simplifications — includes `redundant_and`, which is what
//     consumes the known-zero high bits introduced by the promote rule
//     above to drop the legalizer's `G_AND %, 0xFF` masks (also picks up
//     `redundant_or` / `redundant_sext_inreg` for free).  Required at
//     post-legalize because pre-legalize MIR doesn't yet have the widened
//     ICMP shape that produces these masks.
//   penumbra_neg_imm_to_opposite — see above; flips ADD↔SUB when the
//     constant is negative but -c fits uimm16.
def PenumbraPostLegalizerCombiner
    : GICombiner<"PenumbraPostLegalizerCombinerImpl", [
      commute_constant_to_rhs,
      ptr_add_immed_chain,
      combines_for_extload,
      penumbra_zextload_promote,
      known_bits_simplifications,
      penumbra_neg_imm_to_opposite,
      penumbra_sink_ptr_add_past_use,
      penumbra_mul_to_mul_lohi
    ]>;
