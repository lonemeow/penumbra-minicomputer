// xc7_mmcm_pkg — compute MMCME2 divider parameters from target frequencies.
//
// The counterpart of ecp5_pll_pkg for Xilinx 7-series boards: a board top
// drives an MMCME2_BASE from its oscillator, and this package derives the
// dividers at elaboration from the frequency the MMCM must produce, so a
// clock change is a one-number edit and the top's CLK_FREQ cannot drift from
// the clock the MMCM actually makes. Outputs are exact or nothing: a target
// no legal divider set reaches exactly returns valid=0, and the top must
// refuse to build.
//
// MMCM relationships (CLKFBOUT fed straight back to CLKFBIN):
//   fPFD    = in_hz / D           D  = DIVCLK_DIVIDE
//   fVCO    = in_hz * M / D       M  = CLKFBOUT_MULT_F
//   CLKOUT0 = fVCO / O0           O0 = CLKOUT0_DIVIDE_F
//   CLKOUTn = fVCO / On           On = CLKOUTn_DIVIDE (n = 1..6, integer)
// M and O0 are set in steps of 1/8, so the config stores each as an
// integer count in units of 1/8 (mult_x8 = M * 8, out0_div_x8 = O0 * 8)
// and all the arithmetic stays in integers; a top passes mult_x8 / 8.0
// to the primitive's real-typed parameters. Every frequency in a valid
// config is exact, in whole hertz: out0_hz is the target, and vco_hz is
// what xc7_mmcm_aux_div divides for the other outputs.
//
// Limits are for speed grade -1 (the Nexys Video's XC7A200T-1):
// - VCO, PFD and input bands: DS181 v1.27 Table 37 (MMCM Specification).
//   Vivado confirms the VCO band in its AVAL-46 DRC, but only as a critical
//   warning, and it checks the PFD not at all: an illegal divider set can
//   still reach a bitstream, so this package's valid flag is the guard.
// - Output ceiling: the global buffer every clock output drives, not the
//   MMCM (DS181 Table 32, FMAX_BUFG 464 MHz; the MMCM reaches 800).
// - Divider ranges and steps: Vivado 2025.2's MMCME2 simulation models
//   (M 2..64 and O0 1..128 in steps of 1/8, O0 never strictly between
//   1 and 2; D 1..106; On 1..128).

package xc7_mmcm_pkg;

    localparam longint VCO_MIN = 600_000_000;
    localparam longint VCO_MAX = 1_200_000_000;
    localparam longint PFD_MIN = 10_000_000;
    localparam longint PFD_MAX = 450_000_000;
    localparam longint IN_MIN  = 10_000_000;
    localparam longint IN_MAX  = 800_000_000;
    localparam longint OUT_MAX = 464_000_000;

    localparam longint D_MAX        = 106;
    localparam longint MULT_X8_MIN  = 16;     // 2.000
    localparam longint MULT_X8_MAX  = 512;    // 64.000
    localparam longint OUT0_X8_MIN  = 8;      // 1.000
    localparam longint OUT0_X8_MAX  = 1024;   // 128.000
    localparam longint OUT0_X8_GAP_LO = 8;    // O0 in (1, 2) is illegal:
    localparam longint OUT0_X8_GAP_HI = 16;   //   8 < out0_div_x8 < 16
    localparam longint OUTN_DIV_MAX = 128;

    typedef struct packed {
        int     divclk_divide;  // D
        int     mult_x8;        // M in units of 1/8 (M * 8)
        int     out0_div_x8;    // O0 in units of 1/8 (O0 * 8)
        longint pfd_hz;
        longint vco_hz;
        longint out0_hz;        // achieved CLKOUT0 (== the target when valid)
        bit     valid;          // a legal, exact divider set was found
    } xc7_mmcm_cfg_t;

    // Compute the MMCM config for in_hz -> CLKOUT0 = target_hz. Constant-
    // folded at elaboration; valid=0 means no legal divider set reaches the
    // target exactly, which the top must treat as a hard error.
    function automatic xc7_mmcm_cfg_t xc7_mmcm_compute(
        longint in_hz, longint target_hz);
        xc7_mmcm_cfg_t r;
        longint d, d_lo, d_hi, m, o0, vco;
        r = '0;

        if (in_hz < IN_MIN || in_hz > IN_MAX || target_hz <= 0 || target_hz > OUT_MAX)
            return r;

        // fPFD = in_hz / D must stay inside the phase-detector band.
        d_lo = (in_hz + PFD_MAX - 1) / PFD_MAX;
        d_hi = in_hz / PFD_MIN;
        if (d_hi > D_MAX)
            d_hi = D_MAX;

        // Preference is the search order, and the first exact hit wins: the
        // smallest D (highest fPFD), then the highest VCO. Both lower the
        // output jitter. O0 follows from D and M, so it is computed, not
        // searched. m and o0 count in units of 1/8, like the config fields.
        for (d = d_lo; d <= d_hi; d = d + 1) begin
            if (in_hz % d != 0)
                continue;
            for (m = MULT_X8_MAX; m >= MULT_X8_MIN; m = m - 1) begin
                if ((in_hz * m) % (8 * d) != 0)
                    continue;
                vco = in_hz * m / (8 * d);
                if (vco < VCO_MIN || vco > VCO_MAX)
                    continue;
                if ((in_hz * m) % (d * target_hz) != 0)
                    continue;
                o0 = in_hz * m / (d * target_hz);
                if (o0 < OUT0_X8_MIN || o0 > OUT0_X8_MAX ||
                    (o0 > OUT0_X8_GAP_LO && o0 < OUT0_X8_GAP_HI))
                    continue;
                r.divclk_divide = int'(d);
                r.mult_x8       = int'(m);
                r.out0_div_x8   = int'(o0);
                r.pfd_hz        = in_hz / d;
                r.vco_hz        = vco;
                r.out0_hz       = target_hz;
                r.valid         = 1'b1;
                return r;
            end
        end
        return r;
    endfunction

    // Divider for an integer output (CLKOUT1..6) off the already-chosen VCO.
    // Exact-only, like CLKOUT0: 0 means the VCO cannot serve the target, and
    // the caller must treat that as a hard error rather than run the output
    // detuned.
    function automatic int xc7_mmcm_aux_div(longint vco_hz, longint aux_hz);
        if (aux_hz <= 0 || aux_hz > OUT_MAX || (vco_hz % aux_hz) != 0 ||
            (vco_hz / aux_hz) > OUTN_DIV_MAX)
            return 0;
        return int'(vco_hz / aux_hz);
    endfunction

endpackage
