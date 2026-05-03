// Penumbra CPU performance counters.
//
// Lives inside cpu_core, exposes its read interface as part of SYSDEV_CPU
// (regs 5+).  Free-running 32-bit counters, no atomic snapshot — software
// reads each register independently.  At 25 MHz the cycles counter
// wraps every ~2.9 minutes, which is fine for benchmark-scoped reads.
//
// Each counter increments combinationally from its event signal:
//   cycles        — every CPU clock except during reset
//   insns_retired — pulse on ir_valid (one instruction has been fetched
//                   and dispatched).  In the absence of exceptions this
//                   matches the "retired" notion exactly; on exception
//                   the dispatched instruction does not actually commit,
//                   but Stage 0a accepts that imprecision — Stage 0b
//                   will refine the event-signal definitions across the
//                   full counter set.
//
// Reset clears all counters to 0; this is the only reset path.  Software
// gets deltas by reading-before / reading-after a measured region.

module cpu_perfctr
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // Event signals from cpu_core
    input  logic        i_insn_retired,

    // Read-only sysreg interface (subset of SYSDEV_CPU regs)
    input  logic [3:0]  i_sys_reg,
    output logic [31:0] o_sys_rdata
);

    logic [31:0] cycles_cnt;
    logic [31:0] insns_cnt;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            cycles_cnt <= 32'b0;
            insns_cnt  <= 32'b0;
        end else begin
            cycles_cnt <= cycles_cnt + 32'd1;
            if (i_insn_retired)
                insns_cnt <= insns_cnt + 32'd1;
        end
    end

    always_comb begin
        case (i_sys_reg)
            SYSREG_CPU_CYCLES:        o_sys_rdata = cycles_cnt;
            SYSREG_CPU_INSNS_RETIRED: o_sys_rdata = insns_cnt;
            default:                  o_sys_rdata = 32'b0;
        endcase
    end

endmodule
