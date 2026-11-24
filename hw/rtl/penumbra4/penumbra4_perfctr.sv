// penumbra4_perfctr — the CPU device's performance counters, registers 5
// and up of sysreg device 1.
//
// The smallest form (doc/internals/penumbra4/structure.md, "Smallest
// forms"): the cycle counter and the retirement counter, free-running from
// reset. The stall-attribution counters read 0, which leaves every cycle
// that retires nothing unattributed, as the counters' contract allows
// (doc/system/sysregs.md, "Stall-attribution counters").
//
// The read is combinational; the machine registers the answer.

module penumbra4_perfctr
    import penumbra_pkg::*;
(
    input  logic          i_clk,
    input  logic          i_rst,

    input  logic [1:0]    i_retire_count,

    input  sysreg_index_t i_sysreg,
    output word_t         o_rdata
);

    word_t cycles_q, retired_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            cycles_q  <= '0;
            retired_q <= '0;
        end else begin
            cycles_q  <= cycles_q + word_t'(1);
            retired_q <= retired_q + word_t'(i_retire_count);
        end
    end

    always_comb begin
        case (i_sysreg)
            SYSREG_CPU_CYCLES:        o_rdata = cycles_q;
            SYSREG_CPU_INSNS_RETIRED: o_rdata = retired_q;
            default:                  o_rdata = '0;
        endcase
    end

endmodule
