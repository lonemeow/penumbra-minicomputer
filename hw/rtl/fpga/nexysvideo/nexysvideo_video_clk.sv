// Nexys Video display clocks: the pixel and TMDS serial clocks
//
// An MMCM of the display's own off the 100 MHz oscillator, so the video
// rates never constrain the CPU clock or the DDR3's:
//   o_pixel_clk    25 MHz  the raster, both pixel sources and the TMDS
//                          encoders (mode 0, 640x480 at 60 Hz; 0.7%
//                          under VESA's 25.175 MHz, which monitors
//                          lock to, as on the ULX3S)
//   o_serial_clk  125 MHz  the lane serializers and the pads' DDR
//                          registers, two bits a cycle
// Both divide one VCO, so they are frequency-locked at a fixed phase,
// the clock contract video_serializer states. The VCO is 1000 MHz, the
// one frequency in the MMCM's band that divides exactly to both mode 0's
// rates and mode 1's 40 and 200 MHz (doc/internals/display.md), so a
// mode switch would rewrite only the output dividers.
//
// The MMCM is never reset: its lock follows the oscillator alone, and
// whoever needs the clocks steady waits on o_locked.

module nexysvideo_video_clk (
    input  logic i_clk_100mhz,   // board oscillator
    output logic o_pixel_clk,
    output logic o_serial_clk,   // 5x o_pixel_clk, rising edges aligned
    output logic o_locked        // MMCM lock, asynchronous to both clocks
);
    import xc7_mmcm_pkg::*;

    localparam longint BOARD_CLK_HZ = 100_000_000;
    localparam int     VCO_MULT     = 10;
    localparam longint VCO_HZ       = BOARD_CLK_HZ * VCO_MULT;
    localparam longint PIXEL_HZ     = 25_000_000;
    localparam longint SERIAL_HZ    = 5 * PIXEL_HZ;

    localparam int PIXEL_DIV  = xc7_mmcm_aux_div(VCO_HZ, PIXEL_HZ);
    localparam int SERIAL_DIV = xc7_mmcm_aux_div(VCO_HZ, SERIAL_HZ);

    build_check #(
        .COND (PIXEL_DIV != 0 && SERIAL_DIV != 0),
        .MSG  ("nexysvideo_video_clk: the VCO does not divide exactly to both video clocks")
    ) u_check_video_divs ();

    logic clkfb;
    logic serial_clk_mmcm;
    logic pixel_clk_mmcm;

    // CLKFBOUT feeds CLKFBIN directly: the video clocks need aligning
    // only to each other, which sharing the MMCM gives them, not to the
    // oscillator.
    /* verilator lint_off PINMISSING */
    MMCME2_BASE #(
        .CLKIN1_PERIOD    (1.0e9 / BOARD_CLK_HZ),
        .DIVCLK_DIVIDE    (1),
        .CLKFBOUT_MULT_F  (VCO_MULT * 1.0),
        .CLKOUT0_DIVIDE_F (SERIAL_DIV * 1.0),
        .CLKOUT1_DIVIDE   (PIXEL_DIV)
    ) u_mmcm (
        .CLKIN1   (i_clk_100mhz),
        .CLKFBIN  (clkfb),
        .CLKFBOUT (clkfb),
        .CLKOUT0  (serial_clk_mmcm),
        .CLKOUT1  (pixel_clk_mmcm),
        .LOCKED   (o_locked),
        .RST      (1'b0),
        .PWRDWN   (1'b0)
    );
    /* verilator lint_on PINMISSING */

    BUFG u_serial_clk_bufg (
        .I (serial_clk_mmcm),
        .O (o_serial_clk)
    );

    BUFG u_pixel_clk_bufg (
        .I (pixel_clk_mmcm),
        .O (o_pixel_clk)
    );

endmodule
