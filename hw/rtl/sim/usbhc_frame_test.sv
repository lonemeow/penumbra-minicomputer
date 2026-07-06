// Penumbra USB frame-timer test wrapper (simulation DUT)
//
// usbhc_frame with a short frame period so the testbench can exercise many
// frame boundaries — including the full 11-bit FRAME wrap — in a fast run.
// Pure pass-through otherwise.

module usbhc_frame_test (
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic        i_run,
    input  logic [1:0]  i_speed,
    output logic        o_sof_irq,
    output logic [10:0] o_frame,
    output logic        o_req,
    input  logic        i_grant,
    output logic        o_tx_start,
    output logic [3:0]  o_tx_pid,
    output logic [10:0] o_tx_field,
    output logic        o_tx_keepalive,
    input  logic        i_tx_done
);
    usbhc_frame #(
        .CLKS_PER_MS    (200)
    ) u_frame (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_run          (i_run),
        .i_speed        (i_speed),
        .o_sof_irq      (o_sof_irq),
        .o_frame        (o_frame),
        .o_req          (o_req),
        .i_grant        (i_grant),
        .o_tx_start     (o_tx_start),
        .o_tx_pid       (o_tx_pid),
        .o_tx_field     (o_tx_field),
        .o_tx_keepalive (o_tx_keepalive),
        .i_tx_done      (i_tx_done)
    );
endmodule
