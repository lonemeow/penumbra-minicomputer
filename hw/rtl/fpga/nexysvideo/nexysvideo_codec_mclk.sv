// The Nexys Video codec's master clock: the board oscillator, divided.
//
// The ADAU1761's PLL makes its core clock from MCLK, and the codec's
// setup writes PLL words worked out from MCLK's frequency, so MCLK has
// to be a clock nothing retunes. The oscillator is the one clock on the
// board that is, and 12.5 MHz divides out of its 100 MHz exactly. MCLK
// is therefore a counter on the oscillator, with no clock manager: its
// only jitter is the oscillator's own, there is no lock to wait for,
// and it runs from configuration on, whatever resets the machine. The
// codec needs that last: it answers no I2C without MCLK, and it goes on
// playing through a reset.
//
// o_mclk toggles every half period's worth of oscillator cycles, so the
// division has to be even for both halves to match.

module nexysvideo_codec_mclk #(
    parameter int MCLK_HZ = 12_500_000
)(
    input  logic i_clk_100mhz,   // board oscillator
    output logic o_mclk
);

    localparam int BOARD_CLK_HZ = 100_000_000;
    localparam int HALF_CYCLES  = BOARD_CLK_HZ / (2 * MCLK_HZ);
    localparam int COUNT_W      = $clog2(HALF_CYCLES);

    build_check #(
        .COND (BOARD_CLK_HZ % (2 * MCLK_HZ) == 0 && HALF_CYCLES >= 2),
        .MSG  ("nexysvideo_codec_mclk: MCLK_HZ is not the oscillator over an even number")
    ) u_check_even ();

    // The oscillator reaches the clock managers straight from its pin;
    // fabric flops take it from a global buffer.
    logic clk;

    BUFG u_bufg (
        .I (i_clk_100mhz),
        .O (clk)
    );

    // No reset: nothing here belongs to the machine.
    logic [COUNT_W-1:0] count_q;

    always_ff @(posedge clk) begin
        if (count_q == COUNT_W'(HALF_CYCLES - 1)) begin
            count_q <= '0;
            o_mclk  <= !o_mclk;
        end else begin
            count_q <= count_q + COUNT_W'(1);
        end
    end

endmodule
