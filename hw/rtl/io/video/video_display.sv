// Penumbra display adapter — the CLASS_DISPLAY bus device.
//
// The inner device behind autoconfig_dev: implements the class
// minimum protocol (doc/system/devices/display.md) for a single-mode
// 80x30 color console plus the framebuffer capability — the CAP /
// INFO / CTRL / CURSOR / FB_GEOM / FB_FORMAT register block and the
// CELLS, FB_PALETTE and FB apertures — in the bus clock domain, and
// drives the output chain (timing, both pixel generators, the source
// select, TMDS encoders, DDR serializers) in the video clock domains.
// CFG_SIZE is 256 KiB: registers in the first page, cells at
// 0x1000 + (row*80 + col)*4, the framebuffer palette at 0x10000, and
// the pixel aperture at 0x20000.
//
// Clock domains: registers and the cell-aperture port live on i_clk;
// the chain runs on i_pclk / i_sclk with its own reset (gated by the
// video PLL's lock, not the bus reset). Control values cross as
// quasi-static state through 2-FF synchronizers; the cell array
// crosses inside the chain's dual-clock BRAM.
//
// Reads present the 1-cycle registered response every bus device
// does (the access_pending busy idiom); writes complete unstalled.
// Unimplemented register slots (modes) read 0 and ignore writes.

