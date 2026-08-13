// Penumbra display character generator — cells + font -> parallel RGB.
//
// The text scan-out pipeline of the display device
// (doc/internals/display.md): from the timing generator's raster
// position it fetches the character cell, then the glyph row, selects
// the pixel, applies the cursor, and maps the color index through the
// built-in CGA/ANSI palette. Four pipeline stages, so the RGB output
// lags the raster input by LATENCY = 4 pixel clocks; i_de / i_hsync /
// i_vsync ride matching delay registers and emerge aligned with their
// pixel. Blanking is forced black (TMDS carries no pixel data there).
//
//   S1  cell RAM read      addr = char_row * COLUMNS + char_col
//   S2  font ROM read      {glyph, y[3:0]}, attr rides alongside
//   S3  pixel select       font bit [7 - x[2:0]] picks FG/BG nibble;
//                          the cursor inverts the cell (FG/BG swap)
//   S4  palette LUT        4-bit index -> 24-bit RGB, black in blanking
//
// The cursor is drawn at (i_cursor_col, i_cursor_row) while
// i_cursor_en is set, as inverse video over the whole cell, blinking
// on a frame counter: visible for 32 frames, hidden for 32, starting
// visible at reset. The counter advances on each vsync assertion
// (VSYNC_POL names the asserted level).
//
// The CPU-domain port passes through to the cell RAM's bus-side port —
// the device wrapper's CELLS aperture. Cell writes land mid-scan
// without a handshake; the contract permits the one-frame tear.

