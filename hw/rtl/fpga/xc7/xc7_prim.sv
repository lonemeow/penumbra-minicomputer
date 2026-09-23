// Xilinx 7-series primitive stubs for Verilator lint
//
// Minimal blackbox modules matching the 7-series primitives the board
// tops instantiate, with the ports and parameters of Vivado's own
// simulation models. Only for `verilator --lint-only`; synthesis uses
// the real primitives via Vivado.

/* verilator lint_off DECLFILENAME */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off UNUSEDPARAM */

// ── MMCME2_BASE: mixed-mode clock manager ───────────────────
// For lint, every output follows the input clock and the MMCM
// reports lock at once.
module MMCME2_BASE #(
    parameter      BANDWIDTH          = "OPTIMIZED",
    parameter real CLKFBOUT_MULT_F    = 5.000,
    parameter real CLKFBOUT_PHASE     = 0.000,
    parameter real CLKIN1_PERIOD      = 0.000,
    parameter real CLKOUT0_DIVIDE_F   = 1.000,
    parameter real CLKOUT0_DUTY_CYCLE = 0.500,
    parameter real CLKOUT0_PHASE      = 0.000,
    parameter int  CLKOUT1_DIVIDE     = 1,
    parameter real CLKOUT1_DUTY_CYCLE = 0.500,
    parameter real CLKOUT1_PHASE      = 0.000,
    parameter int  CLKOUT2_DIVIDE     = 1,
    parameter real CLKOUT2_DUTY_CYCLE = 0.500,
    parameter real CLKOUT2_PHASE      = 0.000,
    parameter int  CLKOUT3_DIVIDE     = 1,
    parameter real CLKOUT3_DUTY_CYCLE = 0.500,
    parameter real CLKOUT3_PHASE      = 0.000,
    parameter      CLKOUT4_CASCADE    = "FALSE",
    parameter int  CLKOUT4_DIVIDE     = 1,
    parameter real CLKOUT4_DUTY_CYCLE = 0.500,
    parameter real CLKOUT4_PHASE      = 0.000,
    parameter int  CLKOUT5_DIVIDE     = 1,
    parameter real CLKOUT5_DUTY_CYCLE = 0.500,
    parameter real CLKOUT5_PHASE      = 0.000,
    parameter int  CLKOUT6_DIVIDE     = 1,
    parameter real CLKOUT6_DUTY_CYCLE = 0.500,
    parameter real CLKOUT6_PHASE      = 0.000,
    parameter int  DIVCLK_DIVIDE      = 1,
    parameter real REF_JITTER1        = 0.010,
    parameter      STARTUP_WAIT       = "FALSE"
) (
    output CLKFBOUT,
    output CLKFBOUTB,
    output CLKOUT0,
    output CLKOUT0B,
    output CLKOUT1,
    output CLKOUT1B,
    output CLKOUT2,
    output CLKOUT2B,
    output CLKOUT3,
    output CLKOUT3B,
    output CLKOUT4,
    output CLKOUT5,
    output CLKOUT6,
    output LOCKED,
    input  CLKFBIN,
    input  CLKIN1,
    input  PWRDWN,
    input  RST
);
    assign CLKFBOUT  = CLKIN1;
    assign CLKFBOUTB = ~CLKIN1;
    assign CLKOUT0   = CLKIN1;
    assign CLKOUT0B  = ~CLKIN1;
    assign CLKOUT1   = CLKIN1;
    assign CLKOUT1B  = ~CLKIN1;
    assign CLKOUT2   = CLKIN1;
    assign CLKOUT2B  = ~CLKIN1;
    assign CLKOUT3   = CLKIN1;
    assign CLKOUT3B  = ~CLKIN1;
    assign CLKOUT4   = CLKIN1;
    assign CLKOUT5   = CLKIN1;
    assign CLKOUT6   = CLKIN1;
    assign LOCKED    = 1'b1;
endmodule

// ── BUFG: global clock buffer ───────────────────────────────
module BUFG (
    output O,
    input  I
);
    assign O = I;
endmodule
