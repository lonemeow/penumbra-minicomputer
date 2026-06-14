// Penumbra video timing generator — VESA-style display timing.
//
// Free-running horizontal and vertical counters on the pixel clock,
// producing the parallel sync/blank/coordinate stream that drives the
// rest of the display pipeline (cell + font fetch, palette, then the
// TMDS PHY). Defaults are 640x480 @ 60 Hz; the timing numbers are
// parameters, so a different mode is just a re-parameterization.
//
// {o_x, o_y, o_de, o_hsync, o_vsync} is the parallel-RGB seam: this
// block is a pure function of the pixel clock with no PHY dependency, so
// it is fully verifiable in simulation against the VESA numbers before
// any clocking or TMDS hardware exists.
//
// Per-line layout (in pixel-clock cycles), repeated every line. The
// vertical axis uses the identical layout counted in whole lines:
//
//   |<--- H_ACTIVE --->|<- H_FRONT ->|<- H_SYNC ->|<- H_BACK ->|
//   0               639            porch         sync         back   799
//   de = 1 here -------^   de = 0 (blanking) for the rest of the line
//
// Note the ordering: active video, THEN front porch, THEN the sync
// pulse, THEN back porch — the sync window does not begin at H_ACTIVE.

module video_timing #(
    // Display geometry; defaults are console mode 0 (see video_pkg).
    // Horizontal values are pixel-clock cycles, vertical are whole lines.
    parameter int H_ACTIVE   = video_pkg::H_ACTIVE,
    parameter int H_FRONT    = video_pkg::H_FRONT,
    parameter int H_SYNC     = video_pkg::H_SYNC,
    parameter int H_BACK     = video_pkg::H_BACK,
    parameter int V_ACTIVE   = video_pkg::V_ACTIVE,
    parameter int V_FRONT    = video_pkg::V_FRONT,
    parameter int V_SYNC     = video_pkg::V_SYNC,
    parameter int V_BACK     = video_pkg::V_BACK,
    // Sync polarity: the level driven while a sync pulse is asserted.
    parameter bit H_SYNC_POL = video_pkg::H_SYNC_POL,
    parameter bit V_SYNC_POL = video_pkg::V_SYNC_POL
) (
    input  logic        i_clk,    // pixel clock
    input  logic        i_rst,    // synchronous, active-high
    output logic [11:0] o_x,      // column, valid while o_de (0..H_ACTIVE-1)
    output logic [11:0] o_y,      // row,    valid while o_de (0..V_ACTIVE-1)
    output logic        o_de,     // data enable — inside the active region
    output logic        o_hsync,  // horizontal sync, at H_SYNC_POL when asserted
    output logic        o_vsync   // vertical sync,   at V_SYNC_POL when asserted
);

    localparam int H_TOTAL = H_ACTIVE + H_FRONT + H_SYNC + H_BACK;
    localparam int V_TOTAL = V_ACTIVE + V_FRONT + V_SYNC + V_BACK;

    // Comparison bounds, sized to the 12-bit raster counters so counter
    // and constant widths match. Sync windows are half-open [start, end)
    // spans, so the constant math is written once here.
    localparam logic [11:0] H_LAST       = 12'(H_TOTAL - 1);
    localparam logic [11:0] V_LAST       = 12'(V_TOTAL - 1);
    localparam logic [11:0] H_ACT        = 12'(H_ACTIVE);
    localparam logic [11:0] V_ACT        = 12'(V_ACTIVE);
    localparam logic [11:0] H_SYNC_START = 12'(H_ACTIVE + H_FRONT);
    localparam logic [11:0] H_SYNC_END   = 12'(H_ACTIVE + H_FRONT + H_SYNC);
    localparam logic [11:0] V_SYNC_START = 12'(V_ACTIVE + V_FRONT);
    localparam logic [11:0] V_SYNC_END   = 12'(V_ACTIVE + V_FRONT + V_SYNC);

    // Free-running raster position. hcount advances every pixel and wraps
    // at end of line; vcount advances once per line and wraps at end of
    // frame.
    logic [11:0] hcount;
    logic [11:0] vcount;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            hcount <= '0;
            vcount <= '0;
        end else if (hcount == H_LAST) begin
            hcount <= '0;
            vcount <= (vcount == V_LAST) ? 12'd0 : vcount + 12'd1;
        end else begin
            hcount <= hcount + 12'd1;
        end
    end

    // Active-region coordinate and data-enable. o_x/o_y are only
    // meaningful while o_de is high; downstream fetch uses them to index
    // the cell grid.
    assign o_x  = hcount;
    assign o_y  = vcount;
    assign o_de = (hcount < H_ACT) && (vcount < V_ACT);

    // Sync pulses: a named in-window flag per axis, then mapped to the
    // configured polarity — inside the window the output drives its
    // *_SYNC_POL level, outside it the opposite.
    logic in_hsync, in_vsync;
    assign in_hsync = (hcount >= H_SYNC_START) && (hcount < H_SYNC_END);
    assign in_vsync = (vcount >= V_SYNC_START) && (vcount < V_SYNC_END);

    assign o_hsync = in_hsync ? H_SYNC_POL : ~H_SYNC_POL;
    assign o_vsync = in_vsync ? V_SYNC_POL : ~V_SYNC_POL;

endmodule
