// Test harness: the timing generator and the test-pattern generator
// wired into one top module, so a Verilator frame-dump testbench can
// drive the pixel clock and capture the full RGB image. Not a product
// module — it exists to verify the generator front end (the
// parallel-RGB seam) before the TMDS PHY and clocking exist.

module video_pattern_test (
    input  logic        i_clk,
    input  logic        i_rst,
    output logic [11:0] o_x,
    output logic [11:0] o_y,
    output logic        o_de,
    output logic        o_hsync,
    output logic        o_vsync,
    output logic [7:0]  o_r,
    output logic [7:0]  o_g,
    output logic [7:0]  o_b
);

    logic [11:0] x, y;
    logic        de;

    video_timing u_timing (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .o_x     (x),
        .o_y     (y),
        .o_de    (de),
        .o_hsync (o_hsync),
        .o_vsync (o_vsync)
    );

    video_pattern_gen u_pattern (
        .i_x  (x),
        .i_y  (y),
        .i_de (de),
        .o_r  (o_r),
        .o_g  (o_g),
        .o_b  (o_b)
    );

    assign o_x  = x;
    assign o_y  = y;
    assign o_de = de;

endmodule
