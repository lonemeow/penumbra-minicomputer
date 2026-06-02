// penumbra2_scoreboard — Penumbra/2 RAW hazard scoreboard.
//
// Specified by doc/internals/penumbra2/hazard-model.md. Gates issue
// at ID: it holds the ID instruction whenever a source operand it
// reads is still being produced by an instruction further down the
// pipe (EX/MEM/WB). RAW is the only data hazard the gen2 pipeline
// has — completion is strictly in order, so WAW/WAR cannot arise
// (§4), and last-writer-wins falls out for free.
//
// The valid bits are NOT stored in set/clear flops. They are
// re-derived combinationally each cycle from the destinations of the
// instructions currently in flight (§3, §11): an entry is valid iff
// no in-flight instruction will write it. This is what makes a chain
// of writers to one entry behave correctly — the entry stays invalid
// until its *youngest* writer drains — and what makes the scoreboard
// squash-safe (a flushed instruction simply stops contributing its
// destination next cycle).
//
// Entries are physical, not architectural: USP (14) and SSP (15) are
// separate, so the WRSPR-USP / user-R14 aliasing hazard is caught
// (§5.1). The ID->physical mapping that produces these indices lives
// in the decoder (§5); this module consumes already-mapped indices.

module penumbra2_scoreboard
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    // ── ID instruction source operands ──────────────────────────
    // Physical entry indices from the ID->physical mapping, each
    // with an enable marking whether the instruction actually reads
    // it. A source that is not read must never cause a stall.
    input  logic [SB_IDX_W-1:0]       i_src_a,
    input  logic                      i_src_a_en,
    input  logic [SB_IDX_W-1:0]       i_src_b,
    input  logic                      i_src_b_en,

    // ── In-flight writers downstream of ID ──────────────────────
    // The destination each of EX, MEM, WB will write, with an enable
    // that is 0 for instructions with no GPR/SPR destination (ST,
    // Bcc, ...). Driven from the pipeline registers.
    input  logic [SB_IDX_W-1:0]       i_ex_dst,
    input  logic                      i_ex_dst_en,
    input  logic [SB_IDX_W-1:0]       i_mem_dst,
    input  logic                      i_mem_dst_en,
    input  logic [SB_IDX_W-1:0]       i_wb_dst,
    input  logic                      i_wb_dst_en,

    // An auxiliary in-flight writer, beyond the three stage
    // destinations — for an instruction whose destination is not the
    // primary dst of any single stage. The current user is divmul: its
    // second result (Rdh) is an in-flight writer of its own entry for
    // the whole iteration, alongside Rd, so a reader of either stalls
    // until it drains (regfile.md §4, hazard-model.md §3.4 / §10).
    input  logic [SB_IDX_W-1:0]       i_aux_dst,
    input  logic                      i_aux_dst_en,

    // ── Outputs ─────────────────────────────────────────────────
    // Per-entry valid vector: valid[P]=1 means no in-flight writer
    // targets P (its committed value is in the regfile). Entry 0
    // (R0) is tied valid. Exposed for downstream use and observation.
    output logic [SB_NUM_ENTRIES-1:0] o_valid,
    // RAW stall for the ID instruction: a read source is not valid.
    output logic                      o_stall
);

    // entry_busy(p): 1 iff some enabled in-flight writer has p as its
    // destination. This is the combinational re-derive at the heart
    // of the scoreboard (hazard-model.md §3-§4): there is
    // deliberately no notion of "which stage is older" — in-order
    // completion (§4) makes that unnecessary, so an entry is simply
    // busy while ANY in-flight writer targets it.
    function automatic logic entry_busy(input logic [SB_IDX_W-1:0] p);
        return p == i_ex_dst && i_ex_dst_en
            || p == i_mem_dst && i_mem_dst_en
            || p == i_wb_dst && i_wb_dst_en
            || p == i_aux_dst && i_aux_dst_en;
    endfunction

    // valid[P] = not busy, except entry 0 (R0), which is tied valid:
    // the decoder never emits 0 as a real source or destination.
    always_comb begin
        for (int p = 0; p < SB_NUM_ENTRIES; p++)
            o_valid[p] = (p == 0) ? 1'b1 : ~entry_busy(p[SB_IDX_W-1:0]);
    end

    // RAW stall: an actually-read source whose entry is not valid.
    assign o_stall = (i_src_a_en & ~o_valid[i_src_a])
                   | (i_src_b_en & ~o_valid[i_src_b]);

endmodule
