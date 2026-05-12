// SPI master unit-test wrapper.
//
// Instantiates `spi` with small, sim-friendly parameters so the
// testbench can hit FIFO/threshold/full/empty edges quickly:
//   FIFO_DEPTH = 8   → watermark = 4, full reached after 8 pushes
//   SLOW_DIV   = 1   → ~4 cycles/bit slow path (vs. 256 at the
//                      production default of 63 — for ≤400 kHz SD init)
//   FAST_DIV   = 0   → 2 cycles/bit fast path (same as production)
//
// Port list mirrors `spi.sv` verbatim — pure passthrough wrapper.
// See `hw/sim/tb_spi.cpp` for the test cases driven against this.

module spi_test
(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    output logic        o_sclk,
    output logic        o_mosi,
    input  logic        i_miso,
    output logic        o_cs0,
    output logic        o_cs1,

    output logic        o_irq
);

    spi #(
        .FIFO_DEPTH (8),
        .SLOW_DIV   (16'd1),
        .FAST_DIV   (16'd0)
    ) u_spi (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_addr  (i_addr),
        .i_wdata (i_wdata),
        .i_we    (i_we),
        .i_re    (i_re),
        .o_rdata (o_rdata),
        .o_busy  (o_busy),
        .o_sclk  (o_sclk),
        .o_mosi  (o_mosi),
        .i_miso  (i_miso),
        .o_cs0   (o_cs0),
        .o_cs1   (o_cs1),
        .o_irq   (o_irq)
    );

endmodule
