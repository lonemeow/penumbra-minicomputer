// ps2_test — module-test harness for the PS/2 port.
//
// The port alone, on a bus clock of CLK_HZ. The testbench reads the
// rate back from o_clk_hz and keeps the device's times to it, so one
// testbench runs the port at any rate. The default is a slow clock,
// where the fixed few cycles of synchroniser and output flops weigh
// most against the protocol's times.

module ps2_test #(
    parameter int CLK_HZ = 25_000_000
)(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack,
    output logic        o_irq,

    output logic        o_ps2_clk,
    input  logic        i_ps2_clk,
    output logic        o_ps2_data,
    input  logic        i_ps2_data,

    output logic [31:0] o_clk_hz    // the rate the port is told i_clk has
);

    assign o_clk_hz = CLK_HZ;

    ps2 #(
        .CLK_HZ (CLK_HZ)
    ) u_ps2 (
        .i_clk      (i_clk),
        .i_rst      (i_rst),
        .i_addr     (i_addr),
        .i_wdata    (i_wdata),
        .i_we       (i_we),
        .i_re       (i_re),
        .o_rdata    (o_rdata),
        .o_ack      (o_ack),
        .o_irq      (o_irq),
        .o_ps2_clk  (o_ps2_clk),
        .i_ps2_clk  (i_ps2_clk),
        .o_ps2_data (o_ps2_data),
        .i_ps2_data (i_ps2_data)
    );

endmodule
