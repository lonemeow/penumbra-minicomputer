// Frame requests for the audio device, counted down from a fixed clock.
//
// The pacing for an output stage that converts on a clock of its own,
// as a modulator does: the device consumes one frame per request, so
// the sample rate is whatever this divides CLK_HZ to.
//
// The division must be exact. The device reports SAMPLE_HZ as its RATE,
// so a remainder would play every stream off pitch by that much while
// the device claimed the nominal rate.

module audio_rate_divider #(
    parameter int CLK_HZ    = 60_000_000,
    parameter int SAMPLE_HZ = 48_000
)(
    input  logic i_clk,
    input  logic i_rst,
    output logic o_frame_req    // one i_clk pulse per frame
);

    localparam int DIV = CLK_HZ / SAMPLE_HZ;

    build_check #(
        .COND (CLK_HZ % SAMPLE_HZ == 0),
        .MSG  ("audio_rate_divider: CLK_HZ does not divide to SAMPLE_HZ")
    ) u_check_exact ();

    logic [$clog2(DIV)-1:0] count_q;

    assign o_frame_req = (count_q == ($clog2(DIV))'(DIV - 1));

    always_ff @(posedge i_clk) begin
        if (i_rst)
            count_q <= '0;
        else
            count_q <= o_frame_req ? '0 : count_q + 1'b1;
    end

endmodule
