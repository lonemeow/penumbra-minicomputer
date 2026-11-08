// penumbra3_if1_stage -- Penumbra/3 instruction fetch, first half.
//
// Owns the PC register. Each cycle it drives the fetch address (PC) to the
// I-cache BRAM combinationally -- the BRAM samples it at this edge and presents
// the word during IF2 next cycle -- and registers this fetch's PC, next_PC, and
// valid bit into the IF1/IF2 register so IF2 can pair that word with the
// address that produced it.
//
// Three redirect-class actors steer or bubble the fetch:
//
//   - Taken-branch / vector-fetch / ERET redirect, and a miss's replay
//     (i_redirect): steers PC to the target and bubbles the in-flight fetch
//     (IF1 is the deepest wrong-path slot a taken branch flushes). The PC steer
//     overrides back-pressure -- the target is captured even while we are held
//     -- but its launch follows the normal hold gate: the read fires the cycle
//     the hold falls, from the steered PC.
//
//   - Fault flush (i_flush): at the fault commit, bubbles the in-flight
//     (wrong-path) fetch *without* steering PC -- the handler address is not
//     known yet. The vector-fetch FSM then owns the fetch port for a couple of
//     cycles, during which IF1 is held (i_stall_in), and finally drives
//     i_redirect with the handler address it read from the vector table.
//
//   - Interrupt drain (i_fetch_stop): freezes PC at the boundary (so the
//     interrupt unit can capture it as EPC) without steering. It bubbles the
//     IF1/IF2 output only when the fetch is *advancing*; if the front end is
//     stalled with a valid, un-consumed fetch in that register, the register
//     holds so the drain consumes it rather than dropping it (EPC is the
//     next-fetch PC, one past the held fetch, so a dropped held instruction
//     would be lost).
//
// i_stall_in holds the fetch for three reasons, each read from a flop: IF2's
// slot waits for FIFO room, the L1-I is busy with an access, or the vector
// fetch is under way. A hit or a miss never reaches it: a miss steers PC back
// through i_redirect a cycle later instead.

// keep_hierarchy: hold this stage boundary through synth_ecp5 so the backward
// stall path places compactly instead of smearing across the die, and reads
// with real names in timing reports. Paired across the pipeline stages.
(* keep_hierarchy = "yes" *)
module penumbra3_if1_stage #(
    parameter logic [31:0] RESET_PC = 32'h0000_0000
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic        i_stall_in,    // hold PC + IF1/IF2: no launch

    // ── Taken-branch / vector-fetch redirect ─────────────────────
    input  logic        i_redirect,    // steer PC to the target + bubble this fetch
    input  logic [31:0] i_redirect_pc, // redirect target

    // ── Fault flush (from exception entry) ───────────────────────
    input  logic        i_flush,       // bubble the in-flight fetch without steering PC

    // ── Interrupt drain (from the interrupt unit) ────────────────
    input  logic        i_fetch_stop,  // freeze PC at the boundary + bubble, while the pipe drains

    // ── I-cache BRAM address (combinational) ─────────────────────
    output logic [31:0] o_fetch_addr,
    output logic        o_fetch_en,    // read clock-enable: freezes the BRAM with the PC under stall

    // ── IF1/IF2 register out (to IF2) ────────────────────────────
    output logic [31:0] o_pc,
    output logic [31:0] o_next_pc,
    output logic        o_valid
);

    // The fetch holds -- PC unchanged, IF1/IF2 register frozen -- under
    // i_stall_in; otherwise it advances by one instruction. A redirect wins
    // over both hold and sequential advance.
    logic [31:0] pc;
    logic [31:0] pc_plus_4;
    logic        hold;
    logic        redirect;

    assign pc_plus_4 = pc + 32'd4;
    assign hold      = i_stall_in;
    assign redirect  = i_redirect;

    // Drive the current PC to the I-cache BRAM every cycle. The BRAM read
    // advances only when the fetch does, so under stall the registered output
    // freezes in lockstep with the IF1/IF2 register below. The launch gate is
    // purely ~hold. A redirect does not force its own launch; it only steers
    // PC. Keeping every redirect source out of this enable holds the fetch
    // address off the back end's stall cone.
    assign o_fetch_addr = pc;
    assign o_fetch_en   = ~hold;

    // ── PC register ──────────────────────────────────────────────
    // Priority: a redirect steers PC; otherwise a back-pressure hold or an
    // interrupt-drain fetch-stop freezes it (the fetch-stop at the boundary,
    // which the interrupt unit captures as EPC); otherwise advance sequentially.
    always_ff @(posedge i_clk) begin
        if (i_rst)
            pc <= RESET_PC;
        else begin
            if (redirect)
                pc <= i_redirect_pc;
            else if (hold || i_fetch_stop)
                pc <= pc;
            else
                pc <= pc_plus_4;
        end
    end

    // ── IF1/IF2 register ─────────────────────────────────────────
    // Carries this fetch's PC / next_PC / valid to IF2 so it can pair the BRAM
    // word (which lands next cycle) with the address that produced it. Frozen
    // under back-pressure so the held word stays matched to o_pc. A redirect or
    // fault flush bubbles it: the fetch in flight is the wrong-path shadow and
    // must be discarded.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid <= 1'b0;
        end else if (redirect || i_flush) begin
            o_valid <= 1'b0;            // discard the in-flight fetch
        end else if (!hold) begin
            if (i_fetch_stop) begin
                o_valid <= 1'b0;        // interrupt-drain bubble -- only when advancing, so a
                                        // held (stalled) valid fetch is consumed first, not dropped
            end else begin
                o_pc      <= pc;
                o_next_pc <= pc_plus_4;
                o_valid   <= 1'b1;
            end
        end
        // else (hold, no redirect/flush): the IF1/IF2 register holds -- a fetch
        // stalled here stays valid so an interrupt drain drains it rather than
        // dropping it (EPC points one past it, so a dropped fetch is lost).
    end

    // ── Assertions (sim-only; stripped at synth) ─────────────────
    // A redirect or fault flush discards the in-flight fetch unconditionally --
    // the IF1/IF2 slot is a bubble the next cycle.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (redirect || i_flush) |=> !o_valid)
        else $error("penumbra3_if1_stage: redirect/flush did not bubble the IF1/IF2 slot");
    // An interrupt-drain fetch-stop bubbles the slot only when it is advancing
    // (~hold); under back-pressure the slot holds so its valid instruction is
    // drained, not dropped.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_fetch_stop && !hold) |=> !o_valid)
        else $error("penumbra3_if1_stage: advancing fetch-stop did not bubble the IF1/IF2 slot");

endmodule
