// audio_multi_test — module-test harness for a multi-channel CLASS_AUDIO
// device.
//
// The single-channel harness cannot reach anything the channel count
// introduces: per-channel rate and gain reduce to one value, the mixer's
// walk visits one entry, and the control select that names the channel
// being walked never moves. Four channels is what the board builds and
// what a tracker drives, so it is what the per-channel behaviour is
// checked at.
//
// Queue and clock ratio are shrunk as in the single-channel harness, and
// for the same reason: both are discoverable through DEPTH and RATE, so
// a consumer cannot tell the difference beyond the numbers it reads.

module audio_multi_test (
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,
    output logic        o_irq,

    input  logic               i_aclk,
    input  logic               i_arst,
    output logic signed [15:0] o_frame_l,
    output logic signed [15:0] o_frame_r,
    output logic               o_frame_en
);

    audio #(
        .CHANNELS  (4),
        .DEPTH     (16),
        .SAMPLE_HZ (48_000),
        .AUDIO_HZ  (3_072_000)   // 64 output clocks per frame
    ) u_audio (
        .i_clk      (i_clk),
        .i_rst      (i_rst),
        .i_addr     (i_addr),
        .i_wdata    (i_wdata),
        .i_we       (i_we),
        .i_re       (i_re),
        .o_rdata    (o_rdata),
        .o_busy     (o_busy),
        .o_irq      (o_irq),
        .i_aclk     (i_aclk),
        .i_arst     (i_arst),
        .o_frame_l  (o_frame_l),
        .o_frame_r  (o_frame_r),
        .o_frame_en (o_frame_en)
    );

endmodule
