// Penumbra display character/attribute RAM — the CELLS backing store.
//
// True dual-port, dual-clock BRAM: port A lives in the CPU/bus clock
// domain and serves the device's CELLS aperture (write + registered
// readback), port B lives in the pixel clock domain and feeds the
// scan-out pipeline (registered read, one pixel-clock cycle — the
// fetch pipeline allots it a stage, like the font ROM). Each entry is
// one 16-bit {attr, glyph} cell; the depth covers the largest mode's
// COLUMNS x ROWS grid, rounded to the BRAM's natural power of two.
//
// The ports are independent: a CPU write becomes visible on the next
// scan that reads the cell, with no coherency handshake — a cell
// updated mid-frame can tear for one frame, which the device contract
// (doc/system/devices/display.md) deliberately permits. Contents are
// undefined at power-up; the consumer initializes the grid.

module video_cell_ram #(
    parameter int DEPTH    = 4096,
    // Optional $readmemh preload — the CPU-free bring-up path (a
    // splash screen before any bus master exists). Empty: contents
    // are undefined at power-up, the device contract's default.
    parameter     INIT_HEX = ""
) (
    // ── Port A: CPU/bus domain (CELLS aperture) ─────────────────
    input  logic                     i_clk,
    input  logic                     i_we,
    input  logic [$clog2(DEPTH)-1:0] i_addr,
    input  logic [15:0]              i_wdata,
    output logic [15:0]              o_rdata,

    // ── Port B: pixel domain (scan-out read) ────────────────────
    input  logic                     i_pclk,
    input  logic [$clog2(DEPTH)-1:0] i_scan_addr,
    output logic [15:0]              o_scan_data
);

    logic [15:0] mem [0:DEPTH-1];

    if (INIT_HEX != "") begin : g_preload
        initial $readmemh(INIT_HEX, mem);
    end

    logic [15:0] rdata_q;
    logic [15:0] scan_data_q;

    always_ff @(posedge i_clk) begin
        if (i_we)
            mem[i_addr] <= i_wdata;
        rdata_q <= mem[i_addr];
    end

    always_ff @(posedge i_pclk)
        scan_data_q <= mem[i_scan_addr];

    assign o_rdata     = rdata_q;
    assign o_scan_data = scan_data_q;

endmodule
