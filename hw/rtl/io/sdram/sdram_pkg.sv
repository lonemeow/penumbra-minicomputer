// SDR SDRAM controller — shared definitions and chip presets
//
// Single source of truth for:
//   • SDRAM command encoding (used by controller and behavioral model).
//   • Mode register field positions.
//   • Per-chip / per-clock timing presets, packaged as a single
//     `sdram_params_t` struct each.  A board top picks one with a
//     one-line localparam alias, then threads its fields through the
//     controller's parameter ports:
//
//         localparam sdram_params_t SDP = sdram_pkg::W9825_100;
//
//         sdram_ctrl #(
//             .ROW_BITS (SDP.ROW_BITS),
//             .T_RCD    (SDP.T_RCD),
//             ...
//         ) u_ctrl ( ... );
//
// Adding a new chip preset = adding one block: a `localparam
// sdram_params_t <NAME>_<CLOCKMHZ> = '{ ROW_BITS: …, T_RCD: …, … };`.
// Switching presets at a board top is a one-line edit.
//
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
    // Chip parameter record
    //
    // All timing fields are in cycles at the target SDRAM clock rate.
    // Geometry fields are bit widths (so DQ_BITS=16 → 16-bit DQ bus).
    //
    // typedef struct packed: each field gets fixed-width storage so
    // sv2v / yosys evaluate field accesses to constants at elab time.
    // The struct only carries fields the controller actually consumes
    // — adding new ones (e.g. T_RAS, T_RC) is a one-line append when
    // a future controller revision needs them.
    // ─────────────────────────────────────────────────────────────
    typedef struct packed {
        int ROW_BITS;
        int COL_BITS;
        int BA_BITS;
        int DQ_BITS;
        int T_RCD;
        int T_RP;
        int T_RFC;
        int T_WR;
        int T_MRD;
        int T_REFI;
        int T_POWERUP;
        int CAS_LATENCY;
    } sdram_params_t;

    // ─────────────────────────────────────────────────────────────
    // Chip preset: Winbond W9825G6KH (default ULX3S SDRAM)
    // 32 MB = 4 banks × 8192 rows × 512 cols × 16 bits.
    //
    // Conservative timings @ 100 MHz (10 ns period).  Values chosen
    // to satisfy the slowest -7 grade variant; each datasheet ns
    // figure rounded up to whole cycles.
    // ─────────────────────────────────────────────────────────────
    localparam sdram_params_t W9825_100 = '{
        ROW_BITS:       13,
        COL_BITS:        9,
        BA_BITS:         2,
        DQ_BITS:        16,
        T_RCD:           2,    // 18-21 ns → 2 cyc @ 10 ns
        T_RP:            2,    // 18-21 ns → 2 cyc
        T_RFC:           7,    // 60-66 ns → 7 cyc
        T_WR:            2,    // 1 CLK + 7.5 ns
        T_MRD:           2,    // 2 CLK
        T_REFI:        750,    // 7.81 µs → 781 cyc, 750 for margin
        T_POWERUP:   20000,    // 200 µs @ 100 MHz
        CAS_LATENCY:     2
    };

    // ─────────────────────────────────────────────────────────────
    // Sim power-up override.
    //
    // Skipping the 200 µs JEDEC power-up wait keeps unit tests in
    // milliseconds-of-sim-time, not seconds.  Sim instantiation
    // sites override only this one field at the parameter port:
    //
    //   sdram_ctrl #(
    //       .T_POWERUP (W9825_SIM_T_POWERUP),
    //       /* ...other fields from SDP... */
    //   )
    //
    // Mixing struct-field overrides with explicit ones is legal in
    // SV parameter port lists.
    // ─────────────────────────────────────────────────────────────
    localparam int W9825_SIM_T_POWERUP = 8;

endpackage
/* verilator lint_on UNUSEDPARAM */
