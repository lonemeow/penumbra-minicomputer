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

// ── XADC: analog-to-digital converter and on-chip sensors ───
// For lint and simulation the die stays cool: no alarm, no
// over-temperature, and the DRP never answers.
module XADC #(
    parameter [15:0] INIT_40 = 16'h0000,
    parameter [15:0] INIT_41 = 16'h0000,
    parameter [15:0] INIT_42 = 16'h0800,
    parameter [15:0] INIT_43 = 16'h0000,
    parameter [15:0] INIT_44 = 16'h0000,
    parameter [15:0] INIT_45 = 16'h0000,
    parameter [15:0] INIT_46 = 16'h0000,
    parameter [15:0] INIT_47 = 16'h0000,
    parameter [15:0] INIT_48 = 16'h0000,
    parameter [15:0] INIT_49 = 16'h0000,
    parameter [15:0] INIT_4A = 16'h0000,
    parameter [15:0] INIT_4B = 16'h0000,
    parameter [15:0] INIT_4C = 16'h0000,
    parameter [15:0] INIT_4D = 16'h0000,
    parameter [15:0] INIT_4E = 16'h0000,
    parameter [15:0] INIT_4F = 16'h0000,
    parameter [15:0] INIT_50 = 16'h0000,
    parameter [15:0] INIT_51 = 16'h0000,
    parameter [15:0] INIT_52 = 16'h0000,
    parameter [15:0] INIT_53 = 16'h0000,
    parameter [15:0] INIT_54 = 16'h0000,
    parameter [15:0] INIT_55 = 16'h0000,
    parameter [15:0] INIT_56 = 16'h0000,
    parameter [15:0] INIT_57 = 16'h0000,
    parameter [15:0] INIT_58 = 16'h0000,
    parameter [15:0] INIT_59 = 16'h0000,
    parameter [15:0] INIT_5A = 16'h0000,
    parameter [15:0] INIT_5B = 16'h0000,
    parameter [15:0] INIT_5C = 16'h0000,
    parameter [15:0] INIT_5D = 16'h0000,
    parameter [15:0] INIT_5E = 16'h0000,
    parameter [15:0] INIT_5F = 16'h0000,
    parameter        SIM_DEVICE       = "7SERIES",
    parameter        SIM_MONITOR_FILE = "design.txt"
) (
    output [7:0]  ALM,
    output        BUSY,
    output [4:0]  CHANNEL,
    output [15:0] DO,
    output        DRDY,
    output        EOC,
    output        EOS,
    output        JTAGBUSY,
    output        JTAGLOCKED,
    output        JTAGMODIFIED,
    output [4:0]  MUXADDR,
    output        OT,
    input         CONVST,
    input         CONVSTCLK,
    input  [6:0]  DADDR,
    input         DCLK,
    input         DEN,
    input  [15:0] DI,
    input         DWE,
    input         RESET,
    input  [15:0] VAUXN,
    input  [15:0] VAUXP,
    input         VN,
    input         VP
);
    assign ALM          = 8'h00;
    assign BUSY         = 1'b0;
    assign CHANNEL      = 5'd0;
    assign DO           = 16'h0000;
    assign DRDY         = 1'b0;
    assign EOC          = 1'b0;
    assign EOS          = 1'b0;
    assign JTAGBUSY     = 1'b0;
    assign JTAGLOCKED   = 1'b0;
    assign JTAGMODIFIED = 1'b0;
    assign MUXADDR      = 5'd0;
    assign OT           = 1'b0;
endmodule
