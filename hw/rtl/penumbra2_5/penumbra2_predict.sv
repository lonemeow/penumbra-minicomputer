// penumbra2_predict — Penumbra/2.5 ID-stage static branch direction
// predictor (BTFN).
//
// A gen2.5-only leaf: it lives in hw/rtl/penumbra2_5/ and is instantiated by
// the gen2.5 ID-stage fork. gen2 has no predictor at all — this file is simply
// not part of gen2's fileset — which is why gen2's datapath reads as a machine
// with no notion of prediction.
//
// Combinational. For the Format B instruction in ID it produces a static
// taken/not-taken prediction and the branch target. The spine uses these to
// redirect fetch at ID instead of waiting for EX to resolve the branch —
// turning a correctly-predicted taken branch's 3-bubble flush into 2.
//
// Scheme — BTFN (Backward-Taken / Forward-Not-taken) plus an
// unconditional-direct fold:
//   - unconditional direct branch (B / BL): always taken.
//   - conditional branch (Bcc): taken iff the displacement is backward
//     (negative), not-taken iff forward.
// BTFN matches the code the compiler emits — block placement makes the likely
// path fall through (forward not-taken) and closes loops with a backward
// branch (backward taken) — so a historyless static predictor hits on
// well-formed code. Every branch is still resolved in EX, the authority, so a
// misprediction costs an extra flush, never a wrong result.
//
// Why ID and not fetch: the target add here runs on registered ID inputs, off
// the icache/TLB critical path. Beating ID's 2-bubble penalty from fetch needs
// a BTB (a target RAM read parallel to the icache), the planned evolution —
// see doc/internals/penumbra2/overview.md and doc/TODO.md.
//
// The encoding's offset is relative to the branch itself, and i_imm is the
// decoder's sign-extended byte offset (== sign_extend(offset22 << 2)), so the
// target is a single add and the displacement's sign is i_imm[31].

module penumbra2_predict
    import penumbra_pkg::*;     // COND_AL, COND_BL
(
    // ── The Format B instruction in ID ───────────────────────────
    input  logic        i_is_branch,  // ID op_class == OPC_BRANCH
    input  logic [3:0]  i_cond,       // its condition field
    input  logic [31:0] i_pc,         // the branch's own PC
    input  logic [31:0] i_imm,        // sign-extended byte offset; i_imm[31] = backward

    // ── Static prediction for the spine's ID-stage redirect ──────
    output logic        o_predict_taken,
    output logic [31:0] o_predict_target
);

    // Target: PC + the branch-relative sign-extended byte offset. A single
    // add (no PC+4 correction — the offset is branch-relative). Only
    // meaningful when i_is_branch.
    assign o_predict_target = i_pc + i_imm;

    // ── BTFN direction policy ────────────────────────────────────
    // Predict taken when the ID instruction is a branch and it is either
    // unconditional (COND_AL plain B / COND_BL branch-and-link, both always
    // taken) or its displacement is backward (i_imm[31] — a loop-closing
    // branch). Forward conditionals fall through. The i_is_branch guard is
    // outermost so a non-branch word never predicts taken (the assertion
    // below enforces it).
    assign o_predict_taken = i_is_branch & (i_imm[31] | (i_cond == COND_AL) | (i_cond == COND_BL));

    // ── Assertion (sim-only; stripped at synth) ──────────────────
    // Never predict a non-branch taken: a stray taken on a non-Format-B word
    // would steer fetch to a bogus target. The i_is_branch guard in the policy
    // makes this hold.
    always_comb
        if (o_predict_taken)
            assert (i_is_branch)
                else $error("penumbra2_predict: predicted taken on a non-branch");

endmodule
