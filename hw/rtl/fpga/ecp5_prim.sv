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
    parameter CLKOS2_DIV    = 1,
    parameter CLKOS2_ENABLE = "DISABLED",
    parameter CLKOS2_CPHASE = 0,
    parameter CLKOS2_FPHASE = 0,
    parameter CLKOS3_DIV    = 1,
    parameter CLKOS3_ENABLE = "DISABLED",
    parameter CLKOS3_CPHASE = 0,
    parameter CLKOS3_FPHASE = 0,
    parameter FEEDBK_PATH   = "CLKOP"
)(
    input  CLKI,
    input  CLKFB,
    output CLKOP,
    output CLKOS,
    output CLKOS2,
    output CLKOS3,
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
    // For lint: pretend outputs follow CLKI, LOCK always high.  The
    // real Lattice primitive synthesises distinct phase-shifted clocks
    // here; for Verilator linting we only need outputs to drive
    // something so downstream logic doesn't see "undriven" warnings.
    assign CLKOP  = CLKI;
    assign CLKOS  = CLKI;
    assign CLKOS2 = CLKI;
    assign CLKOS3 = CLKI;
    assign LOCK   = 1'b1;
endmodule

// ── ODDRX1F: SDR clock-out / data-out from the I/O cell ─────
// Used by sdram_phy_ecp5 to forward the SDRAM clock through an
// IOB-resident toggling register.  For lint, model as a simple
// alternating output driven by SCLK rising/falling.
module ODDRX1F (
    input  D0,
    input  D1,
    input  SCLK,
    input  RST,
    output Q
);
    reg q_pos, q_neg;
    always @(posedge SCLK) q_pos <= D0;
    always @(negedge SCLK) q_neg <= D1;
    assign Q = SCLK ? q_pos : q_neg;
endmodule

// ── DCCA: Dynamic Clock Buffer, Always-on ───────────────────
// Used by ulx3s_penumbra1_top to promote high-fanout signals (e.g. rst)
// onto global clock nets via the ECP5 clock distribution
// network.  CE=1 keeps the buffer transparent; CLKO is the
// same logical value as CLKI but routed through a global net
// rather than general fabric.  For lint, model as a simple
// CE-gated buffer.
module DCCA (
    input  CLKI,
    input  CE,
    output CLKO
);
    assign CLKO = CE ? CLKI : 1'b0;
endmodule