// keep_hierarchy: hold the display subsystem together through
// synth_ecp5 so its cells stay a cohesive island. A standalone
// peripheral whose only CPU-clock logic is the small register tier
// must not scatter into — or let the optimizer blend it with — the
// main clock domain's critical regions.
(* keep_hierarchy = "yes" *)
module video_display #(
    // Cell-RAM preload, passed to the chain. Empty is the contract's
    // behavior — power-up contents undefined, screen content owned by
    // software. A CPU-free bring-up probe names an image instead, so
    // it has something to show without a bus master.
    parameter CELLS_HEX = ""
) (
    // ── Bus (behind autoconfig_dev), CPU clock domain ───────────
    input  logic        i_clk,
    input  logic        i_rst,
    // Window-offset bits [17:0] carry the decode; the base-aligned
    // upper bits are the wrapper's concern.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_addr,
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] i_wdata,
    // Byte lanes, honored by the pixel aperture alone — every other
    // register and aperture here is word-strided.
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Video clock domains ─────────────────────────────────────
    input  logic        i_pclk,   // pixel clock
    input  logic        i_sclk,   // serial clock — 5x pixel, same PLL
    input  logic        i_vrst,   // video-domain reset (PLL-lock gated)

    // ── GPDI DDR bit pairs to the board's ODDRX1F cells ─────────
    output logic [3:0]  o_d0,
    output logic [3:0]  o_d1
);

    // ── Identity: the minimum protocol this device implements ───
    // CAP: version 1, COLOR (full 16-color render), EXTGLYPHS (the
    // CP437 built-in font), FRAMEBUFFER, MODE_COUNT 1. No mode
    // switch, custom text palette, or soft font — those slots read 0
    // below.
    localparam int FB_WIDTH  = 320;
    localparam int FB_HEIGHT = 240;

    localparam logic [31:0] CAP_VALUE =
        (32'd1 << 16)     // MODE_COUNT
      | (32'b1 << 13)     // FRAMEBUFFER
      | (32'b1 << 10)     // EXTGLYPHS
      | (32'b1 << 8)      // COLOR
      | 32'd1;            // version
    localparam logic [31:0] INFO_VALUE = (32'd30 << 16) | 32'd80;
    localparam logic [31:0] FB_GEOM_VALUE =
        (32'(FB_HEIGHT) << 16) | 32'(FB_WIDTH);
    // BPP 8, indexed through FB_PALETTE — the only format this
    // revision of the class defines.
    localparam logic [31:0] FB_FORMAT_VALUE = 32'd8;

    // ── Address decode ──────────────────────────────────────────
    // The window splits on its top two offset bits: the register page
    // and the cell aperture below 0x10000, the framebuffer palette at
    // 0x10000, and the pixel aperture from 0x20000 up. Each region's
    // storage registers its own read, so all of them answer with the
    // same one-cycle latency.
    logic        cells_sel;
    logic        fbpal_sel;
    logic        fb_sel;
    logic [11:0] cell_idx;

    assign fb_sel    = i_addr[17];
    assign fbpal_sel = (i_addr[17:16] == 2'b01);
    assign cells_sel = (i_addr[17:16] == 2'b00) && (i_addr[15:12] != 4'd0);
    assign cell_idx  = 12'(i_addr[13:2]) - 12'd1024;

    // ── Register file (bus domain) ──────────────────────────────
    // CTRL holds only its implemented bits; CURSOR stores the full
    // written word so readback is faithful, while the render path
    // consumes the low bits of each 16-bit field.
    logic        ctrl_enable_q;
    logic        ctrl_cursor_en_q;
    logic        ctrl_fb_sel_q;
    logic [31:0] cursor_q;

    logic regs_sel;
    assign regs_sel = !cells_sel && !fbpal_sel && !fb_sel;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ctrl_enable_q    <= 1'b0;   // black until software draws
            ctrl_cursor_en_q <= 1'b0;
            ctrl_fb_sel_q    <= 1'b0;   // the character console powers up
            cursor_q         <= '0;
        end else if (i_we && regs_sel) begin
            case (i_addr[5:2])
                4'd2: {ctrl_fb_sel_q, ctrl_cursor_en_q, ctrl_enable_q} <=
                          {i_wdata[4], i_wdata[1:0]};
                4'd3: cursor_q <= i_wdata;
                default: ;   // read-only and absent slots ignore writes
            endcase
        end
    end

    logic [31:0] reg_rdata;
    always_comb begin
        case (i_addr[5:2])
            4'd0:    reg_rdata = CAP_VALUE;
            4'd1:    reg_rdata = INFO_VALUE;
            4'd2:    reg_rdata = {27'b0, ctrl_fb_sel_q, 2'b0,
                                  ctrl_cursor_en_q, ctrl_enable_q};
            4'd3:    reg_rdata = cursor_q;
            4'd7:    reg_rdata = FB_GEOM_VALUE;
            4'd8:    reg_rdata = FB_FORMAT_VALUE;
            default: reg_rdata = 32'b0;   // modes absent
        endcase
    end

    // ── Registered read + busy (the access_pending idiom) ───────
    // The cell RAM registers its own read; the register file mirrors
    // it here, so every read is 1-cycle and busy drops as the data
    // becomes valid.
    logic        pending_q;
    logic        cells_sel_q, fbpal_sel_q, fb_sel_q;
    logic [31:0] reg_rdata_q;
    logic [15:0] cell_rdata;
    logic [31:0] fb_rdata;
    logic [23:0] fbpal_rdata;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            pending_q <= 1'b0;
        else
            pending_q <= i_re && !pending_q;
        cells_sel_q <= cells_sel;
        fbpal_sel_q <= fbpal_sel;
        fb_sel_q    <= fb_sel;
        reg_rdata_q <= reg_rdata;
    end

    assign o_busy = i_re && !pending_q;

    always_comb begin
        if (fb_sel_q)         o_rdata = fb_rdata;
        else if (fbpal_sel_q) o_rdata = {8'b0, fbpal_rdata};
        else if (cells_sel_q) o_rdata = {16'b0, cell_rdata};
        else                  o_rdata = reg_rdata_q;
    end

    // ── Control crossing into the pixel domain ──────────────────
    // Quasi-static: 2-FF per bit, no handshake. The cursor position
    // crosses as independent bits; a mid-change sample misplaces the
    // cursor for at most a frame.
    logic       en_s1_q, en_s2_q;
    logic       cen_s1_q, cen_s2_q;
    logic       fbs_s1_q, fbs_s2_q;
    logic [7:0] col_s1_q, col_s2_q;
    logic [5:0] row_s1_q, row_s2_q;

    always_ff @(posedge i_pclk) begin
        en_s1_q  <= ctrl_enable_q;
        en_s2_q  <= en_s1_q;
        cen_s1_q <= ctrl_cursor_en_q;
        cen_s2_q <= cen_s1_q;
        fbs_s1_q <= ctrl_fb_sel_q;
        fbs_s2_q <= fbs_s1_q;
        col_s1_q <= cursor_q[7:0];
        col_s2_q <= col_s1_q;
        row_s1_q <= cursor_q[21:16];
        row_s2_q <= row_s1_q;
    end

    // ── The output chain ────────────────────────────────────────
    // The pixel aperture is the one place byte enables mean anything:
    // it behaves as plain memory so a rendering process can be handed
    // the pages directly. The palette aperture is word-strided like
    // the registers, keeping the color in the low three bytes.
    video_pixel_chain #(
        .CELLS_HEX    (CELLS_HEX)
    ) u_chain (
        .i_clk        (i_clk),
        .i_cell_we    (i_we && cells_sel),
        .i_cell_addr  (cell_idx),
        .i_cell_wdata (i_wdata[15:0]),
        .o_cell_rdata (cell_rdata),
        .i_fb_we      (i_we && fb_sel),
        .i_fb_byte_en (i_byte_en),
        .i_fb_addr    (i_addr[16:2]),
        .i_fb_wdata   (i_wdata),
        .o_fb_rdata   (fb_rdata),
        .i_pal_we     (i_we && fbpal_sel),
        .i_pal_addr   (i_addr[9:2]),
        .i_pal_wdata  (i_wdata[23:0]),
        .o_pal_rdata  (fbpal_rdata),
        .i_pclk       (i_pclk),
        .i_sclk       (i_sclk),
        .i_rst        (i_vrst),
        .i_enable     (en_s2_q),
        .i_fb_sel     (fbs_s2_q),
        .i_cursor_en  (cen_s2_q),
        .i_cursor_col (col_s2_q),
        .i_cursor_row (row_s2_q),
        .o_d0         (o_d0),
        .o_d1         (o_d1)
    );

endmodule
