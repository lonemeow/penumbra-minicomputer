// Penumbra CPU performance counters.
//
// Lives inside cpu_core, exposes its read interface as part of SYSDEV_CPU
// (regs 5+).  Free-running 32-bit counters, no atomic snapshot — software
// reads each register independently.  At 25 MHz the cycles counter
// wraps every ~2.9 minutes, which is fine for benchmark-scoped reads.
//
// Counters:
//   cycles        — every CPU clock except during reset
//   insns_retired — pulse on i_insn_retired (one instruction dispatched)
//   stall_funit   — cycles stalled on a multi-cycle execution unit (divmul)
//   stall_ifetch  — cycles stalled waiting on instruction-fetch memory
//   stall_load    — cycles stalled on a data read miss-fill
//   stall_store   — cycles stalled on a data write completing downstream
//
// The four stall events are mutually exclusive (enforced at their source
// in the sequencer), so their sum is the total cycles the core spent
// unable to make forward progress.  cycles − Σstall is productive
// fetch/execute work; cycles / insns_retired is effective CPI.
//
// Each counter increments combinationally from its event signal.  Reset
// clears all counters to 0; software gets deltas by reading-before /
// reading-after a measured region.

module cpu_perfctr
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // Event signals from cpu_core
    input  logic        i_insn_retired,
    input  logic        i_stall_funit,
    input  logic        i_stall_ifetch,
    input  logic        i_stall_load,
    input  logic        i_stall_store,

    // Read-only sysreg interface (subset of SYSDEV_CPU regs)
    input  logic [3:0]  i_sys_reg,
    output logic [31:0] o_sys_rdata
);

    logic [31:0] cycles_cnt;
    logic [31:0] insns_cnt;
    logic [31:0] stall_funit_cnt;
    logic [31:0] stall_ifetch_cnt;
    logic [31:0] stall_load_cnt;
    logic [31:0] stall_store_cnt;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            cycles_cnt       <= 32'b0;
            insns_cnt        <= 32'b0;
            stall_funit_cnt  <= 32'b0;
            stall_ifetch_cnt <= 32'b0;
            stall_load_cnt   <= 32'b0;
            stall_store_cnt  <= 32'b0;
        end else begin
            cycles_cnt <= cycles_cnt + 32'd1;
            if (i_insn_retired) insns_cnt        <= insns_cnt        + 32'd1;
            if (i_stall_funit)  stall_funit_cnt  <= stall_funit_cnt  + 32'd1;
            if (i_stall_ifetch) stall_ifetch_cnt <= stall_ifetch_cnt + 32'd1;
            if (i_stall_load)   stall_load_cnt   <= stall_load_cnt   + 32'd1;
            if (i_stall_store)  stall_store_cnt  <= stall_store_cnt  + 32'd1;
        end
    end

    always_comb begin
        case (i_sys_reg)
            SYSREG_CPU_CYCLES:        o_sys_rdata = cycles_cnt;
            SYSREG_CPU_INSNS_RETIRED: o_sys_rdata = insns_cnt;
            SYSREG_CPU_STALL_FUNIT:   o_sys_rdata = stall_funit_cnt;
            SYSREG_CPU_STALL_IFETCH:  o_sys_rdata = stall_ifetch_cnt;
            SYSREG_CPU_STALL_LOAD:    o_sys_rdata = stall_load_cnt;
            SYSREG_CPU_STALL_STORE:   o_sys_rdata = stall_store_cnt;
            default:                  o_sys_rdata = 32'b0;
        endcase
    end

endmodule
