// penumbra2_scratch_file — Penumbra/2 scratch special-purpose registers.
//
// SCR0..SCR3 are the four supervisor scratch SPRs the TLB-miss fast path uses
// as a register save area before it can touch memory (RDSPR / WRSPR SCRn).
// They are plain storage: unlike EPC/ESR/SR (penumbra2_spr_file) the exception
// model never writes them implicitly, and unlike USP they are not regfile-
// backed — their scoreboard entries SB_SCR0..3 fall outside the 16-entry
// register file, so they need storage of their own. Hence this small file: one
// write port (WRSPR, committed at WB) and one combinational read port (RDSPR,
// read as operand B at ID).
//
// The spine gates i_we to SCRn and routes o_rd_value into the operand-B SPR
// mux. A non-SCRn i_rd_sel reads 0 here — the spine selects the SPR file (for
// EPC/ESR) or the regfile (for USP) in those cases, so this file only has to
// answer for its own SPR numbers.

module penumbra2_scratch_file
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── WRSPR write (from WB; the spine gates i_we to SCRn) ───────
    input  logic        i_we,
    input  logic [3:0]  i_w_sel,       // SPR number (SPR_SCR0 .. SPR_SCR3)
    input  logic [31:0] i_w_value,

    // ── RDSPR read (combinational, at ID) ────────────────────────
    input  logic [3:0]  i_rd_sel,      // SPR number; a non-SCRn sel reads 0
    output logic [31:0] o_rd_value
);
    // One entry per scratch SPR; deriving the count from the SPR numbering
    // keeps it from drifting out of step with the SPR_SCR* definitions.
    localparam int N_SCR  = int'(SPR_SCR3) - int'(SPR_SCR0) + 1;
    localparam int SCR_IW = $clog2(N_SCR);

    logic [31:0] scr [N_SCR];

    // SCRn occupy contiguous SPR numbers (SPR_SCR0..SPR_SCR3), so the array
    // index is simply the SPR number offset from SPR_SCR0 (a SCRn sel is in
    // range by construction, so the offset truncates cleanly to SCR_IW bits).
    logic [SCR_IW-1:0] rd_idx, w_idx;
    assign rd_idx = SCR_IW'(i_rd_sel - SPR_SCR0);
    assign w_idx  = SCR_IW'(i_w_sel  - SPR_SCR0);

    function automatic logic is_scr(input logic [3:0] sel);
        is_scr = (sel >= SPR_SCR0) && (sel <= SPR_SCR3);
    endfunction

    // ── RDSPR readback mux ───────────────────────────────────────
    // A SCRn read returns its entry; anything else reads 0 (the spine does not
    // consume this output for non-SCRn SPRs, but a defined value keeps the
    // operand-B mux deterministic).
    assign o_rd_value = is_scr(i_rd_sel) ? scr[rd_idx] : 32'b0;

    // ── WRSPR scratch write (committed at WB) ────────────────────
    // Drive the scratch array write. i_we is already gated to SCRn by the
    // spine, so a fired i_we always names a valid entry; w_idx (above) is the
    // array index and i_w_value the data. Each WRSPR must land in exactly one
    // entry and leave the others untouched (test_scratch_sprs probes for
    // cross-wiring with distinct per-SCR patterns). Whether these flops carry a
    // reset is a design call — see the Learn by Doing note.
    always_ff @(posedge i_clk) begin
        if (i_we) scr[w_idx] <= i_w_value;
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // The spine gates i_we to SCRn; a write naming any other SPR means that
    // gating is broken and a scratch entry would be addressed out of range.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_we |-> is_scr(i_w_sel))
        else $error("penumbra2_scratch_file: WRSPR write to a non-SCRn SPR (%0d)", i_w_sel);

endmodule
