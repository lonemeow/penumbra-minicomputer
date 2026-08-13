// Penumbra display framebuffer RAM — the FB aperture's backing store.
//
// True dual-port, dual-clock BRAM holding the pixel array packed four
// 8-bit pixels per word. Port A lives in the CPU/bus clock domain and
// serves the FB aperture with per-byte write enables: the aperture is
// mapped straight into a rendering process, whose compiler-generated
// stores arrive at every width the CPU can issue, so a write commits
// exactly the enabled lanes and leaves the rest. Port B lives in the
// pixel clock domain and feeds scan-out a whole word at a time — one
// word covers four pixels, so the fetch rate stays well under the
// pixel rate.
//
// Storage is four byte-wide banks. That makes the byte enables plain
// per-bank write enables, and it is the shape synthesis reliably
// infers block RAM from.
//
// The ports are independent, with no coherency handshake: a pixel
// written mid-frame becomes visible on the next scan that reads its
// word, so a frame can tear. The device contract
// (doc/system/devices/display.md) permits that. Contents are undefined
// at power-up — the picture is software's.

module video_fb_ram #(
    // Depth in 32-bit words; the pixel array is four pixels per word.
    parameter int WORDS = 19200
) (
    // ── Port A: CPU/bus domain (FB aperture) ────────────────────
    input  logic                     i_clk,
    input  logic                     i_we,
    input  logic [3:0]               i_byte_en,
    input  logic [$clog2(WORDS)-1:0] i_addr,
    input  logic [31:0]              i_wdata,
    output logic [31:0]              o_rdata,

    // ── Port B: pixel domain (scan-out read) ────────────────────
    input  logic                     i_pclk,
    input  logic [$clog2(WORDS)-1:0] i_scan_addr,
    output logic [31:0]              o_scan_data
);

    (* ram_style = "block" *) logic [7:0] mem0 [0:WORDS-1];
    (* ram_style = "block" *) logic [7:0] mem1 [0:WORDS-1];
    (* ram_style = "block" *) logic [7:0] mem2 [0:WORDS-1];
    (* ram_style = "block" *) logic [7:0] mem3 [0:WORDS-1];

    logic [31:0] rdata_q;
    logic [31:0] scan_data_q;

    always_ff @(posedge i_clk) begin
        if (i_we) begin
            if (i_byte_en[0]) mem0[i_addr] <= i_wdata[ 7: 0];
            if (i_byte_en[1]) mem1[i_addr] <= i_wdata[15: 8];
            if (i_byte_en[2]) mem2[i_addr] <= i_wdata[23:16];
            if (i_byte_en[3]) mem3[i_addr] <= i_wdata[31:24];
        end
        rdata_q <= {mem3[i_addr], mem2[i_addr], mem1[i_addr], mem0[i_addr]};
    end

    always_ff @(posedge i_pclk)
        scan_data_q <= {mem3[i_scan_addr], mem2[i_scan_addr],
                        mem1[i_scan_addr], mem0[i_scan_addr]};

    assign o_rdata     = rdata_q;
    assign o_scan_data = scan_data_q;

endmodule
