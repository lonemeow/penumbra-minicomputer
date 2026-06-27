// penumbra3_spr_file -- Penumbra/3 privileged save-state registers.
//
// Holds the pipeline-resident special-purpose registers the exception path
// reads and writes: the status register SR (S, I, NZCV), the exception PC
// (EPC), and the exception SR snapshot (ESR). USP is the banked R14 and lives
// in penumbra3_regfile (entries SB_USP/SB_SSP); the SCRn scratch SPRs live in
// penumbra3_scratch_file. Neither is held here.
//
// SR write sources, in the priority the exception model fences them:
//   - Save-state pulse (exception / interrupt entry): EPC <- saved PC,
//     ESR <- current SR, then SR.S <- 1, SR.I <- 0. One parallel cycle, no
//     push/pop. Fires at the commit point with younger instructions already
//     flushed, so it bypasses the scoreboard.
//   - ERET (drain-commit, from EX): SR <- ESR. The PC <- EPC half is the IF
//     redirect; this module exposes o_epc for it.
//   - EI / DI (drain-commit, from EX): set / clear SR.I only. EI's
//     one-instruction enable delay (ei_shadow) lives in the interrupt unit.
//   - NZCV flag commit (from WB): updates only SR[3:0].
//
// SR is not WRSPR-writable: the decoder raises WRSPR SR illegal, so software
// changes S/I only through entry / ERET / EI / DI and NZCV only through
// flag-writing ALU ops. WRSPR therefore targets only EPC / ESR here (USP routes
// to the regfile, SCRn to the scratch file), which keeps the SR next-state mux
// free of a software bulk-load leg.
//
// The SR write sources are mutually exclusive in time by construction (entry
// and ERET both land at a drained/flushed point; an inert faulting instruction
// has no flag write), but the next-state still ranks them so a future change
// cannot silently let two collide. RDSPR EPC/ESR is a combinational read; SR is
// read in EX (committed S/I from o_sr_read plus the flag-bypassed NZCV), not
// through the operand-B read port.

module penumbra3_spr_file
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // -- NZCV flag commit (from WB) -------------------------------
    input  logic        i_flag_we,
    input  logic [3:0]  i_flag_value,      // NZCV, packed as SR[3:0]

    // -- Save-state pulse (exception / interrupt entry) -----------
    input  logic        i_save_state,
    input  logic [31:0] i_save_pc,         // EPC source: faulting PC (fault) or next-fetch PC (IRQ)

    // -- ERET restore (drain-commit, from EX) ---------------------
    input  logic        i_eret,            // SR <- ESR (PC <- EPC is the IF redirect)

    // -- EI / DI (drain-commit, from EX) --------------------------
    input  logic        i_ei,              // enable interrupts: SR.I <- 1
    input  logic        i_di,              // disable interrupts: SR.I <- 0

    // -- WRSPR write (EPC / ESR; sel-routed) ----------------------
    input  logic        i_spr_we,
    input  logic [3:0]  i_spr_sel,
    input  logic [31:0] i_spr_value,

    // -- RDSPR read (combinational; EPC / ESR as operand B) -------
    input  logic [3:0]  i_rd_sel,
    output logic [31:0] o_rd_value,

    // -- Current state outputs ------------------------------------
    output logic [3:0]  o_sr_flags,        // NZCV -> EX flag-bypass committed fallback
    output logic        o_sr_s,            // supervisor bit
    output logic        o_sr_i,            // interrupt-enable bit
    output logic [31:0] o_sr_read,         // full SR word (RDSPR SR composes from this in EX)
    output logic [31:0] o_epc,
    output logic [31:0] o_esr
);

    // SR is a 32-bit word; only S(31), I(30), and NZCV(3:0) are live -- reserved
    // bits read 0.
    logic [31:0] sr, esr, epc;

    assign o_sr_s     = sr[SR_S];
    assign o_sr_i     = sr[SR_I];
    assign o_sr_flags = sr[3:0];
    assign o_sr_read  = sr;
    assign o_epc      = epc;
    assign o_esr      = esr;

    // RDSPR read mux over the SPRs this file exposes as operand B. USP/SCRn
    // resolve through the regfile / scratch read, and SR through EX, so they
    // read 0 here.
    always_comb begin
        case (i_rd_sel)
            SPR_EPC: o_rd_value = epc;
            SPR_ESR: o_rd_value = esr;
            default: o_rd_value = 32'b0;
        endcase
    end

    // -- EPC / ESR registers --------------------------------------
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

    // -- SR register ----------------------------------------------
    // Next SR value by write source, in priority order (else hold):
    //   1. i_save_state -- entry: SR.S <- 1, SR.I <- 0, NZCV/reserved unchanged.
    //   2. i_eret       -- restore from the snapshot: sr <- esr.
    //   3. i_ei / i_di  -- flip SR.I only.
    //   4. i_flag_we    -- update only NZCV: sr[3:0] <- i_flag_value.
    always_ff @(posedge i_clk) begin
        if (i_rst)
            sr <= {1'b1, 31'b0};                // reset: S=1 (supervisor), I=0, NZCV=0
        else begin
            if (i_save_state) begin
                sr[SR_S] <= 1'b1;
                sr[SR_I] <= 1'b0;
            end else if (i_eret)
                sr <= esr;
            else if (i_ei)
                sr[SR_I] <= 1'b1;          // EI: enable (the 1-insn delay is ei_shadow, external)
            else if (i_di)
                sr[SR_I] <= 1'b0;          // DI: disable immediately
            else if (i_flag_we)
                sr[3:0] <= i_flag_value;
        end
    end

    // ==============================================================
    // Assertions -- sim-only (Verilator --assert); stripped at synth.
    // ==============================================================

    // The SR-control writers (entry, ERET, EI, DI) are each fenced to a
    // drained/flushed point and must never fire together -- if they do, the
    // priority mux is silently dropping an architectural SR update.
    always_comb
        assert ($onehot0({i_save_state, i_eret, i_ei, i_di}))
            else $error("penumbra3_spr_file: multiple SR-control writers in one cycle");

    // WRSPR reaches this file only for EPC / ESR; SR is decoder-illegal and
    // USP/SCRn route elsewhere, so any other sel means a misroute.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_spr_we && i_spr_sel != SPR_EPC && i_spr_sel != SPR_ESR))
        else $error("penumbra3_spr_file: WRSPR to a non-EPC/ESR SPR (%0d)", i_spr_sel);

    // Exception entry always leaves the CPU supervisor with interrupts masked --
    // the precondition the handler and the vector-fetch FSM rely on.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_save_state |=> (sr[SR_S] && !sr[SR_I]))
        else $error("penumbra3_spr_file: save-state did not enter S=1,I=0");

    // EPC captures exactly the PC the commit point handed over.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_save_state |=> (epc == $past(i_save_pc)))
        else $error("penumbra3_spr_file: EPC did not latch the saved PC");

    // ESR captures the pre-entry SR (the snapshot is the old SR, not the
    // post-entry S=1/I=0 value).
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_save_state |=> (esr == $past(sr)))
        else $error("penumbra3_spr_file: ESR did not snapshot the pre-entry SR");

endmodule
