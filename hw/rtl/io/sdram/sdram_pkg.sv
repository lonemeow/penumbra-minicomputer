// SDR SDRAM controller — shared definitions and chip presets
//
// Single source of truth for:
//   • SDRAM command encoding (used by controller and behavioral model).
//   • Mode register field positions.
//   • Per-chip / per-clock timing presets, named like
//     `<CHIP>_<CLOCKMHZ>_<FIELD>` so a top-level instantiation reads:
//
//         sdram_ctrl #(
//             .ROW_BITS (W9825_100_ROW_BITS),
//             .T_RCD    (W9825_100_T_RCD),
//             ...
//         ) u_ctrl ( ... );
//
// Adding a new chip preset = adding one block of `localparam int`s.
// See doc/internals/sdram-controller.md for the design plan.

/* verilator lint_off UNUSEDPARAM */
package sdram_pkg;

    // ── Command encoding {CSn, RASn, CASn, WEn} ─────────────────
    // CSn=1 → command inhibit. With CSn=0 the {RAS,CAS,WE} triplet
    // selects the operation. Keep these as the de-facto JEDEC table.
    localparam logic [3:0] SDRAM_CMD_INHIBIT   = 4'b1111;
    localparam logic [3:0] SDRAM_CMD_NOP       = 4'b0111;
    localparam logic [3:0] SDRAM_CMD_ACTIVATE  = 4'b0011;
    localparam logic [3:0] SDRAM_CMD_READ      = 4'b0101;
    localparam logic [3:0] SDRAM_CMD_WRITE     = 4'b0100;
    localparam logic [3:0] SDRAM_CMD_PRECHARGE = 4'b0010;
    localparam logic [3:0] SDRAM_CMD_REFRESH   = 4'b0001;
    localparam logic [3:0] SDRAM_CMD_MODE_SET  = 4'b0000;
    localparam logic [3:0] SDRAM_CMD_BURST_TER = 4'b0110;

    // ── Mode register field layout (driven on A[12:0] during MRS) ──
    // A[2:0]   burst length (000=BL1, 001=BL2, 010=BL4, 011=BL8,
    //                        111=full page)
    // A[3]     burst type   (0=sequential, 1=interleaved)
    // A[6:4]   CAS latency  (010=CL2, 011=CL3)
    // A[8:7]   test/operating mode (00=normal)
    // A[9]     write burst  (0=programmed, 1=single-bit)
    // A[12:10] reserved (must be 0)
    localparam logic [2:0] SDRAM_BL_1     = 3'b000;
    localparam logic [2:0] SDRAM_BL_2     = 3'b001;
    localparam logic [2:0] SDRAM_BL_4     = 3'b010;
    localparam logic [2:0] SDRAM_BL_8     = 3'b011;
    localparam logic [2:0] SDRAM_BL_PAGE  = 3'b111;
    localparam logic      SDRAM_BT_SEQ    = 1'b0;
    localparam logic      SDRAM_BT_INTLV  = 1'b1;

    // ─────────────────────────────────────────────────────────────
    // Chip preset: Winbond W9825G6KH (default ULX3S SDRAM)
    // 32 MB = 4 banks × 8192 rows × 512 cols × 16 bits.
    //
    // Conservative timings @ 100 MHz (10 ns period). Values below are
    // chosen to satisfy the slowest -7 grade variant and round up
    // each datasheet ns figure to whole cycles.
    // ─────────────────────────────────────────────────────────────
    localparam int W9825_100_ROW_BITS    = 13;
    localparam int W9825_100_COL_BITS    = 9;
    localparam int W9825_100_BA_BITS     = 2;
    localparam int W9825_100_DQ_BITS     = 16;

    localparam int W9825_100_T_RCD       = 2;     // 18-21 ns  → 2 cyc
    localparam int W9825_100_T_RP        = 2;     // 18-21 ns  → 2 cyc
    localparam int W9825_100_T_RC        = 7;     // 60-63 ns  → 7 cyc
    localparam int W9825_100_T_RAS       = 5;     // 42 ns min → 5 cyc
    localparam int W9825_100_T_RFC       = 7;     // 60-66 ns  → 7 cyc
    localparam int W9825_100_T_WR        = 2;     // 1 CLK + 7.5 ns
    localparam int W9825_100_T_MRD       = 2;     // 2 CLK
    localparam int W9825_100_T_REFI      = 750;   // 7.81 µs → 781, use 750 for margin
    localparam int W9825_100_T_POWERUP   = 20000; // 200 µs @ 100 MHz
    localparam int W9825_100_CAS_LATENCY = 2;

    // ─────────────────────────────────────────────────────────────
    // Sim preset: same chip, but skip the 200 µs power-up wait so
    // unit tests start in milliseconds-of-sim-time, not seconds.
    // ─────────────────────────────────────────────────────────────
    localparam int W9825_SIM_T_POWERUP   = 8;

endpackage
/* verilator lint_on UNUSEDPARAM */
