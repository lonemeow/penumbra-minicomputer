// Penumbra display framebuffer generator — the pixel source behind
// CTRL.FB_SEL.
//
// The bitmap sibling of video_textgen: same raster inputs, same
// delay-matched RGB output, with the cell grid and font replaced by a
// packed pixel array and a 256-entry palette. Scan-out doubles each
// framebuffer pixel onto the mode-0 raster in both axes, so a
// FB_WIDTH x FB_HEIGHT picture fills the 640x480 active region and
// selecting this source never re-times the output.
//
// Two coordinate spaces meet here, and every name says which one it
// belongs to. **Raster** coordinates are the output resolution the
// timing generator counts in — i_raster_x / i_raster_y, spanning the
// whole frame including blanking. **Framebuffer** coordinates are the
// FB_WIDTH x FB_HEIGHT picture, and everything in that space carries an
// fb_ prefix. Doubling is the conversion between them: a framebuffer
// pixel is raster (x >> 1, y >> 1).
//
// Addressing carries no adder, and so no stride constant and no
// multiplier. The scan address is a counter that steps once every
// eight raster columns of active video — four pixels per word and two
// raster columns per pixel — which walks exactly one framebuffer line
// per raster line. Lines are contiguous, so a walk ends standing on
// the next line's first word: advancing costs nothing, and repeating a
// line reloads the start this walk began from. The counter wraps at
// the end of the array, which is also where the last visible line
// leaves it, so a frame always begins at zero and the address can
// never leave the array.
//
// Pipeline, one register stage each, with de/hsync/vsync delayed to
// match:
//   S1  pixel word read     addr = the scan counter
//   S2  palette lookup      index = the word's pixel at (raster_x >> 1) & 3
//   S3  blanking gate       ENABLE and de force black
//
// Contents are undefined at power-up and the module never writes them;
// the picture belongs to software through the FB aperture.

