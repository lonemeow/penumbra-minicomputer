// Test wrapper: nexysvideo_ddr3_selftest at simulation scale.
//
// A 1 MHz clock makes a millisecond 1000 cycles, and 100 kbaud makes a
// bit exactly ten, so the testbench's serial receiver needs no rounding;
// the run timings shrink to match.

module nexysvideo_ddr3_selftest_test (
    input  logic        i_clk,
    input  logic        i_rst,
    output logic        o_ctrl_rst_n,
    input  logic        i_calib_complete,
    output logic        o_dbg_stb,
    output logic [6:0]  o_dbg_addr,
    input  logic        i_dbg_stall,
    input  logic        i_dbg_ack,
    input  logic [31:0] i_dbg_data,
    output logic        o_tx,
    output logic        o_testing,
    output logic        o_last_pass,
    output logic        o_mismatch_seen
);
    nexysvideo_ddr3_selftest #(
        .CLK_HZ     (1_000_000),
        .BAUD       (100_000),
        .BIST_MODE  (2),
        .HOLD_MS    (1),
        .TIMEOUT_MS (20),
        .PAUSE_MS   (2)
    ) u_dut (
        .i_clk            (i_clk),
        .i_rst            (i_rst),
        .o_ctrl_rst_n     (o_ctrl_rst_n),
        .i_calib_complete (i_calib_complete),
        .o_dbg_stb        (o_dbg_stb),
        .o_dbg_addr       (o_dbg_addr),
        .i_dbg_stall      (i_dbg_stall),
        .i_dbg_ack        (i_dbg_ack),
        .i_dbg_data       (i_dbg_data),
        .o_tx             (o_tx),
        .o_testing        (o_testing),
        .o_last_pass      (o_last_pass),
        .o_mismatch_seen  (o_mismatch_seen)
    );

endmodule
