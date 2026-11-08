// penumbra3_perfctr -- Penumbra/3 CPU performance counters: the CPU
// device's registers 5 and up, as doc/system/sysregs.md ("Performance
// Counters", "Stall-attribution counters") specifies them. Every cycle that
// retires nothing is charged to a stall, so the counts partition the cycles
// exactly (asserted below).
//
// The block only observes. Its inputs are what the core resolves at the
// commit point, and its outputs feed only counter flops and the read mux, so
// it adds no path through the core. keep_hierarchy keeps the placer from
// scattering the counters into the pipeline's logic, and since software reads
// counter differences over long stretches, a constant counting latency would
// not show: the decode can take a register to come off a critical path.

(* keep_hierarchy = "yes" *)
module penumbra3_perfctr
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Each cycle: the retire pulse and the carried cause ───────
    input  logic        i_insn_retired,   // an instruction retired this cycle
    input  bcause_e     i_bcause,         // the cause a non-retiring cycle is charged to

    // ── Sysreg read side (combinational; the device complex captures it) ──
    input  logic [3:0]  i_sys_reg,
    output logic [31:0] o_sys_rdata
);

    // ── Which stall counter a cycle advances ─────────────────────
    // A non-retiring cycle advances the one counter its cause selects, and a
    // retiring cycle none. i_bcause is a single value, so the decode is one-hot
    // by construction.
    logic inc_funit, inc_ifetch, inc_load, inc_store, inc_hazard, inc_flush;

    always_comb begin
        inc_load   = 1'b0;
        inc_store  = 1'b0;
        inc_funit  = 1'b0;
        inc_hazard = 1'b0;
        inc_ifetch = 1'b0;
        inc_flush  = 1'b0;

        if (!i_insn_retired) begin
            case (i_bcause)
                BCAUSE_LOAD:   inc_load   = 1'b1;
                BCAUSE_STORE:  inc_store  = 1'b1;
                BCAUSE_FUNIT:  inc_funit  = 1'b1;
                BCAUSE_HAZARD: inc_hazard = 1'b1;
                BCAUSE_IFETCH: inc_ifetch = 1'b1;
                // A front-end redirect or fill is the residual. BCAUSE_NONE on
                // a non-retiring cycle would be a bubble that reached the
                // commit point untagged, which the assertion below catches.
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

    // ── Read mux (regs 5–12; the CPU identity serves 0–4) ─────────
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

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // Every cycle is charged to exactly one outcome, a retirement or one stall
    // counter: with STALL_FLUSH the residual, the seven partition all cycles.
    assert property (@(posedge i_clk) disable iff (i_rst)
        $onehot({i_insn_retired, inc_funit, inc_ifetch, inc_load, inc_store,
                 inc_hazard, inc_flush}))
        else $error("penumbra3_perfctr: a cycle not charged to exactly one counter");

    // A non-retiring cycle carries a real cause. BCAUSE_NONE there means the
    // carried-cause chain has a gap upstream, and the cycle would be counted
    // as STALL_FLUSH.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_insn_retired || i_bcause != BCAUSE_NONE)
        else $error("penumbra3_perfctr: a non-retiring cycle with no carried cause");

endmodule
