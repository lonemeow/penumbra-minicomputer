// Test harness: the complete text-video output chain — timing
// generator, test-pattern generator, the three TMDS lane encoders, and
// the four DDR serializers (blue/green/red data lanes plus the DVI
// clock lane) — wired as one module so a Verilator testbench can play
// the monitor: deserialize both DDR phases, character-align on the
// blanking control codes, TMDS-decode, and compare a recovered frame
// against the pattern oracle. Not a product module, but also the DUT
// the bring-up board top (ulx3s_video_test_top) synthesizes: the top
// adds only the PLL, the ODDRX1F output cells, and the pads around
// this module, so exactly the sim-verified wiring reaches the glass.

module video_chain_test (
    input  logic       i_pclk,   // pixel clock
    input  logic       i_sclk,   // serial clock — 5x pixel, same PLL
    input  logic       i_rst,
    output logic [3:0] o_d0,     // DDR high-half bits: [0]=B [1]=G [2]=R [3]=clock
    output logic [3:0] o_d1     // DDR low-half bits, same lane order
);

    // ── Pixel-domain front end ──────────────────────────────────
    logic [11:0] x, y;
    logic        de, hsync, vsync;
    logic [7:0]  r, g, b;

    video_timing u_timing (
        .i_clk   (i_pclk),
        .i_rst   (i_rst),
        .o_x     (x),
        .o_y     (y),
        .o_de    (de),
        .o_hsync (hsync),
        .o_vsync (vsync)
    );

    video_pattern_gen u_pattern (
        .i_x  (x),
        .i_y  (y),
        .i_de (de),
        .o_r  (r),
        .o_g  (g),
        .o_b  (b)
    );

    // ── TMDS lane encoders ──────────────────────────────────────
    // DVI lane roles: blue carries the sync levels in its control
    // bits; green and red tie theirs low.
    logic [9:0] tmds_b, tmds_g, tmds_r;

    video_tmds_encoder u_enc_b (
        .i_clk  (i_pclk),
        .i_rst  (i_rst),
        .i_data (b),
        .i_c0   (hsync),
        .i_c1   (vsync),
        .i_de   (de),
        .o_tmds (tmds_b)
    );

    video_tmds_encoder u_enc_g (
        .i_clk  (i_pclk),
        .i_rst  (i_rst),
        .i_data (g),
        .i_c0   (1'b0),
        .i_c1   (1'b0),
        .i_de   (de),
        .o_tmds (tmds_g)
    );

    video_tmds_encoder u_enc_r (
        .i_clk  (i_pclk),
        .i_rst  (i_rst),
        .i_data (r),
        .i_c0   (1'b0),
        .i_c1   (1'b0),
        .i_de   (de),
        .o_tmds (tmds_r)
    );

    // ── Lane serializers ────────────────────────────────────────
    // The clock lane is the same serializer fed a constant word: five
    // 1s then five 0s, LSB-first, is the pixel clock on the wire.
    localparam logic [9:0] CLOCK_LANE_WORD = 10'b0000011111;

    logic [9:0] lane_word [4];
    assign lane_word[0] = tmds_b;
    assign lane_word[1] = tmds_g;
    assign lane_word[2] = tmds_r;
    assign lane_word[3] = CLOCK_LANE_WORD;

    for (genvar i = 0; i < 4; i++) begin : g_lane
        video_serializer u_ser (
            .i_pclk (i_pclk),
            .i_sclk (i_sclk),
            .i_rst  (i_rst),
            .i_word (lane_word[i]),
            .o_d0   (o_d0[i]),
            .o_d1   (o_d1[i])
        );
    end

endmodule
