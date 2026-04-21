//=- PenumbraCombine.td - Define Penumbra Combine Rules ------*- tablegen -*-=//
//
// Part of the Penumbra LLVM Backend
//
//===----------------------------------------------------------------------===//
//
// GlobalISel combiner rule groups for Penumbra.  The generic rules referenced
// by name are defined in llvm/Target/GlobalISel/Combine.td.
//
// Pipeline placement:
//   PreLegalizer   — IR-to-IR rewrites on pre-legalized generic MIR
//                    (canonicalization, DCE, constant folding).
//   PostLegalizer  — optimizations on legalized MIR (must preserve
//                    instruction legality).
// Both run only at -O1+.  At -O0 the combiner is skipped entirely; we
// can add an O0 pass if a future rule needs -O0 participation.
//
// Current rule set is intentionally minimal: just `copy_prop` in each pass.
// This commit enables the pipeline scaffolding only, so any behavioral
// change is attributable to the framework wiring, not to rule selection.
// Subsequent commits will build out the rule lists (ptr_add_immed_chain,
// combines_for_extload, identity_combines, etc.) with targeted tests.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/GlobalISel/Combine.td"

def PenumbraPreLegalizerCombiner
    : GICombiner<"PenumbraPreLegalizerCombinerImpl", [copy_prop]>;

def PenumbraPostLegalizerCombiner
    : GICombiner<"PenumbraPostLegalizerCombinerImpl", [copy_prop]>;
