// ECP5 primitive stubs for Verilator lint
//
// Minimal blackbox modules matching the ECP5 primitives used in the
// design.  Only for `verilator --lint-only` — synthesis uses the real
// Lattice primitives via Yosys.

/* verilator lint_off DECLFILENAME */
/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off UNUSEDPARAM */

module EHXPLLL #(
    parameter CLKI_DIV      = 1,
    parameter CLKFB_DIV     = 1,
    parameter CLKOP_DIV     = 1,
    parameter CLKOP_ENABLE  = "ENABLED",
    parameter CLKOP_CPHASE  = 0,
    parameter CLKOP_FPHASE  = 0,
    parameter CLKOS_DIV     = 1,
    parameter CLKOS_ENABLE  = "DISABLED",
    parameter CLKOS_CPHASE  = 0,
    parameter CLKOS_FPHASE  = 0,
    parameter FEEDBK_PATH   = "CLKOP"
)(
    input  CLKI,
    input  CLKFB,
    output CLKOP,
    output CLKOS,
    output LOCK,
    input  RST,
    input  STDBY,
    input  PHASESEL0,
    input  PHASESEL1,
    input  PHASEDIR,
    input  PHASESTEP,
    input  PHASELOADREG,
    input  PLLWAKESYNC,
    input  ENCLKOP,
    input  ENCLKOS,
    input  ENCLKOS2,
    input  ENCLKOS3
);
    // For lint: pretend outputs follow CLKI, LOCK always high
    assign CLKOP = CLKI;
    assign CLKOS = CLKI;
    assign LOCK  = 1'b1;
endmodule
