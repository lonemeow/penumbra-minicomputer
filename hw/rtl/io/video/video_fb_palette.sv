// Penumbra display framebuffer palette — the FB_PALETTE aperture's
// backing store.
//
// Dual-clock lookup mapping each 8-bit pixel value to a color. Port A
// lives in the CPU/bus clock domain behind the word-strided aperture,
// port B in the pixel domain at the tail of the scan-out pipeline,
// which reads it every pixel.
//
// Entries are the aperture's 0x00RRGGBB with the unused byte dropped:
// the store is 24 bits wide and readback zero-extends, so a slot reads
// back the color it was given.
//
// Writes take effect on the next scan-out read whether the framebuffer
// is deselected or live — animating the palette under a still image is
// a legitimate technique, so there is nothing to hold writes back for.
// Contents are undefined at power-up; the consumer loads all entries
// before selecting the framebuffer.

module video_fb_palette #(
    parameter int ENTRIES = 256
) (
    // ── Port A: CPU/bus domain (FB_PALETTE aperture) ────────────
    input  logic                       i_clk,
    input  logic                       i_we,
    input  logic [$clog2(ENTRIES)-1:0] i_addr,
    input  logic [23:0]                i_wdata,
    output logic [23:0]                o_rdata,

    // ── Port B: pixel domain (scan-out lookup) ──────────────────
    input  logic                       i_pclk,
    input  logic [$clog2(ENTRIES)-1:0] i_index,
    output logic [23:0]                o_color
);

    (* ram_style = "block" *) logic [23:0] mem [0:ENTRIES-1];

    logic [23:0] rdata_q;
    logic [23:0] color_q;

    always_ff @(posedge i_clk) begin
        if (i_we)
            mem[i_addr] <= i_wdata;
        rdata_q <= mem[i_addr];
    end

    always_ff @(posedge i_pclk)
        color_q <= mem[i_index];

    assign o_rdata = rdata_q;
    assign o_color = color_q;

endmodule
