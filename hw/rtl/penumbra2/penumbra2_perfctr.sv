// penumbra2_perfctr — Penumbra/2 CPU performance counters (SYSDEV_CPU regs 5+).
//
// Six free-running 32-bit counters, read back through the sysreg sideband:
//   CYCLES / INSNS_RETIRED — the two architecturally-portable counters, plus
//   STALL_FUNIT / STALL_IFETCH / STALL_LOAD / STALL_STORE — the stall
//   breakdown. Counter layout and read semantics are the contract in
//   doc/system/sysregs.md (Device 1: CPU).
//
// Stall attribution is head-of-line: a cycle that retires no instruction is
// charged to the blocker of the *oldest* un-retired instruction — which, in an
// in-order pipeline, is the most-downstream stalling stage. The per-cause stall
// inputs can be asserted together (an older load filling in MEM while a younger
// divmul iterates in EX), so a single cycle is resolved to one bucket by a
// downstream-first priority: clearing an upstream stall cannot let the cycle
// retire while a downstream one still holds, so the downstream stall is the one
// that actually bound progress. A retiring instruction means the cycle was
// productive and is charged to nothing. The four stall counters are therefore
// mutually exclusive — at most one advances per cycle (asserted below) — so
// their sum is the stall total, the basis for a CPI breakdown.
//
// The block is observational: its inputs are stall signals each stage already
// generates, and its outputs feed only counter flops and the read mux. It never
// drives the pipeline, so it adds no register-to-register path through the core.
// keep_hierarchy keeps the placer from scattering the counters into the
// pipeline logic; and because software reads counter *deltas* over long
// regions, a constant counting latency is invisible — the attribution can be
// registered to retime off a critical path with no semantic change.

(* keep_hierarchy = "yes" *)
module penumbra2_perfctr
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Per-cycle event + stall inputs (head-of-line attribution) ─
    input  logic        i_insn_retired,   // a productive cycle: an instruction retired
    input  logic        i_stall_load,     // MEM holds the pipe for a load access
    input  logic        i_stall_store,    // MEM holds the pipe for a store access
    input  logic        i_stall_funit,    // EX waits on the multi-cycle execution unit (divmul)
    input  logic        i_stall_ifetch,   // IF waits on the instruction-fetch memory

    // ── Sysreg read sideband (combinational; device complex captures it) ──
    input  logic [3:0]  i_sys_reg,
    output logic [31:0] o_sys_rdata
);

    // ── Stall-bucket increment enables ───────────────────────────
    // Resolved from the (possibly overlapping) per-cause stall inputs by
    // head-of-line priority — see the module header. At most one is set per
    // cycle, and none is set on a retiring cycle (the assertion checks both).
    logic inc_funit, inc_ifetch, inc_load, inc_store;

    always_comb begin
        inc_load   = 1'b0;
        inc_store  = 1'b0;
        inc_funit  = 1'b0;
        inc_ifetch = 1'b0;

        if (!i_insn_retired) begin
            if (i_stall_load)       inc_load    = 1'b1;
            else if (i_stall_store) inc_store   = 1'b1;
            else if (i_stall_funit) inc_funit   = 1'b1;
            else if (i_stall_ifetch) inc_ifetch = 1'b1;
        end
        // A cycle matching none — a RAW hazard or a front-end-redirect bubble —
        // is a residual that charges nothing, awaiting the STALL_HAZARD /
        // STALL_FLUSH buckets.
    end

    // ── Counters ──────────────────────────────────────────────────
    logic [31:0] cnt_cycles, cnt_insns;
    logic [31:0] cnt_funit, cnt_ifetch, cnt_load, cnt_store;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            cnt_cycles <= 32'b0;
            cnt_insns  <= 32'b0;
            cnt_funit  <= 32'b0;
            cnt_ifetch <= 32'b0;
            cnt_load   <= 32'b0;
            cnt_store  <= 32'b0;
        end else begin
            cnt_cycles <= cnt_cycles + 32'd1;
            if (i_insn_retired) cnt_insns  <= cnt_insns  + 32'd1;
            if (inc_funit)      cnt_funit  <= cnt_funit  + 32'd1;
            if (inc_ifetch)     cnt_ifetch <= cnt_ifetch + 32'd1;
            if (inc_load)       cnt_load   <= cnt_load   + 32'd1;
            if (inc_store)      cnt_store  <= cnt_store  + 32'd1;
        end
    end

    // ── Read mux (regs 5–10; other regs read 0 — cpuid serves 0–4) ──
    always_comb begin
        case (i_sys_reg)
            SYSREG_CPU_CYCLES:        o_sys_rdata = cnt_cycles;
            SYSREG_CPU_INSNS_RETIRED: o_sys_rdata = cnt_insns;
            SYSREG_CPU_STALL_FUNIT:   o_sys_rdata = cnt_funit;
            SYSREG_CPU_STALL_IFETCH:  o_sys_rdata = cnt_ifetch;
            SYSREG_CPU_STALL_LOAD:    o_sys_rdata = cnt_load;
            SYSREG_CPU_STALL_STORE:   o_sys_rdata = cnt_store;
            default:                  o_sys_rdata = 32'b0;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // Head-of-line attribution charges each cycle to at most one outcome: a
    // retirement or exactly one stall bucket (sysregs.md — the stall counters
    // are mutually exclusive). A residual cycle (RAW hazard / redirect bubble)
    // charges none, which is still onehot0. If this fires, the priority logic
    // let two buckets — or a bucket and a retirement — claim the same cycle.
    assert property (@(posedge i_clk) disable iff (i_rst)
        $onehot0({i_insn_retired, inc_funit, inc_ifetch, inc_load, inc_store}))
        else $error("penumbra2_perfctr: cycle charged to multiple counters");

endmodule
