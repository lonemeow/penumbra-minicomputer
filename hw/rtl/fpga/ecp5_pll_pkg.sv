// ecp5_pll_pkg — compute EHXPLLL divider parameters from target frequencies.
//
// The ULX3S board tops drive the ECP5 PLL (EHXPLLL) to produce the CPU/system
// clock (CLKOP) plus the SDRAM fabric/pin clocks (CLKOS/CLKOS2) from one shared
// VCO. This package computes the divider set at elaboration from the target
// frequencies instead of hand-picking literals — so a clock change is a
// one-number edit and the board's CLK_FREQ constant can't drift from the
// clock the PLL actually produces.
//
// PLL relationships (FEEDBK_PATH = "CLKOP"):
//   fPFD  = in_hz / CLKI_DIV
//   CLKOP = fPFD * CLKFB_DIV = in_hz * CLKFB_DIV / CLKI_DIV     (the CPU clock)
//   fVCO  = CLKOP * CLKOP_DIV                       (must be VCO_MIN..VCO_MAX)
//   CLKOS = fVCO / CLKOS_DIV                                  (the SDRAM clock)
//
// The SDRAM is the constraint-heavy output — it must be exact, and its pin
// clock (CLKOS2) carries a phase-shift table calibrated for one divider — so
// it ANCHORS the search: the VCO is taken as an integer multiple of *both*
// targets (both output dividers land exact), preferring the VCO nearest
// VCO_PREF, the value the SDRAM phase table is built around (CLKOS_DIV = 6 at
// 600 MHz / 100 MHz). The CPU clock then derives off the same VCO. A target
// whose VCO would force a different CLKOS_DIV is still returned, but the caller
// must check `clkos_div` before trusting its phase constants — the board tops
// assert it.
//
// The phase-detector frequency is floored at PFD_MIN: a target whose best
// (smallest) CLKI_DIV still can't keep fPFD at/above the datasheet minimum
// returns valid=0. An unlockable clock is then a build-time assert failure
// (the board tops check PLL.valid) instead of a dead board. `pfd_hz` is still
// surfaced so the achieved margin is visible at the call site.
//
// Worked example: 30 MHz from a 25 MHz input forces the 6/5 ratio ->
// CLKI_DIV = 5 -> fPFD = 5 MHz, half the ~10 MHz floor. This was flashed and
// confirmed dead on the ULX3S (the PLL never locks). The next CPU clock that
// keeps fPFD >= floor while holding the 600 MHz VCO is 37.5 MHz (3/2 ratio,
// fPFD 12.5) — above the current core's fmax, so 25 MHz stands until either
// fmax clears it or a second PLL stage supplies a higher reference.

package ecp5_pll_pkg;

    // ECP5 VCO band (prjtrellis): 400-800 MHz. VCO_PREF is the value the SDRAM
    // phase-shift table and the historical operating point are built around.
    localparam longint VCO_MIN  = 400_000_000;
    localparam longint VCO_MAX  = 800_000_000;
    localparam longint VCO_PREF = 600_000_000;
    localparam longint DIV_MAX  = 128;        // EHXPLLL divider field limit
    localparam longint PFD_MIN  = 10_000_000; // datasheet phase-detector floor

    typedef struct packed {
        int     clki_div;
        int     clkfb_div;
        int     clkop_div;
        int     clkos_div;      // serves both CLKOS and CLKOS2 (same SDRAM freq)
        int     clkop_cphase;   // = clkop_div - 1  (0 deg on CLKOP)
        int     clkos_cphase;   // = clkos_div - 1  (0 deg on CLKOS)
        longint vco_hz;
        longint clk_hz;         // achieved CPU clock
        longint sdram_hz;       // achieved SDRAM clock
        longint pfd_hz;         // achieved phase-detector input frequency
        bit     valid;          // a legal solution was found
    } ecp5_pll_cfg_t;

    // Compute the EHXPLLL config for (in_hz -> cpu_hz, sdram_hz). Constant-
    // folded at elaboration; valid=0 means no legal divider set exists, which
    // a caller must treat as a hard error (the tops assert valid).
    function automatic ecp5_pll_cfg_t ecp5_pll_compute(
        longint in_hz, longint cpu_hz, longint sdram_hz);
        ecp5_pll_cfg_t r;
        longint vco, vdist, best_dist, fb;
        longint op, os, clki, clki_found, clkfb_found;

        r         = '0;
        best_dist = 0;

        // Output dividers: choose CLKOP_DIV (CPU) and CLKOS_DIV (SDRAM) so one
        // VCO serves both exactly. The equal-VCO constraint cpu*op == sdram*os
        // selects the legal pairs; the band check keeps fVCO in range.
        for (op = 1; op <= DIV_MAX; op = op + 1) begin
            for (os = 1; os <= DIV_MAX; os = os + 1) begin
                vco = cpu_hz * op;
                if ((cpu_hz * op == sdram_hz * os) &&
                    (vco >= VCO_MIN) && (vco <= VCO_MAX)) begin

                    // Feedback: CLKOP (= cpu_hz) = in_hz * CLKFB_DIV / CLKI_DIV.
                    // Take the smallest CLKI_DIV with an integer CLKFB_DIV — the
                    // smallest divider gives the highest fPFD (best loop margin).
                    clki_found  = 0;
                    clkfb_found = 0;
                    for (clki = 1; clki <= DIV_MAX; clki = clki + 1) begin
                        if (clki_found == 0 && (cpu_hz * clki) % in_hz == 0) begin
                            fb = (cpu_hz * clki) / in_hz;
                            if (fb >= 1 && fb <= DIV_MAX) begin
                                clki_found  = clki;
                                clkfb_found = fb;
                            end
                        end
                    end

                    // Prefer the VCO nearest VCO_PREF (keeps SDRAM at its
                    // calibrated divider; matches the historical 600 MHz point).
                    // Reject sub-floor fPFD: clki_found is already the smallest
                    // legal CLKI_DIV (highest fPFD), so if it can't clear the
                    // floor, no divider set can — leave valid=0.
                    if (clki_found != 0 && (in_hz / clki_found) >= PFD_MIN) begin
                        vdist = (vco > VCO_PREF) ? (vco - VCO_PREF) : (VCO_PREF - vco);
                        if (!r.valid || vdist < best_dist) begin
                            best_dist      = vdist;
                            r.valid        = 1'b1;
                            r.clki_div     = int'(clki_found);
                            r.clkfb_div    = int'(clkfb_found);
                            r.clkop_div    = int'(op);
                            r.clkos_div    = int'(os);
                            r.clkop_cphase = int'(op - 1);
                            r.clkos_cphase = int'(os - 1);
                            r.vco_hz       = vco;
                            r.clk_hz       = in_hz * clkfb_found / clki_found;
                            r.sdram_hz     = vco / os;
                            r.pfd_hz       = in_hz / clki_found;
                        end
                    end
                end
            end
        end
        return r;
    endfunction

    // Divider for an auxiliary CLKOS3 output off the already-chosen
    // VCO (e.g. the USB host's 60 MHz).  Exact-only, like every output
    // this package computes: 0 means the VCO cannot serve the target,
    // and the caller must treat that as a hard error rather than run
    // the output detuned.
    function automatic int ecp5_pll_aux_div(longint vco_hz, longint aux_hz);
        if (aux_hz <= 0 || (vco_hz % aux_hz) != 0 ||
            (vco_hz / aux_hz) > DIV_MAX)
            return 0;
        return int'(vco_hz / aux_hz);
    endfunction

endpackage
