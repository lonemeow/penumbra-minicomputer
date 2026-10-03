// nexysvideo_codec_setup_test — module-test harness for the codec setup.
//
// Runs the setup on a 1 MHz clock, so a millisecond is a thousand
// cycles and a whole bring-up, with its PLL lock and precharge waits,
// fits in a short simulation. The testbench stands in for the I2C
// master at its command port, with the codec behind it.

module nexysvideo_codec_setup_test (
    input  logic       i_clk,
    input  logic       i_rst,

    output logic       o_cmd_valid,
    input  logic       i_cmd_ready,
    output logic       o_cmd_start,
    output logic       o_cmd_stop,
    output logic       o_cmd_read,
    output logic [7:0] o_cmd_wdata,
    output logic       o_cmd_nack,
    input  logic       i_rsp_valid,
    input  logic [7:0] i_rsp_rdata,
    input  logic       i_rsp_nack,
    input  logic       i_rsp_error,

    output logic       o_busy,
    output logic       o_fault,
    output logic [7:0] o_fault_code
);

    nexysvideo_codec_setup #(
        .CLK_HZ  (1_000_000),
        .MCLK_HZ (12_500_000)
    ) u_setup (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .o_cmd_valid  (o_cmd_valid),
        .i_cmd_ready  (i_cmd_ready),
        .o_cmd_start  (o_cmd_start),
        .o_cmd_stop   (o_cmd_stop),
        .o_cmd_read   (o_cmd_read),
        .o_cmd_wdata  (o_cmd_wdata),
        .o_cmd_nack   (o_cmd_nack),
        .i_rsp_valid  (i_rsp_valid),
        .i_rsp_rdata  (i_rsp_rdata),
        .i_rsp_nack   (i_rsp_nack),
        .i_rsp_error  (i_rsp_error),
        .o_busy       (o_busy),
        .o_fault      (o_fault),
        .o_fault_code (o_fault_code)
    );

endmodule
