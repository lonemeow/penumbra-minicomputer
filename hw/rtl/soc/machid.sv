// Penumbra machine identity — read-only board/machine identification.
//
// Sysreg device 8 (SYSDEV_MACH). Hardwired constants describing the
// board this CPU is mounted on: a feature word, a 16-byte name string,
// and the CPU clock frequency the board's PLL supplies.
//
// Register map:
//   0      MACH_FEAT      — Machine feature flags
//   1–4    MACH_NAME0–3   — Machine name string, 16 bytes packed LE, null-padded
//   5      MACH_CPU_FREQ  — CPU clock frequency in Hz (parameterized per-board)
//   6–15   Reserved (reads as 0)
//
// Note on CPU_FREQ placement: the CPU has no idea what frequency its
// clock runs at — that's a property of the board's PLL configuration.
// Same RTL on a different board with a different PLL produces a different
// frequency, so the value belongs to machine identity, not CPU identity.
//
// Purely combinational — no clock, no state.

module machid
    import penumbra_pkg::*;
#(
    parameter logic [31:0]  MACH_FEAT_VALUE = 32'd0,

    // Machine name: 16 bytes packed LE, null-padded.
    // Default: all zeros (unnamed machine). Overridden per-board.
    parameter logic [31:0]  MACH_NAME0 = 32'h00000000,
    parameter logic [31:0]  MACH_NAME1 = 32'h00000000,
    parameter logic [31:0]  MACH_NAME2 = 32'h00000000,
    parameter logic [31:0]  MACH_NAME3 = 32'h00000000,

    // CPU clock frequency in Hz (0 = unknown). Set by board top-level
    // from its PLL configuration.
    parameter logic [31:0]  CPU_FREQ = 32'd0
)(
    input  logic [3:0]  i_sys_reg,
    output logic [31:0] o_sys_rdata
);

    always_comb begin
        case (i_sys_reg)
            SYSREG_MACH_FEAT:     o_sys_rdata = MACH_FEAT_VALUE;
            SYSREG_MACH_NAME0:    o_sys_rdata = MACH_NAME0;
            SYSREG_MACH_NAME1:    o_sys_rdata = MACH_NAME1;
            SYSREG_MACH_NAME2:    o_sys_rdata = MACH_NAME2;
            SYSREG_MACH_NAME3:    o_sys_rdata = MACH_NAME3;
            SYSREG_MACH_CPU_FREQ: o_sys_rdata = CPU_FREQ;
            default:              o_sys_rdata = 32'b0;
        endcase
    end

endmodule
