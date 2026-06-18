// ecp5_pll_test — module-test harness for ecp5_pll_compute.
//
// Exposes the computed EHXPLLL config for three configurations so
// tb_ecp5_pll_test.cpp can verify the divider search in sim. This checks the
// *parameter math* only — the PLL primitive is a sim black box, so producing
// the correct silicon clock is still a synth + on-board concern.
//   25 MHz : the in-use CPU clock (CPU 25, SDRAM 100) — reproduces the
//            historical hand-picked literals exactly.
//   30 MHz : must be REJECTED (valid=0). The 6/5 ratio forces fPFD = 5 MHz,
//            below PFD_MIN; flashed and confirmed dead on the board.
//   37.5 MHz: the next CPU clock that clears the fPFD floor on the 600 MHz VCO
//            (3/2 ratio, fPFD 12.5) — documents the reachable set.
module ecp5_pll_test
    import ecp5_pll_pkg::*;
(
    output logic   o25_valid,
    output int     o25_clki, o25_clkfb, o25_clkop, o25_clkos, o25_cphase,
    output logic   o30_valid,
    output logic   o37_valid,
    output int     o37_clki, o37_clkfb, o37_clkop, o37_clkos, o37_cphase,
    output longint o37_pfd, o37_clk, o37_sdram
);
    localparam ecp5_pll_cfg_t C25 = ecp5_pll_compute(25_000_000, 25_000_000, 100_000_000);
    localparam ecp5_pll_cfg_t C30 = ecp5_pll_compute(25_000_000, 30_000_000, 100_000_000);
    localparam ecp5_pll_cfg_t C37 = ecp5_pll_compute(25_000_000, 37_500_000, 100_000_000);

    assign o25_valid  = C25.valid;
    assign o25_clki   = C25.clki_div;
    assign o25_clkfb  = C25.clkfb_div;
    assign o25_clkop  = C25.clkop_div;
    assign o25_clkos  = C25.clkos_div;
    assign o25_cphase = C25.clkop_cphase;

    assign o30_valid  = C30.valid;   // expect 0: fPFD 5 MHz < PFD_MIN

    assign o37_valid  = C37.valid;
    assign o37_clki   = C37.clki_div;
    assign o37_clkfb  = C37.clkfb_div;
    assign o37_clkop  = C37.clkop_div;
    assign o37_clkos  = C37.clkos_div;
    assign o37_cphase = C37.clkop_cphase;
    assign o37_pfd    = C37.pfd_hz;
    assign o37_clk    = C37.clk_hz;
    assign o37_sdram  = C37.sdram_hz;
endmodule
