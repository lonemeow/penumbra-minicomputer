// penumbra2_perfctr — Penumbra/2 CPU performance counters (SYSDEV_CPU regs 5+).
//
// Eight free-running 32-bit counters, read back through the sysreg sideband:
//   CYCLES / INSNS_RETIRED — the two architecturally-portable counters, plus
//   the stall breakdown STALL_FUNIT / STALL_IFETCH / STALL_LOAD / STALL_STORE /
//   STALL_HAZARD / STALL_FLUSH. Counter layout and read semantics are the
//   contract in doc/system/sysregs.md (Device 1: CPU).
//
// Stall attribution is by carried cause. The core hands this block one cause
// tag per cycle — i_bcause, the cause of the bubble (or aux slot) occupying the
// commit point — alongside the retire pulse. A retiring cycle is productive and
// charged to nothing; a non-retiring cycle advances the one counter its carried
// cause selects. Because the cause was stamped where the bubble was injected and
// rode the bubble to the commit point, the charge does not depend on how far
// upstream, or how many cycles earlier, the originating stall was — so a short
// back-end stall (whose bubble outlives its stall signal) and the latency tail
// of a long one are charged to their true cause rather than leaking into the
// front-end residual, which the prior live-stall-signal attribution mis-timed.
// The six stall counters are therefore mutually exclusive, and exactly one of
// {retire, the six} advances each cycle (asserted below) — so the breakdown
// closes CYCLES = INSNS_RETIRED + Sum(stall), accounting for every cycle.
//
// The block is observational: its input is a cause the core already resolves at
// the commit point, and its outputs feed only counter flops and the read mux. It
// never drives the pipeline, so it adds no register-to-register path through the
// core. keep_hierarchy keeps the placer from scattering the counters into the
// pipeline logic; and because software reads counter *deltas* over long regions,
// a constant counting latency is invisible — the decode can be registered to
// retime off a critical path with no semantic change.

(* keep_hierarchy = "yes" *)
module penumbra2_perfctr
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Per-cycle retire + carried-cause inputs ──────────────────
    input  logic        i_insn_retired,   // a productive cycle: an instruction retired
    input  logic [BCAUSE_W-1:0] i_bcause, // cause charged on a non-retiring cycle (carried with the blocking bubble)

    // ── Sysreg read sideband (combinational; device complex captures it) ──
    input  logic [3:0]  i_sys_reg,
    output logic [31:0] o_sys_rdata
);

    // ── Stall-bucket increment enables ───────────────────────────
    // A non-retiring cycle advances exactly the one bucket its carried cause
    // selects; a retiring cycle advances none (the assertion checks both). The
    // decode is exhaustive and one-hot by construction — i_bcause is a single
    // value — so it cannot double-charge a cycle.
    logic inc_funit, inc_ifetch, inc_load, inc_store, inc_hazard, inc_flush;

    always_comb begin
        inc_load   = 1'b0;
        inc_store  = 1'b0;
        inc_funit  = 1'b0;
        inc_hazard = 1'b0;
        inc_ifetch = 1'b0;
        inc_flush  = 1'b0;

        if (!i_insn_retired) begin
            unique case (i_bcause)
                BCAUSE_LOAD:   inc_load   = 1'b1;
                BCAUSE_STORE:  inc_store  = 1'b1;
                BCAUSE_FUNIT:  inc_funit  = 1'b1;
                BCAUSE_HAZARD: inc_hazard = 1'b1;
                BCAUSE_IFETCH: inc_ifetch = 1'b1;
                // BCAUSE_FLUSH (front-end redirect / fill) is the residual; a
                // BCAUSE_NONE on a non-retiring cycle is a tagging bug (a bubble
                // reached the commit point untagged) — caught by the assertion.
                default:       inc_flush  = 1'b1;
            endcase
        end
    end

    // ── Counters ──────────────────────────────────────────────────
    logic [31:0] cnt_cycles, cnt_insns;
    logic [31:0] cnt_funit, cnt_ifetch, cnt_load, cnt_store, cnt_hazard, cnt_flush;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            cnt_cycles <= 32'b0;
            cnt_insns  <= 32'b0;
            cnt_funit  <= 32'b0;
            cnt_ifetch <= 32'b0;
            cnt_load   <= 32'b0;
            cnt_store  <= 32'b0;
            cnt_hazard <= 32'b0;
            cnt_flush  <= 32'b0;
        end else begin
            cnt_cycles <= cnt_cycles + 32'd1;
            if (i_insn_retired) cnt_insns  <= cnt_insns  + 32'd1;
            if (inc_funit)      cnt_funit  <= cnt_funit  + 32'd1;
            if (inc_ifetch)     cnt_ifetch <= cnt_ifetch + 32'd1;
            if (inc_load)       cnt_load   <= cnt_load   + 32'd1;
            if (inc_store)      cnt_store  <= cnt_store  + 32'd1;
            if (inc_hazard)     cnt_hazard <= cnt_hazard + 32'd1;
            if (inc_flush)      cnt_flush  <= cnt_flush  + 32'd1;
        end
    end

    // ── Read mux (regs 5–12; other regs read 0 — cpuid serves 0–4) ──
    always_comb begin
        case (i_sys_reg)
            SYSREG_CPU_CYCLES:        o_sys_rdata = cnt_cycles;
            SYSREG_CPU_INSNS_RETIRED: o_sys_rdata = cnt_insns;
            SYSREG_CPU_STALL_FUNIT:   o_sys_rdata = cnt_funit;
            SYSREG_CPU_STALL_IFETCH:  o_sys_rdata = cnt_ifetch;
            SYSREG_CPU_STALL_LOAD:    o_sys_rdata = cnt_load;
            SYSREG_CPU_STALL_STORE:   o_sys_rdata = cnt_store;
            SYSREG_CPU_STALL_HAZARD:  o_sys_rdata = cnt_hazard;
            SYSREG_CPU_STALL_FLUSH:   o_sys_rdata = cnt_flush;
            default:                  o_sys_rdata = 32'b0;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // Every cycle is charged to exactly one outcome: a retirement or one stall
    // bucket. With STALL_FLUSH as the residual catch-all, the six buckets plus
    // the retire pulse partition all cycles (sysregs.md — the stall counters are
    // mutually exclusive), so exactly one of the seven is high. If this fires,
    // the decode is not exhaustive-and-exclusive — two outcomes, or none,
    // claimed the cycle.
    assert property (@(posedge i_clk) disable iff (i_rst)
        $onehot({i_insn_retired, inc_funit, inc_ifetch, inc_load, inc_store,
                 inc_hazard, inc_flush}))
        else $error("penumbra2_perfctr: cycle not charged to exactly one counter");

    // A non-retiring cycle always carries a real cause. A NONE tag here means a
    // bubble reached the commit point untagged — the carried-cause chain has a
    // gap upstream — and would be silently miscounted as FLUSH above.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_insn_retired || i_bcause != BCAUSE_NONE)
        else $error("penumbra2_perfctr: non-retiring cycle with no carried cause");

endmodule
