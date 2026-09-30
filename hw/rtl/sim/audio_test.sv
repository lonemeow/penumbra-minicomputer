// audio_test — module-test harness for the CLASS_AUDIO device.
//
// Shrinks the queue and the clock ratio so a testbench can run whole
// fill-and-drain cycles in a few thousand edges. Both are discoverable
// through DEPTH and RATE, so a consumer cannot tell the difference
// beyond the numbers it reads. The ratio still has to leave the frame
// handoff a round trip inside one sample period, which is what the
// hardware's 1250 gives it with room to spare.

module audio_test (
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack,
    output logic        o_irq,

    input  logic               i_aclk,
    input  logic               i_arst,
    output logic signed [15:0] o_frame_l,
    output logic signed [15:0] o_frame_r,
    output logic               o_frame_en
);

    localparam int SAMPLE_HZ = 48_000;

    logic frame_req;

    audio_rate_divider #(
        .CLK_HZ    (3_072_000),  // 64 output clocks per frame
        .SAMPLE_HZ (SAMPLE_HZ)
    ) u_rate (
        .i_clk       (i_aclk),
        .i_rst       (i_arst),
        .o_frame_req (frame_req)
    );

    audio #(
        .DEPTH     (16),
        .SAMPLE_HZ (SAMPLE_HZ)
    ) u_audio (
        .i_clk      (i_clk),
        .i_rst      (i_rst),
        .i_addr     (i_addr),
        .i_wdata    (i_wdata),
        .i_we       (i_we),
        .i_re       (i_re),
        .o_rdata    (o_rdata),
        .o_ack      (o_ack),
        .o_irq      (o_irq),
        .i_aclk     (i_aclk),
        .i_arst     (i_arst),
        .i_frame_req(frame_req),
        .o_frame_l  (o_frame_l),
        .o_frame_r  (o_frame_r),
        .o_frame_en (o_frame_en)
    );

endmodule
