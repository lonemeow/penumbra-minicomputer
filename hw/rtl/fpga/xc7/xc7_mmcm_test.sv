// xc7_mmcm_test — module-test harness for xc7_mmcm_pkg.
//
// Exposes computed MMCM configs so tb_xc7_mmcm_test.cpp can check the
// divider search in sim. This checks the *parameter math* only: the MMCM
// primitive is a sim black box, so producing the right silicon clock is
// still a synthesis and on-board concern. All from the Nexys Video's
// 100 MHz oscillator:
//   37.5 MHz: the gen2.5 CPU clock, the board's first real target.
//   100 MHz : the input frequency itself.
//   37.49 MHz: REJECTED, no legal divider set is exact (M would have to
//             be a multiple of 3749/8).
//   4 MHz   : REJECTED, below the slowest output (600 MHz VCO / 128).
//   500 MHz : REJECTED, above the BUFG ceiling though the MMCM reaches it.
module xc7_mmcm_test
    import xc7_mmcm_pkg::*;
(
    output logic   o37_valid,
    output int     o37_d, o37_m8, o37_o8,
    output longint o37_pfd, o37_vco, o37_out,
    output logic   o100_valid,
    output int     o100_d, o100_m8, o100_o8,
    output longint o100_pfd, o100_vco, o100_out,
    output logic   inexact_valid,
    output logic   too_slow_valid,
    output logic   too_fast_valid,
    output int     aux_750_25, aux_750_125, aux_600_125, aux_1000_500
);
    localparam longint IN_HZ = 100_000_000;
    localparam xc7_mmcm_cfg_t C37  = xc7_mmcm_compute(IN_HZ, 37_500_000);
    localparam xc7_mmcm_cfg_t C100 = xc7_mmcm_compute(IN_HZ, 100_000_000);

    assign o37_valid = C37.valid;
    assign o37_d     = C37.divclk_divide;
    assign o37_m8    = C37.mult_x8;
    assign o37_o8    = C37.out0_div_x8;
    assign o37_pfd   = C37.pfd_hz;
    assign o37_vco   = C37.vco_hz;
    assign o37_out   = C37.out0_hz;

    assign o100_valid = C100.valid;
    assign o100_d     = C100.divclk_divide;
    assign o100_m8    = C100.mult_x8;
    assign o100_o8    = C100.out0_div_x8;
    assign o100_pfd   = C100.pfd_hz;
    assign o100_vco   = C100.vco_hz;
    assign o100_out   = C100.out0_hz;

    assign inexact_valid  = xc7_mmcm_compute(IN_HZ, 37_490_000).valid;
    assign too_slow_valid = xc7_mmcm_compute(IN_HZ, 4_000_000).valid;
    assign too_fast_valid = xc7_mmcm_compute(IN_HZ, 500_000_000).valid;

    assign aux_750_25   = xc7_mmcm_aux_div(750_000_000, 25_000_000);
    assign aux_750_125  = xc7_mmcm_aux_div(750_000_000, 125_000_000);
    assign aux_600_125  = xc7_mmcm_aux_div(600_000_000, 125_000_000);
    assign aux_1000_500 = xc7_mmcm_aux_div(1_000_000_000, 500_000_000);
endmodule
