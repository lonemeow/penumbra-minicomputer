// Penumbra text-video built-in font ROM — 256 glyphs x 16 rows.
//
// One 8-pixel glyph row per read: addressed by {glyph, row}, returning
// the row byte with bit 7 as the leftmost column on screen and 1 as
// foreground. The read is registered (one pixel-clock cycle), which
// maps the array onto a block RAM — the scan-out pipeline allots the
// font read its own stage, so the latency is part of the module
// contract, not an implementation detail.
//
// Contents load at synthesis/simulation time via $readmemh from
// font8x16.hex (root-level, generated — never committed), produced by
// hw/tools/wsfont2hex.py from the NetBSD console font bold8x16
// (netbsd/sys/dev/wsfont/bold8x16.h): public-domain glyph data in the
// IBM/CP437 encoding the device contract pins
// (doc/system/devices/display.md). Glyphs the source font omits
// (0 and 255 — blank in CP437) are zero-filled by the tool.

module video_font_rom (
    input  logic       i_clk,    // pixel clock
    input  logic [7:0] i_glyph,  // character code (CP437)
    input  logic [3:0] i_row,    // glyph row index, 0 = top scanline
    output logic [7:0] o_pixels  // glyph row; bit 7 = leftmost, 1 = FG
);

    logic [7:0] rom [0:4095];

    initial $readmemh("font8x16.hex", rom);

    logic [7:0] pixels_q;

    always_ff @(posedge i_clk)
        pixels_q <= rom[{i_glyph, i_row}];

    assign o_pixels = pixels_q;

endmodule
