// Nexys Video DDR3 clocks: the four clocks the DDR3 controller runs on
//
// One PLL off the 100 MHz oscillator, at the rates the controller's
// upstream Nexys Video demonstration calibrates at on this board:
//   o_ctrl_clk     83.333 MHz  the controller, a quarter of the DDR3 clock
//   o_ddr3_clk    333.333 MHz  the DDR3 clock (DDR3-667)
//   o_ddr3_clk_90 333.333 MHz  the same, 90 degrees later, which the PHY
//                              launches DQS against where the bank's pins
//                              have no output delay line
//   o_ref_clk     200 MHz      the IDELAYCTRL reference
// A 1000 MHz VCO gives all four with integer dividers. The rates are the
// controller's configuration, fixed here rather than derived: the DDR3
// timing the controller computes, and what calibration and the
// self-test have proven, are both for exactly these.
//
// The PLL is never reset: its lock follows the oscillator alone, and
// whoever needs the clocks steady waits on o_locked.

module nexysvideo_ddr3_clk (
    input  logic i_clk_100mhz,
    output logic o_ctrl_clk,
    output logic o_ddr3_clk,
    output logic o_ddr3_clk_90,
    output logic o_ref_clk,
    output logic o_locked
);
    logic ctrl_clk_raw, ddr3_clk_raw, ddr3_clk_90_raw, ref_clk_raw;
    logic clkfb;

    // CLKFBOUT feeds CLKFBIN directly (internal compensation): nothing
    // needs these clocks phase-aligned to the oscillator.
    /* verilator lint_off PINCONNECTEMPTY */
    PLLE2_ADV #(
        .BANDWIDTH          ("OPTIMIZED"),
        .COMPENSATION       ("INTERNAL"),
        .STARTUP_WAIT       ("FALSE"),
        .CLKIN1_PERIOD      (10.000),
        .DIVCLK_DIVIDE      (1),
        .CLKFBOUT_MULT      (10),
        .CLKFBOUT_PHASE     (0.000),
        .CLKOUT0_DIVIDE     (12),
        .CLKOUT0_PHASE      (0.000),
        .CLKOUT0_DUTY_CYCLE (0.500),
        .CLKOUT1_DIVIDE     (3),
        .CLKOUT1_PHASE      (0.000),
        .CLKOUT1_DUTY_CYCLE (0.500),
        .CLKOUT2_DIVIDE     (5),
        .CLKOUT2_PHASE      (0.000),
        .CLKOUT2_DUTY_CYCLE (0.500),
        .CLKOUT3_DIVIDE     (3),
        .CLKOUT3_PHASE      (90.000),
        .CLKOUT3_DUTY_CYCLE (0.500)
    ) u_pll (
        .CLKIN1   (i_clk_100mhz),
        .CLKIN2   (1'b0),
        .CLKINSEL (1'b1),
        .CLKFBIN  (clkfb),
        .CLKFBOUT (clkfb),
        .CLKOUT0  (ctrl_clk_raw),
        .CLKOUT1  (ddr3_clk_raw),
        .CLKOUT2  (ref_clk_raw),
        .CLKOUT3  (ddr3_clk_90_raw),
        .CLKOUT4  (),
        .CLKOUT5  (),
        .LOCKED   (o_locked),
        .RST      (1'b0),
        .PWRDWN   (1'b0),
        .DCLK     (1'b0),
        .DEN      (1'b0),
        .DWE      (1'b0),
        .DADDR    (7'd0),
        .DI       (16'h0000),
        .DO       (),
        .DRDY     ()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    BUFG u_ctrl_clk_bufg (
        .I (ctrl_clk_raw),
        .O (o_ctrl_clk)
    );

    BUFG u_ddr3_clk_bufg (
        .I (ddr3_clk_raw),
        .O (o_ddr3_clk)
    );

    BUFG u_ddr3_clk_90_bufg (
        .I (ddr3_clk_90_raw),
        .O (o_ddr3_clk_90)
    );

    BUFG u_ref_clk_bufg (
        .I (ref_clk_raw),
        .O (o_ref_clk)
    );

endmodule
