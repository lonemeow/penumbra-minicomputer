// penumbra2_btb — Penumbra/2.5 branch target buffer.
//
// A gen2.5-only leaf (like penumbra2_predict / penumbra2_ras): it lives in
// hw/rtl/penumbra2_5/ and is instantiated by the gen2.5 core fork. gen2 has no
// BTB — this file is simply not part of gen2's fileset.
//
// BTFN and the RAS predict at ID, which structurally costs two front-end
// bubbles on a correct prediction: the redirect cannot fire until the branch is
// decoded. The BTB predicts at *fetch* instead. Indexed by the fetch PC, it
// reads a small tagged target RAM in parallel with the icache and, on a hit,
// steers fetch to the cached target — turning a correctly-predicted taken
// direct branch's two bubbles into one. The lookup is a pure RAM read in the
// IF1 launch cycle; the tag-compare runs in IF2, alongside the icache's own
// VIPT tag-compare, so the BTB adds nothing to the fmax-critical fetch cone.
//
// Scope: direct taken branches (B / Bcc / BL). A tagged hit guarantees the
// cached target equals this branch's PC+imm (the entry was trained from this
// exact PC), so EX's existing direction-only mispredict check stays sound with
// no target compare on the branch path. Function returns stay with the RAS;
// other indirect jumps stay EX-resolved. EX remains the branch authority — a
// stale or aliased prediction costs at most an extra flush, never a wrong
// result — so the BTB is trained from resolved branches and needs no
// checkpoint/restore: a precise fault leaves a bounded, self-healing entry set
// because every misprediction is corrected downstream.
//
// See doc/internals/penumbra2/overview.md.

module penumbra2_btb #(
    // Number of tracked branch sites. A working set larger than ENTRIES
    // thrashes (aliasing branches evict each other) but never misbehaves —
    // a miss falls back to ID-stage BTFN, a false hit is corrected by EX.
    // Must be a power of two: the index is a slice of the PC. Kept small —
    // a few dozen entries cover the hot-branch working set, and a larger
    // table spends more RAM for diminishing hit-rate.
    parameter int ENTRIES = 32
) (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Fetch-side lookup (launched in IF1, result consumed in IF2) ──
    // i_lookup_en tracks IF1's o_fetch_en: the registered read advances only
    // when the fetch does, so under a stall the prediction freezes in lockstep
    // with the held icache word. o_hit / o_target describe the PC looked up the
    // *previous* cycle (combinational off the registered read), ready in IF2.
    input  logic        i_lookup_en,
    // Word-addressed: the index/tag use i_lookup_pc[31:2]; the low 2 bits are
    // intentionally ignored (a misaligned fetch faults in IF2, not here).
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_lookup_pc,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic        o_hit,
    output logic [31:0] o_target,

    // ── Train-side update (a direct branch resolving in EX) ──────────
    // i_update_taken distinguishes allocate/refresh from invalidate. The target
    // is the resolved PC+imm; it is word-aligned, so only [31:2] is stored.
    input  logic        i_update,
    input  logic [31:0] i_update_pc,
    input  logic [31:0] i_update_target,
    input  logic        i_update_taken
);

    localparam int IDX_W = $clog2(ENTRIES);
    localparam int TAG_W = 32 - 2 - IDX_W;   // PC[1:0] are always 0 (word-aligned)

    // Entry store. `valid` is a flop vector so reset can clear it (a BRAM/RAM
    // cannot); tags and targets need no reset because `valid` gates every read.
    logic              valid   [ENTRIES];
    logic [TAG_W-1:0]  tags    [ENTRIES];
    logic [29:0]       targets [ENTRIES];

    // PC field split, shared by the lookup and update sides.
    logic [IDX_W-1:0]  look_idx;
    logic [TAG_W-1:0]  look_tag;
    logic [IDX_W-1:0]  upd_idx;
    logic [TAG_W-1:0]  upd_tag;
    assign look_idx = i_lookup_pc[2 +: IDX_W];
    assign look_tag = i_lookup_pc[2 + IDX_W +: TAG_W];
    assign upd_idx  = i_update_pc[2 +: IDX_W];
    assign upd_tag  = i_update_pc[2 + IDX_W +: TAG_W];

    // ── Registered fetch read ────────────────────────────────────
    // Read the indexed entry and carry the queried tag forward, so the IF2-cycle
    // hit decision is a compare of two registered values — no RAM access on the
    // consuming cycle. Frozen under stall (i_lookup_en low) to stay matched to
    // the held icache word.
    logic              rd_valid_q;
    logic [TAG_W-1:0]  rd_tag_q;
    logic [29:0]       rd_target_q;
    logic [TAG_W-1:0]  look_tag_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            rd_valid_q <= 1'b0;
        end else if (i_lookup_en) begin
            rd_valid_q  <= valid[look_idx];
            rd_tag_q    <= tags[look_idx];
            rd_target_q <= targets[look_idx];
            look_tag_q  <= look_tag;
        end
    end

    assign o_hit    = rd_valid_q & (rd_tag_q == look_tag_q);
    assign o_target = {rd_target_q, 2'b00};

    // ── Entry update ─────────────────────────────────────────────
    // Policy:
    //   - on a taken resolve  : allocate/refresh the entry at upd_idx
    //   - on a not-taken resolve: tag-checked invalidate (clear only this
    //     branch's own entry, so an aliasing not-taken branch never evicts a
    //     resident, tag-mismatched neighbour).
    // upd_idx/upd_tag and the resolved target are available; tags[]/targets[]
    // are flop-readable at any index, so the invalidate tag-check needs no
    // extra read port.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            for (int e = 0; e < ENTRIES; e++)
                valid[e] <= 1'b0;
        end else if (i_update) begin
            if (i_update_taken) begin
                valid[upd_idx]   <= 1'b1;
                tags[upd_idx]    <= upd_tag;
                targets[upd_idx] <= i_update_target[31:2];
            end else begin
                if (tags[upd_idx] == upd_tag) begin
                    valid[upd_idx] <= 1'b0;
                end
            end
        end
    end

    // ── Assertions (sim-only; stripped at synth) ─────────────────
    // A *resolved* branch's PC and target are always word-aligned: a branch
    // only trains after issuing, which means it was fetched without an alignment
    // fault, and its target is PC + (offset << 2). The dropped low bits there
    // carry no information, so a non-zero one means a miswired feed.
    //
    // The *lookup* PC is deliberately NOT asserted aligned: a jump to a
    // misaligned address is a legal fetch that raises VEC_ALIGN downstream in
    // IF2. Its BTB lookup is harmless — the index/tag use PC[31:2], so it can
    // even alias a trained entry and spuriously hit, but the misaligned slot
    // still faults and the fault flushes any wrong-path redirect, with EX the
    // branch authority regardless.
    always_comb begin
        if (i_update) begin
            assert (i_update_pc[1:0] == 2'b00)
                else $error("penumbra2_btb: misaligned update PC");
            if (i_update_taken)
                assert (i_update_target[1:0] == 2'b00)
                    else $error("penumbra2_btb: misaligned update target");
        end
    end

endmodule
