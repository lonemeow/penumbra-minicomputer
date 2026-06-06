// penumbra2_if1_stage — Penumbra/2 instruction fetch, first half.
//
// Specified by the IF1 section of doc/internals/penumbra2/pipeline-stages.md.
// Owns the PC register. Each cycle it drives the fetch address (PC) to the
// I-cache BRAM combinationally — the BRAM samples it at this edge and presents
// the word during IF2 next cycle — and registers this fetch's PC, next_PC, and
// valid bit into the IF1/IF2 register so IF2 can pair that word with the
// address that produced it.
//
// The full stage also runs the TLB lookup for exception entry; the MMU does
// not exist yet. Two redirect-class actors do: the taken-branch PC redirect
// from EX, and exception entry. When EX resolves a branch taken it drives
// i_redirect with the target on i_redirect_pc; IF1 is the deepest of the three
// wrong-path slots a taken branch flushes, so the redirect both (a) steers PC
// to the target and (b) bubbles the IF1/IF2 register to discard the in-flight
// fetch. The redirect overrides back-pressure: even if IF2 is holding us, the
// target must be steered in so the next fetch reads it.
//
// Exception entry splits those two effects across two cycles. At the fault
// commit, i_flush bubbles the in-flight (wrong-path) fetch *without* steering
// PC — the handler address is not known yet. The external vector-fetch FSM
// (penumbra2_vecfetch) then owns the fetch port for a couple of cycles, during
// which IF1 is held (i_stall_in), and finally drives i_redirect with the
// handler address it read from the vector table.

module penumbra2_if1_stage #(
    parameter logic [31:0] RESET_PC = 32'h0000_0000
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic        i_stall_in,    // IF2 cannot accept (hold PC + IF1/IF2)

    // ── Taken-branch / vector-fetch redirect ─────────────────────
    input  logic        i_redirect,    // steer PC to the target + bubble this fetch
    input  logic [31:0] i_redirect_pc, // redirect target

    // ── Fault flush (from exception entry) ───────────────────────
    input  logic        i_flush,       // bubble the in-flight fetch without steering PC

    // ── I-cache BRAM address (combinational) ─────────────────────
    output logic [31:0] o_fetch_addr,
    output logic        o_fetch_en,    // read clock-enable: freezes the BRAM with the PC under stall

    // ── IF1/IF2 register out (to IF2) ────────────────────────────
    output logic [31:0] o_pc,
    output logic [31:0] o_next_pc,
    output logic        o_valid
);

    // The fetch holds — PC unchanged, IF1/IF2 register frozen — whenever IF2
    // back-pressures; otherwise it advances by one instruction. A redirect is
    // the third actor: it wins over both hold and sequential advance.
    logic [31:0] pc;
    logic [31:0] pc_plus_4;
    logic        hold;
    logic        redirect;

    assign pc_plus_4 = pc + 32'd4;
    assign hold      = i_stall_in;
    assign redirect  = i_redirect;

    // Drive the current PC to the I-cache BRAM every cycle. The BRAM read
    // advances only when the fetch does, so under stall the registered output
    // freezes in lockstep with the IF1/IF2 register below — without this the
    // in-flight read would land against the held PC and drop an instruction.
    // A redirect must let the read advance even under back-pressure: the PC is
    // being steered to the target this edge, and the next fetch has to read it.
    assign o_fetch_addr = pc;
    assign o_fetch_en   = redirect | ~hold;

    // ── PC register ──────────────────────────────────────────────
    // TODO(human): drive the next PC. Three mutually-exclusive cases, in
    // priority order — a taken-branch redirect, a back-pressure hold, and the
    // default sequential advance. Inputs available: `redirect`, `i_redirect_pc`,
    // `hold`, `pc`, `pc_plus_4`. (See o_fetch_en above for the redirect-beats-
    // hold principle, and how ID/MEM order i_bubble ahead of i_stall_in.)
    always_ff @(posedge i_clk) begin
        if (i_rst)
            pc <= RESET_PC;
        else begin
            if (redirect)
                pc <= i_redirect_pc;
            else if (hold)
                pc <= pc;
            else
                pc <= pc_plus_4;
        end
    end

    // ── IF1/IF2 register ─────────────────────────────────────────
    // Carries this fetch's PC / next_PC / valid to IF2 so it can pair the BRAM
    // word (which lands next cycle) with the address that produced it. Frozen
    // under back-pressure so the held word stays matched to o_pc. A redirect
    // bubbles it: the fetch in flight this cycle is the branch shadow and must
    // be discarded — the same flush IF2 and ID apply to their own slots.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid <= 1'b0;
        end else if (redirect || i_flush) begin
            o_valid <= 1'b0;            // discard the wrong-path fetch (branch shadow or fault flush)
        end else if (!hold) begin
            o_pc      <= pc;
            o_next_pc <= pc_plus_4;
            o_valid   <= 1'b1;
        end
    end

    // ── Assertion (sim-only; stripped at synth) ──────────────────
    // A redirect must discard the in-flight (branch-shadow) fetch: the
    // IF1/IF2 slot is a bubble the next cycle. Guards a future reorder that
    // would let the wrong-path fetch survive the flush and reach IF2.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (redirect || i_flush) |=> !o_valid)
        else $error("penumbra2_if1_stage: redirect/flush did not bubble the IF1/IF2 slot");

endmodule
