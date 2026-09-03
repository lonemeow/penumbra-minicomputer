// audio_sigma_delta_test — module-test harness for the audio modulator.
//
// One instance per ladder width the ULX3S can drive, all fed the same
// sample, so the testbench covers the whole set from one stimulus.
// Quantizer step, integrator headroom and input scaling all derive from
// OUT_W, so each width is a distinct design point.

module audio_sigma_delta_test (
    input  logic               i_clk,
    input  logic               i_rst,
    input  logic signed [15:0] i_sample,
    output logic [0:0]         o_code1,
    output logic [1:0]         o_code2,
    output logic [2:0]         o_code3,
    output logic [3:0]         o_code4
);

    audio_sigma_delta #(
        .SAMPLE_W (16),
        .OUT_W    (1)
    ) u_sd1 (
        .i_clk    (i_clk),
        .i_rst    (i_rst),
        .i_sample (i_sample),
        .o_code   (o_code1)
    );

    audio_sigma_delta #(
        .SAMPLE_W (16),
        .OUT_W    (2)
    ) u_sd2 (
        .i_clk    (i_clk),
        .i_rst    (i_rst),
        .i_sample (i_sample),
        .o_code   (o_code2)
    );

    audio_sigma_delta #(
        .SAMPLE_W (16),
        .OUT_W    (3)
    ) u_sd3 (
        .i_clk    (i_clk),
        .i_rst    (i_rst),
        .i_sample (i_sample),
        .o_code   (o_code3)
    );

    audio_sigma_delta #(
        .SAMPLE_W (16),
        .OUT_W    (4)
    ) u_sd4 (
        .i_clk    (i_clk),
        .i_rst    (i_rst),
        .i_sample (i_sample),
        .o_code   (o_code4)
    );

endmodule
