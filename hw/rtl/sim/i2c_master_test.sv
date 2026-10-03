// i2c_master_test — module-test harness for the I2C master.
//
// Slows the clock so a step, a quarter of the SCL period, is ten cycles:
// a testbench then runs whole transactions in a few thousand edges, and
// one cycle is 250 ns, fine enough to hold the bus to the standard-mode
// minimums it measures.

module i2c_master_test (
    input  logic       i_clk,
    input  logic       i_rst,

    input  logic       i_cmd_valid,
    output logic       o_cmd_ready,
    input  logic       i_cmd_start,
    input  logic       i_cmd_stop,
    input  logic       i_cmd_read,
    input  logic [7:0] i_cmd_wdata,
    input  logic       i_cmd_nack,

    output logic       o_rsp_valid,
    output logic [7:0] o_rsp_rdata,
    output logic       o_rsp_nack,
    output logic       o_rsp_error,

    output logic       o_scl,
    input  logic       i_scl,
    output logic       o_sda,
    input  logic       i_sda
);

    i2c_master #(
        .CLK_HZ (4_000_000),
        .SCL_HZ (100_000)
    ) u_i2c (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_cmd_valid (i_cmd_valid),
        .o_cmd_ready (o_cmd_ready),
        .i_cmd_start (i_cmd_start),
        .i_cmd_stop  (i_cmd_stop),
        .i_cmd_read  (i_cmd_read),
        .i_cmd_wdata (i_cmd_wdata),
        .i_cmd_nack  (i_cmd_nack),
        .o_rsp_valid (o_rsp_valid),
        .o_rsp_rdata (o_rsp_rdata),
        .o_rsp_nack  (o_rsp_nack),
        .o_rsp_error (o_rsp_error),
        .o_scl   (o_scl),
        .i_scl       (i_scl),
        .o_sda   (o_sda),
        .i_sda       (i_sda)
    );

endmodule
