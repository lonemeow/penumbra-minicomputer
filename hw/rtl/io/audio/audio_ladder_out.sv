// Resistor-ladder output stage — PCM frames to the board's ladder legs.
//
// One noise-shaping modulator per side, driving a contiguous run of
// ladder legs starting at LEG_LSB. Which legs, and how many, is a
// property of the board rather than of the device: on the ULX3S the two
// highest-value legs measure some 47 dB cleaner than all four, because
// in-band noise there tracks the current the driven legs switch. The
// measurements are in doc/TODO/hw-devices.md.
//
// Legs outside the run are held low rather than floated: floating one
// would make the ladder's Thevenin resistance depend on the code, which
// is itself a nonlinearity.
//
// The pads are registered so the tools can pack them into the IO cells,
// which is the closest the device comes to switching every leg at once.

module audio_ladder_out #(
    parameter int LEGS    = 4,   // ladder legs the board provides
    parameter int OUT_W   = 2,   // legs this configuration drives
    parameter int LEG_LSB = 0    // lowest leg driven; 0 is the largest resistor
) (
    input  logic               i_clk,
    input  logic               i_rst,
    input  logic signed [15:0] i_frame_l,
    input  logic signed [15:0] i_frame_r,
    output logic [LEGS-1:0]    o_left,
    output logic [LEGS-1:0]    o_right
);

    logic [OUT_W-1:0] code_l, code_r;

    audio_sigma_delta #(
        .SAMPLE_W (16),
        .OUT_W    (OUT_W)
    ) u_left (
        .i_clk    (i_clk),
        .i_rst    (i_rst),
        .i_sample (i_frame_l),
        .o_code   (code_l)
    );

    audio_sigma_delta #(
        .SAMPLE_W (16),
        .OUT_W    (OUT_W)
    ) u_right (
        .i_clk    (i_clk),
        .i_rst    (i_rst),
        .i_sample (i_frame_r),
        .o_code   (code_r)
    );

    logic [LEGS-1:0] left_d, right_d;

    always_comb begin
        left_d  = '0;
        right_d = '0;
        left_d [LEG_LSB +: OUT_W] = code_l;
        right_d[LEG_LSB +: OUT_W] = code_r;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_left  <= '0;
            o_right <= '0;
        end else begin
            o_left  <= left_d;
            o_right <= right_d;
        end
    end

endmodule
