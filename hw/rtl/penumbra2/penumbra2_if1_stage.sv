// penumbra2_if1_stage — Penumbra/2 instruction fetch, first half.
//
// Specified by the IF1 section of doc/internals/penumbra2/pipeline-stages.md.
// Owns the PC register. Each cycle it drives the fetch address (PC) to the
// I-cache BRAM combinationally — the BRAM samples it at this edge and presents
// the word during IF2 next cycle — and registers this fetch's PC, next_PC, and
// valid bit into the IF1/IF2 register so IF2 can pair that word with the
// address that produced it.
//
// The full stage also runs the TLB lookup, the vector-fetch FSM for exception
// entry, and accepts the taken-branch PC redirect from EX. None of that exists
// yet: there is no MMU, no exceptions, and branches are deferred, so PC simply
// advances by 4, or holds when IF2 back-pressures. The redirect mux will later
// land in the PC register's advance path.

module penumbra2_if1_stage #(
    parameter logic [31:0] RESET_PC = 32'h0000_0000
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic        i_stall_in,    // IF2 cannot accept (hold PC + IF1/IF2)

    // ── I-cache BRAM address (combinational) ─────────────────────
    output logic [31:0] o_fetch_addr,
    output logic        o_fetch_en,    // read clock-enable: freezes the BRAM with the PC under stall

    // ── IF1/IF2 register out (to IF2) ────────────────────────────
    output logic [31:0] o_pc,
    output logic [31:0] o_next_pc,
    output logic        o_valid
);

    // The fetch holds — PC unchanged, IF1/IF2 register frozen — whenever IF2
    // back-pressures; otherwise it advances by one instruction.
    logic [31:0] pc;
    logic [31:0] pc_plus_4;
    logic        hold;

    assign pc_plus_4 = pc + 32'd4;
    assign hold      = i_stall_in;

    // Drive the current PC to the I-cache BRAM every cycle. The BRAM read
    // advances only when the fetch does (the same `!hold`), so under stall the
    // registered output freezes in lockstep with the IF1/IF2 register below —
    // without this the in-flight read would land against the held PC and drop
    // an instruction.
    assign o_fetch_addr = pc;
    assign o_fetch_en   = ~hold;

    // ── PC register ──────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst)
            pc <= RESET_PC;
        else if (!hold)
            pc <= pc_plus_4;
    end

    // ── IF1/IF2 register ─────────────────────────────────────────
    // Carries this fetch's PC / next_PC / valid to IF2 so it can pair the BRAM
    // word (which lands next cycle) with the address that produced it. Frozen
    // under back-pressure so the held word stays matched to o_pc. Every fetch
    // is valid here — there is no flush source yet (branches are deferred).
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid <= 1'b0;
        end else if (!hold) begin
            o_pc      <= pc;
            o_next_pc <= pc_plus_4;
            o_valid   <= 1'b1;
        end
    end

endmodule