module video_textgen #(
    parameter int COLUMNS    = 80,
    parameter int ROWS       = 30,
    parameter bit HSYNC_POL  = video_pkg::H_SYNC_POL,
    parameter bit VSYNC_POL  = video_pkg::V_SYNC_POL,
    // Passed through to the cell RAM's optional $readmemh preload.
    parameter     CELLS_HEX  = ""
) (
    // ── CPU/bus domain: CELLS aperture ──────────────────────────
    input  logic        i_clk,
    input  logic        i_cell_we,
    input  logic [11:0] i_cell_addr,
    input  logic [15:0] i_cell_wdata,
    output logic [15:0] o_cell_rdata,

    // ── Pixel domain ────────────────────────────────────────────
    input  logic        i_pclk,
    input  logic        i_rst,
    // Raster position from video_timing; leads the output by 4 clocks
    input  logic [11:0] i_x,
    input  logic [11:0] i_y,
    input  logic        i_de,
    input  logic        i_hsync,
    input  logic        i_vsync,
    // Control, quasi-static in the pixel domain (synchronized by the
    // consumer)
    input  logic        i_enable,      // clear: black active video, timing runs
    input  logic        i_cursor_en,
    input  logic [7:0]  i_cursor_col,
    input  logic [5:0]  i_cursor_row,
    // Delay-matched parallel RGB
    output logic [7:0]  o_r,
    output logic [7:0]  o_g,
    output logic [7:0]  o_b,
    output logic        o_de,
    output logic        o_hsync,
    output logic        o_vsync
);

    initial begin
        assert (COLUMNS * ROWS <= 4096)
            else $fatal(1, "video_textgen: %0dx%0d grid exceeds the cell RAM", COLUMNS, ROWS);
    end

    // ── Cursor blink: frame counter on vsync assertion ──────────
    // blink_show covers frames 0..31 after reset, then toggles every
    // 32 frames — the counter's top bit is the hide phase.
    logic       vsync_prev_q;
    logic [5:0] frame_cnt_q;
    logic       blink_show;

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            vsync_prev_q <= ~VSYNC_POL;
            frame_cnt_q  <= '0;
        end else begin
            vsync_prev_q <= i_vsync;
            if (vsync_prev_q != VSYNC_POL && i_vsync == VSYNC_POL)
                frame_cnt_q <= frame_cnt_q + 6'd1;
        end
    end

    assign blink_show = !frame_cnt_q[5];

    // ── S0: cell coordinates (combinational on the raster input) ──
    // Cell geometry is a power of two, so /8 and /16 are bit slices.
    logic [8:0]  char_col;
    logic [7:0]  char_row;
    logic [2:0]  glyph_col;
    logic [3:0]  glyph_row;
    logic [11:0] cell_addr;
    logic        cursor_hit;

    assign char_col  = i_x[11:3];
    assign char_row  = i_y[11:4];
    assign glyph_col = i_x[2:0];
    assign glyph_row = i_y[3:0];
    // The row's base offset is char_row * COLUMNS. COLUMNS is a
    // compile-time constant, so the product is spelled as the sum of
    // shifts its set bits imply: synthesis maps a `*` by a
    // non-power-of-two constant onto the DSP blocks, spending a
    // MULT18X18D on what is a pair of adders.
    function automatic logic [11:0] row_offset(logic [7:0] row);
        row_offset = '0;
        for (int b = 0; b < 12; b++)
            if (COLUMNS[b])
                row_offset += 12'(row) << b;
    endfunction

    assign cell_addr = row_offset(char_row) + 12'(char_col);
    assign cursor_hit = i_cursor_en && blink_show &&
                        (char_col == 9'(i_cursor_col)) &&
                        (char_row == 8'(i_cursor_row));

    // ── S1: cell RAM read ───────────────────────────────────────
    logic [15:0] cell_data;   // {attr, glyph}, valid one cycle after S0

    video_cell_ram #(
        .DEPTH       (4096),
        .INIT_HEX    (CELLS_HEX)
    ) u_cells (
        .i_clk       (i_clk),
        .i_we        (i_cell_we),
        .i_addr      (i_cell_addr),
        .i_wdata     (i_cell_wdata),
        .o_rdata     (o_cell_rdata),
        .i_pclk      (i_pclk),
        .i_scan_addr (cell_addr),
        .o_scan_data (cell_data)
    );

    logic [3:0] s1_glyph_row_q;
    logic [2:0] s1_glyph_col_q;
    logic       s1_cursor_q, s1_de_q, s1_hsync_q, s1_vsync_q;

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            s1_glyph_row_q <= '0;
            s1_glyph_col_q <= '0;
            s1_cursor_q    <= 1'b0;
            s1_de_q        <= 1'b0;
            s1_hsync_q     <= ~HSYNC_POL;
            s1_vsync_q     <= ~VSYNC_POL;
        end else begin
            s1_glyph_row_q <= glyph_row;
            s1_glyph_col_q <= glyph_col;
            s1_cursor_q    <= cursor_hit;
            s1_de_q        <= i_de;
            s1_hsync_q     <= i_hsync;
            s1_vsync_q     <= i_vsync;
        end
    end

    // ── S2: font ROM read; attribute rides alongside ────────────
    logic [7:0] font_row;   // valid one cycle after S2 launch

    video_font_rom u_font (
        .i_clk    (i_pclk),
        .i_glyph  (cell_data[7:0]),
        .i_row    (s1_glyph_row_q),
        .o_pixels (font_row)
    );

    logic [7:0] s2_attr_q;
    logic [2:0] s2_glyph_col_q;
    logic       s2_cursor_q, s2_de_q, s2_hsync_q, s2_vsync_q;

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            s2_attr_q      <= '0;
            s2_glyph_col_q <= '0;
            s2_cursor_q    <= 1'b0;
            s2_de_q        <= 1'b0;
            s2_hsync_q     <= ~HSYNC_POL;
            s2_vsync_q     <= ~VSYNC_POL;
        end else begin
            s2_attr_q      <= cell_data[15:8];
            s2_glyph_col_q <= s1_glyph_col_q;
            s2_cursor_q    <= s1_cursor_q;
            s2_de_q        <= s1_de_q;
            s2_hsync_q     <= s1_hsync_q;
            s2_vsync_q     <= s1_vsync_q;
        end
    end

    // ── S3: pixel select — font bit + attribute + cursor ────────
    // Inputs, all aligned to this stage: font_row (the glyph row,
    // bit 7 = leftmost pixel, 1 = foreground), s2_glyph_col_q (pixel
    // column within the cell, 0 = leftmost), s2_attr_q (FG index in
    // [3:0], BG index in [7:4]), s2_cursor_q (blink-qualified cursor
    // hit for this cell). Result: s3_idx_q, the 4-bit palette index.
    logic [3:0] s3_idx_q;
    logic       s3_de_q, s3_hsync_q, s3_vsync_q;

    // The cursor is inverse video: swapping which nibble a set font
    // bit selects is the same as flipping the bit itself, so the
    // inversion is one XOR on the hoisted select instead of a
    // duplicated FG/BG swap in both branches.
    logic fg_pixel;
    assign fg_pixel = font_row[3'd7 - s2_glyph_col_q] ^ s2_cursor_q;

    always_ff @(posedge i_pclk)
        s3_idx_q <= fg_pixel ? s2_attr_q[3:0] : s2_attr_q[7:4];

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            s3_de_q    <= 1'b0;
            s3_hsync_q <= ~HSYNC_POL;
            s3_vsync_q <= ~VSYNC_POL;
        end else begin
            s3_de_q    <= s2_de_q;
            s3_hsync_q <= s2_hsync_q;
            s3_vsync_q <= s2_vsync_q;
        end
    end

    // ── S4: built-in CGA/ANSI palette, black in blanking ────────
    localparam logic [23:0] CGA_PALETTE [16] = '{
        24'h000000, 24'h0000AA, 24'h00AA00, 24'h00AAAA,
        24'hAA0000, 24'hAA00AA, 24'hAA5500, 24'hAAAAAA,
        24'h555555, 24'h5555FF, 24'h55FF55, 24'h55FFFF,
        24'hFF5555, 24'hFF55FF, 24'hFFFF55, 24'hFFFFFF
    };

    // Selecting the entry by comparison rather than indexing
    // CGA_PALETTE directly: an unpacked-array index survives
    // translation to Verilog as a packed part-select whose offset is
    // the index times the 24-bit entry width, and that multiply lands
    // on a DSP block. The comparison chain is a plain one-hot mux and
    // keeps the palette literal above as the single definition.
    function automatic logic [23:0] palette_entry(logic [3:0] idx);
        palette_entry = 24'h000000;
        for (int e = 0; e < 16; e++)
            if (idx == 4'(e))
                palette_entry = CGA_PALETTE[e];
    endfunction

    logic [23:0] s4_rgb_q;
    logic        s4_de_q, s4_hsync_q, s4_vsync_q;

    always_ff @(posedge i_pclk) begin
        if (i_rst) begin
            s4_rgb_q   <= '0;
            s4_de_q    <= 1'b0;
            s4_hsync_q <= ~HSYNC_POL;
            s4_vsync_q <= ~VSYNC_POL;
        end else begin
            // ENABLE blanks the picture only — de/syncs keep running.
            s4_rgb_q   <= (s3_de_q && i_enable) ? palette_entry(s3_idx_q) : 24'h000000;
            s4_de_q    <= s3_de_q;
            s4_hsync_q <= s3_hsync_q;
            s4_vsync_q <= s3_vsync_q;
        end
    end

    assign {o_r, o_g, o_b} = s4_rgb_q;
    assign o_de    = s4_de_q;
    assign o_hsync = s4_hsync_q;
    assign o_vsync = s4_vsync_q;

endmodule