module video_fbgen #(
    parameter int FB_WIDTH  = 320,
    parameter int FB_HEIGHT = 240,
    // Active raster geometry, defaulted to the console's mode 0. The
    // blanking structure never reaches the addressing — where the
    // active region ends is all it needs.
    parameter int H_ACTIVE  = video_pkg::H_ACTIVE,
    parameter int V_ACTIVE  = video_pkg::V_ACTIVE,
    parameter bit H_SYNC_POL = video_pkg::H_SYNC_POL,
    parameter bit V_SYNC_POL = video_pkg::V_SYNC_POL
) (
    // ── CPU/bus domain: FB and FB_PALETTE aperture ports ────────
    input  logic        i_clk,
    input  logic        i_fb_we,
    input  logic [3:0]  i_fb_byte_en,
    input  logic [14:0] i_fb_addr,
    input  logic [31:0] i_fb_wdata,
    output logic [31:0] o_fb_rdata,
    input  logic        i_pal_we,
    input  logic [7:0]  i_pal_addr,
    input  logic [23:0] i_pal_wdata,
    output logic [23:0] o_pal_rdata,

    // ── Pixel domain ────────────────────────────────────────────
    input  logic        i_pclk,
    input  logic        i_rst,
    // Raster position, whole-frame counters including blanking. Both
    // pixel sources take the same raster inputs so the chain wires
    // them alike; this one reads only the row's parity, since the
    // address counter finds every other boundary by itself.
    input  logic [11:0] i_raster_x,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [11:0] i_raster_y,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic        i_de,
    input  logic        i_hsync,
    input  logic        i_vsync,
    input  logic        i_enable,
    output logic [7:0]  o_r,
    output logic [7:0]  o_g,
    output logic [7:0]  o_b,
    output logic        o_de,
    output logic        o_hsync,
    output logic        o_vsync
);

    // Pixel array geometry: four 8-bit pixels per word, so a
    // framebuffer line is FB_WIDTH/4 words and the array is that many
    // per line for FB_HEIGHT lines.
    localparam int FB_LINE_WORDS = FB_WIDTH / 4;
    localparam int FB_WORDS      = FB_LINE_WORDS * FB_HEIGHT;
    localparam int FB_ADDR_W     = $clog2(FB_WORDS);

    localparam logic [FB_ADDR_W-1:0] FB_LAST_WORD = FB_ADDR_W'(FB_WORDS - 1);
    localparam logic [11:0]          H_ACT        = 12'(H_ACTIVE);

    initial begin
        assert (FB_WIDTH % 4 == 0)
            else $fatal(1, "video_fbgen: FB_WIDTH %0d is not a whole number of words", FB_WIDTH);
        assert (FB_WIDTH * 2 == H_ACTIVE && FB_HEIGHT * 2 == V_ACTIVE)
            else $fatal(1, "video_fbgen: %0dx%0d does not double onto the %0dx%0d raster",
                        FB_WIDTH, FB_HEIGHT, H_ACTIVE, V_ACTIVE);
    end

    // ── Scan address: a counter and the line start to fall back to ──
    // The counter is what the RAM's read port sees, so nothing but a
    // register sits on that path. fb_line_start_q remembers where the
    // walk of the line now on screen began, which is what the second
    // showing of that line rewinds to.
    logic [FB_ADDR_W-1:0] fb_word_addr_q;
    logic [FB_ADDR_W-1:0] fb_line_start_q;

    // A word lasts eight raster columns, and only active video
    // advances — held through blanking, the counter stays lined up
    // with the column it is fetching for.
    logic word_step;
    logic line_done;
    logic next_line_repeats;

    assign word_step         = i_de && (i_raster_x[2:0] == 3'd7);
    // Horizontal blanking is where the rewind belongs: the walk has
    // finished, and the counter must already hold the next line's
    // first address by the time that line's first column arrives.
    assign line_done         = (i_raster_x == H_ACT);
    // Even raster lines are a framebuffer line's first showing, so the
    // line following one of those repeats it.
    assign next_line_repeats = !i_raster_y[0];

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            fb_word_addr_q  <= '0;
            fb_line_start_q <= '0;
        end else if (line_done) begin
            // The walk has left the counter on the next framebuffer
            // line's first word. Repeating rewinds to where this walk
            // began; advancing keeps the counter where it stands and
            // remembers that as the new line start.
            if (next_line_repeats)
                fb_word_addr_q  <= fb_line_start_q;
            else
                fb_line_start_q <= fb_word_addr_q;
        end else if (word_step) begin
            // Wrapping at the end of the array is the frame rewind:
            // the last visible line finishes exactly here, and the
            // address is structurally unable to leave the array —
            // vertical blanking then holds it at zero.
            fb_word_addr_q <= (fb_word_addr_q == FB_LAST_WORD)
                                ? '0 : fb_word_addr_q + 1'b1;
        end
    end

    // ── S1: pixel word fetch ────────────────────────────────────
    // The fetched word lands next cycle, so the pixel selector rides
    // one stage behind the raster column it belongs to.
    logic [31:0] fb_word;
    logic [1:0]  s1_pixel_sel_q;
    logic        s1_de_q, s1_hsync_q, s1_vsync_q;

    video_fb_ram #(
        .WORDS       (FB_WORDS)
    ) u_fb_ram (
        .i_clk       (i_clk),
        .i_we        (i_fb_we),
        .i_byte_en   (i_fb_byte_en),
        .i_addr      (i_fb_addr[FB_ADDR_W-1:0]),
        .i_wdata     (i_fb_wdata),
        .o_rdata     (o_fb_rdata),
        .i_pclk      (i_pclk),
        .i_scan_addr (fb_word_addr_q),
        .o_scan_data (fb_word)
    );

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            s1_pixel_sel_q <= '0;
            s1_de_q        <= 1'b0;
            s1_hsync_q     <= ~H_SYNC_POL;
            s1_vsync_q     <= ~V_SYNC_POL;
        end else begin
            s1_pixel_sel_q <= i_raster_x[2:1];
            s1_de_q        <= i_de;
            s1_hsync_q     <= i_hsync;
            s1_vsync_q     <= i_vsync;
        end
    end

    // ── S2: palette lookup ──────────────────────────────────────
    // The pixel's byte within the word runs low byte first — the
    // aperture's little-endian packing seen from the pixel side.
    logic [7:0]  pal_index;
    logic [23:0] pal_color;
    logic        s2_de_q, s2_hsync_q, s2_vsync_q;

    always_comb begin
        case (s1_pixel_sel_q)
            2'd0:    pal_index = fb_word[ 7: 0];
            2'd1:    pal_index = fb_word[15: 8];
            2'd2:    pal_index = fb_word[23:16];
            default: pal_index = fb_word[31:24];
        endcase
    end

    video_fb_palette u_fb_palette (
        .i_clk   (i_clk),
        .i_we    (i_pal_we),
        .i_addr  (i_pal_addr),
        .i_wdata (i_pal_wdata),
        .o_rdata (o_pal_rdata),
        .i_pclk  (i_pclk),
        .i_index (pal_index),
        .o_color (pal_color)
    );

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            s2_de_q    <= 1'b0;
            s2_hsync_q <= ~H_SYNC_POL;
            s2_vsync_q <= ~V_SYNC_POL;
        end else begin
            s2_de_q    <= s1_de_q;
            s2_hsync_q <= s1_hsync_q;
            s2_vsync_q <= s1_vsync_q;
        end
    end

    // ── S3: blanking gate ───────────────────────────────────────
    logic [23:0] s3_rgb_q;
    logic        s3_de_q, s3_hsync_q, s3_vsync_q;

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            s3_rgb_q   <= '0;
            s3_de_q    <= 1'b0;
            s3_hsync_q <= ~H_SYNC_POL;
            s3_vsync_q <= ~V_SYNC_POL;
        end else begin
            // ENABLE blanks the picture only — de/syncs keep running.
            s3_rgb_q   <= (s2_de_q && i_enable) ? pal_color : 24'h000000;
            s3_de_q    <= s2_de_q;
            s3_hsync_q <= s2_hsync_q;
            s3_vsync_q <= s2_vsync_q;
        end
    end

    assign {o_r, o_g, o_b} = s3_rgb_q;
    assign o_de            = s3_de_q;
    assign o_hsync         = s3_hsync_q;
    assign o_vsync         = s3_vsync_q;

endmodule
