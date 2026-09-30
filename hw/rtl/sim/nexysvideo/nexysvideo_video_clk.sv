// nexysvideo_video_clk — the Nexys Video's display clocks, as simulation sees them
//
// Stands in for hw/rtl/fpga/nexysvideo/nexysvideo_video_clk.sv in the
// board top's testbench, which puts this directory ahead of the board's,
// so this file shadows the MMCM wrapper by name; synthesis and fpga-lint
// use the wrapper. The ports are the wrapper's; keep the two in step.
//
// A cycle-based simulator cannot multiply a clock, and the MMCM stub
// passes the oscillator through on every output, which would run the
// serializers' two clocks at one rate. Here the serial clock is the
// oscillator and the pixel clock that divided by five, rising on the
// serial clock's rising edges: frequency-locked at a fixed phase, the
// clock contract video_serializer states, as the MMCM gives it.

module nexysvideo_video_clk (
    input  logic i_clk_100mhz,
    output logic o_pixel_clk,
    output logic o_serial_clk,
    output logic o_locked
);
    localparam int RATIO = 5;   // serial cycles per pixel cycle

    logic [2:0] phase_q;
    logic [2:0] phase_d;
    logic       pixel_q;

    initial begin
        phase_q = '0;
        pixel_q = 1'b0;
    end

    // The pixel clock is high for the first half of its period, rounded
    // up, so it rises as the phase returns to 0.
    assign phase_d = (phase_q == 3'(RATIO - 1)) ? '0 : phase_q + 3'd1;

    always_ff @(posedge i_clk_100mhz) begin
        phase_q <= phase_d;
        pixel_q <= phase_d < 3'(RATIO - RATIO / 2);
    end

    assign o_serial_clk = i_clk_100mhz;
    assign o_pixel_clk  = pixel_q;
    assign o_locked     = 1'b1;

endmodule
