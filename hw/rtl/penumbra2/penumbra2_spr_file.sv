// penumbra2_spr_file — Penumbra/2 privileged save-state registers.
//
// Holds the pipeline-resident special-purpose registers the exception path
// reads and writes: the status register SR (S, I, NZCV), the exception PC
// (EPC), and the exception SR snapshot (ESR). USP is the banked R14 and lives
// in the register file (scoreboard entries SB_USP/SB_SSP); the SCRn scratch
// SPRs are added with the TLB-miss fast path — neither is held here.
//
// Write sources, by the order the exception model fences them:
//   - Save-state pulse (exception / interrupt entry): EPC <- saved PC,
//     ESR <- current SR, then SR.S <- 1, SR.I <- 0. One cycle, parallel,
//     no push/pop sequence. Fires at the commit point with the pipeline
//     already flushed of younger instructions, so it bypasses the scoreboard.
//   - ERET (drain-commit, from EX): SR <- ESR. The PC <- EPC half is the IF
//     redirect; this module exposes o_epc for it.
//   - WRSPR (sel-routed): EPC / ESR / SR. SR is the bulk-load (sanitised to
//     the live bits); EPC/ESR are plain writes.
//   - NZCV flag commit (from WB): updates only SR[3:0].
//
// These sources are mutually exclusive in time by construction (entry and ERET
// both happen at a drained/flushed point; an inert faulting instruction has no
// flag write), but the SR next-state still ranks them so a future change can't
// silently let two collide. RDSPR is a combinational read of the held SPRs.

module penumbra2_spr_file
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── NZCV flag commit (from WB) ───────────────────────────────
    input  logic        i_flag_we,
    input  logic [3:0]  i_flag_value,      // NZCV, packed as SR[3:0]

    // ── Save-state pulse (exception / interrupt entry) ───────────
    input  logic        i_save_state,
    input  logic [31:0] i_save_pc,         // EPC source: faulting PC (fault) or next-fetch PC (IRQ)

    // ── ERET restore (drain-commit, from EX) ─────────────────────
    input  logic        i_eret,            // SR <- ESR (PC <- EPC is the IF redirect)

    // ── WRSPR write (sel-routed) ─────────────────────────────────
    input  logic        i_spr_we,
    input  logic [3:0]  i_spr_sel,
    input  logic [31:0] i_spr_value,

    // ── RDSPR read (combinational) ───────────────────────────────
    input  logic [3:0]  i_rd_sel,
    output logic [31:0] o_rd_value,

    // ── Current state outputs ────────────────────────────────────
    output logic [3:0]  o_sr_flags,        // NZCV → EX flag-bypass committed fallback
    output logic        o_sr_s,            // supervisor bit
    output logic        o_sr_i,            // interrupt-enable bit
    output logic [31:0] o_sr_read,         // full SR word (RDSPR SR)
    output logic [31:0] o_epc,
    output logic [31:0] o_esr
);

    // SR is stored as a 32-bit word; only S(31), I(30), and NZCV(3:0) are
    // live — reserved bits read 0.
    logic [31:0] sr, esr, epc;

    assign o_sr_s     = sr[SR_S];
    assign o_sr_i     = sr[SR_I];
    assign o_sr_flags = sr[3:0];
    assign o_sr_read  = sr;
    assign o_epc      = epc;
    assign o_esr      = esr;

    // RDSPR read mux over the SPRs this file holds. USP/SCRn resolve through
    // the integration's read mux (regfile / scratch), so they read 0 here.
    always_comb begin
        case (i_rd_sel)
            SPR_EPC: o_rd_value = epc;
            SPR_ESR: o_rd_value = esr;
            SPR_SR:  o_rd_value = sr;
            default: o_rd_value = 32'b0;
        endcase
    end

    // A bulk SR load (WRSPR SR, ERET) keeps only the live bits — reserved
    // bits stay 0 regardless of what software wrote.
    function automatic logic [31:0] sr_sanitize(input logic [31:0] w);
        sr_sanitize          = '0;
        sr_sanitize[SR_S]    = w[SR_S];
        sr_sanitize[SR_I]    = w[SR_I];
        sr_sanitize[3:0]     = w[3:0];
    endfunction

    // ── EPC / ESR registers ──────────────────────────────────────
    // Save-state writes both in parallel (ESR snapshots the pre-entry SR);
    // otherwise a WRSPR targeting either lands.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            epc <= 32'b0;
            esr <= 32'b0;
        end else if (i_save_state) begin
            epc <= i_save_pc;
            esr <= sr;                          // pre-entry SR snapshot
        end else if (i_spr_we) begin
            if (i_spr_sel == SPR_EPC) epc <= i_spr_value;
            if (i_spr_sel == SPR_ESR) esr <= i_spr_value;
        end
    end

    // ── SR register ──────────────────────────────────────────────
    // TODO(human): drive the next SR value. Four write sources, highest
    // priority first, else hold:
    //   1. i_save_state — exception entry: SR.S <- 1, SR.I <- 0, NZCV and the
    //      reserved bits unchanged. (SR_S / SR_I are the bit indices; sr[3:0]
    //      is NZCV.)
    //   2. i_eret — restore from the snapshot: sr <- esr.
    //   3. i_spr_we && i_spr_sel == SPR_SR — bulk load: sr <- sr_sanitize(i_spr_value).
    //   4. i_flag_we — update only NZCV: sr[3:0] <- i_flag_value.
    always_ff @(posedge i_clk) begin
        if (i_rst)
            sr <= {1'b1, 31'b0};                // reset: S=1 (supervisor), I=0, NZCV=0
        else begin
            if (i_save_state) begin
                sr[SR_S] <= 1'b1;
                sr[SR_I] <= 1'b0;
            end else if (i_eret)
                sr <= esr;
            else if (i_spr_we && i_spr_sel == SPR_SR)
                sr <= sr_sanitize(i_spr_value);
            else if (i_flag_we)
                sr[3:0] <= i_flag_value;
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // The three bulk SR writers (entry, ERET, WRSPR-SR) are fenced to
    // drained/flushed points and must never fire together — if they do, the
    // priority mux is silently dropping one architectural SR update.
    always_comb begin
        assert ($onehot0({i_save_state, i_eret,
                          i_spr_we && i_spr_sel == SPR_SR}))
            else $error("penumbra2_spr_file: multiple bulk SR writers in one cycle");
    end

    // Exception entry always leaves the CPU in supervisor mode with interrupts
    // masked — the precondition the handler and the vector-fetch FSM rely on.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_save_state |=> (sr[SR_S] && !sr[SR_I]))
        else $error("penumbra2_spr_file: save-state did not enter S=1,I=0");

    // EPC captures exactly the PC the commit point handed over.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_save_state |=> (epc == $past(i_save_pc)))
        else $error("penumbra2_spr_file: EPC did not latch the saved PC");

    // ESR captures the pre-entry SR (the snapshot is the *old* SR, not the
    // post-entry S=1/I=0 value).
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_save_state |=> (esr == $past(sr)))
        else $error("penumbra2_spr_file: ESR did not snapshot the pre-entry SR");

endmodule
