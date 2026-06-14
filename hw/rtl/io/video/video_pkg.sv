// Penumbra text-video console — shared display constants.
//
// The canonical timing geometry for console mode 0 (VESA 640x480 @ 60),
// kept in one place so the timing generator, the pattern generator, and
// any other pixel-domain module agree on the active resolution and the
// blanking/sync structure. Horizontal values are pixel-clock cycles;
// vertical values are whole lines.

package video_pkg;

    // Mode 0 — VESA 640x480 @ 60 Hz.
    localparam int H_ACTIVE = 640;
    localparam int H_FRONT  = 16;
    localparam int H_SYNC   = 96;
    localparam int H_BACK   = 48;
    localparam int V_ACTIVE = 480;
    localparam int V_FRONT  = 10;
    localparam int V_SYNC   = 2;
    localparam int V_BACK   = 33;

    // Sync polarity: the level driven while a pulse is asserted
    // (active-low on both axes for this mode).
    localparam bit H_SYNC_POL = 1'b0;
    localparam bit V_SYNC_POL = 1'b0;

endpackage
