// Penumbra display text output chain — timing generator, character
// generator, the three TMDS lane encoders, and the four DDR
// serializers, wired as one module. The character-cell sibling of
// video_chain_test: same lane structure and clocking contract, with
// the test-pattern source replaced by video_textgen. The character
// generator's outputs are already delay-matched to its 4-stage
// pipeline, so the encoders see RGB and syncs aligned by
// construction.
//
// The bring-up board top synthesizes this with the cell RAM preloaded
// from CELLS_HEX (a splash screen — no CPU exists yet); the
// closed-loop testbench plays the monitor against the same preload.
// The CPU-domain cell port is exposed for consumers that have a bus
// master; a preload-only user ties it off.

module video_text_chain #(
    parameter CELLS_HEX = "splash_cells.hex"
) (
    // ── CPU/bus domain: cell access (tie off when preload-only) ──
    input  logic        i_clk,
    input  logic        i_cell_we,
    input  logic [11:0] i_cell_addr,
    input  logic [15:0] i_cell_wdata,
    output logic [15:0] o_cell_rdata,

    // ── Pixel/serial domains ────────────────────────────────────
    input  logic       i_pclk,   // pixel clock
    input  logic       i_sclk,   // serial clock — 5x pixel, same PLL
    input  logic       i_rst,
    // Control, quasi-static in the pixel domain (synchronized by the
    // consumer)
    input  logic       i_enable,
    input  logic       i_cursor_en,
    input  logic [7:0] i_cursor_col,
    input  logic [5:0] i_cursor_row,
    output logic [3:0] o_d0,     // DDR high-half bits: [0]=B [1]=G [2]=R [3]=clock
    output logic [3:0] o_d1      // DDR low-half bits, same lane order
);

    // ── Pixel-domain front end ──────────────────────────────────
    logic [11:0] x, y;
    logic        de, hsync, vsync;

    video_timing u_timing (
        .i_clk   (i_pclk),
        .i_rst   (i_rst),
        .o_x     (x),
        .o_y     (y),
        .o_de    (de),
        .o_hsync (hsync),
        .o_vsync (vsync)
    );

    // Character generator: RGB + syncs emerge delay-matched.
    logic [7:0] r, g, b;
    logic       tg_de, tg_hsync, tg_vsync;

    video_textgen #(
        .CELLS_HEX    (CELLS_HEX)
    ) u_textgen (
        .i_clk        (i_clk),
        .i_cell_we    (i_cell_we),
        .i_cell_addr  (i_cell_addr),
        .i_cell_wdata (i_cell_wdata),
        .o_cell_rdata (o_cell_rdata),
        .i_pclk       (i_pclk),
        .i_rst        (i_rst),
        .i_x          (x),
        .i_y          (y),
        .i_de         (de),
        .i_hsync      (hsync),
        .i_vsync      (vsync),
        .i_enable     (i_enable),
        .i_cursor_en  (i_cursor_en),
        .i_cursor_col (i_cursor_col),
        .i_cursor_row (i_cursor_row),
        .o_r          (r),
        .o_g          (g),
        .o_b          (b),
        .o_de         (tg_de),
        .o_hsync      (tg_hsync),
        .o_vsync      (tg_vsync)
    );

    // ── TMDS lane encoders ──────────────────────────────────────
    // DVI lane roles: blue carries the sync levels in its control
    // bits; green and red tie theirs low.
    logic [9:0] tmds_b, tmds_g, tmds_r;

    video_tmds_encoder u_enc_b (
        .i_clk  (i_pclk),
        .i_rst  (i_rst),
        .i_data (b),
        .i_c0   (tg_hsync),
        .i_c1   (tg_vsync),
        .i_de   (tg_de),
        .o_tmds (tmds_b)
    );

    video_tmds_encoder u_enc_g (
        .i_clk  (i_pclk),
        .i_rst  (i_rst),
        .i_data (g),
        .i_c0   (1'b0),
        .i_c1   (1'b0),
        .i_de   (tg_de),
        .o_tmds (tmds_g)
    );

    video_tmds_encoder u_enc_r (
        .i_clk  (i_pclk),
        .i_rst  (i_rst),
        .i_data (r),
        .i_c0   (1'b0),
        .i_c1   (1'b0),
        .i_de   (tg_de),
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
