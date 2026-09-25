// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 Penumbra Minicomputer Contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at
// your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Nexys Video DDR3: the board's DDR3 chip behind its controller
//
// The only file of this repository that instantiates UberDDR3
// (hw/third_party/uberddr3/), and so the one licensed GPL-3.0-or-later
// rather than BSD 2-Clause; everything outside it sees the ports below
// and nothing of UberDDR3. See that directory's README for the boundary.
//
// The configuration is the board's: an MT41K256M16 (4 Gb, x16) as two
// byte lanes, 15 row, 10 column and 3 bank bits, at a 4:1 controller
// ratio on the clocks nexysvideo_ddr3_clk makes. Its bank has no output
// delay lines, so the PHY launches DQS off the 90-degree clock instead.
// Self-refresh stays under the (tied-off) input's control: the automatic
// modes put the chip to sleep after a few hundred idle cycles, and the
// next access then waits out the self-refresh exit.
//
// Ports:
//   - the memory port, pipelined Wishbone: one 128-bit burst (16 bytes,
//     the chip's BL8) per request, addressed in bursts; i_sel masks
//     bytes on writes; i_aux rides along and returns with the ack, so a
//     consumer can tell a write's ack from a read's. The bus cycle is
//     held open, so nothing in flight is ever cancelled.
//   - the debug port, UberDDR3's second Wishbone: a small register map
//     of PHY delays and self-test counters (ddr3_controller.v).
//   - o_calib_complete: calibration, and the self-test when BIST_MODE
//     asks for one, finished. A self-test mismatch before then resets
//     the controller into another attempt, so it never rises on a chip
//     that fails the test. o_calib_state is the calibration step.
//
// BIST_MODE picks the self-test calibration ends with: 0 none, 1 one
// pass over the whole address space, 2 that pass for every pattern
// (burst, random, alternating). Byte-masked writes are part of it.

module nexysvideo_ddr3 #(
    parameter int BIST_MODE  = 0,
    parameter bit MICRON_SIM = 1'b0,   // shortened power-up waits, for simulation only

    localparam int ROW_BITS   = 15,
    localparam int COL_BITS   = 10,
    localparam int BA_BITS    = 3,
    localparam int BYTE_LANES = 2,
    localparam int AUX_WIDTH  = 4,
    localparam int ADDR_BITS  = ROW_BITS + COL_BITS + BA_BITS - 3,  // bursts of eight
    localparam int DATA_BITS  = 8 * BYTE_LANES * 8,
    localparam int SEL_BITS   = DATA_BITS / 8,
    localparam int DBG_ADDR_BITS = 7
) (
    input  logic                     i_ctrl_clk,
    input  logic                     i_ddr3_clk,
    input  logic                     i_ddr3_clk_90,
    input  logic                     i_ref_clk,
    input  logic                     i_rst_n,

    // ── Memory port ─────────────────────────────────────────
    input  logic                     i_stb,
    input  logic                     i_we,
    input  logic [ADDR_BITS-1:0]     i_addr,
    input  logic [DATA_BITS-1:0]     i_data,
    input  logic [SEL_BITS-1:0]      i_sel,
    input  logic [AUX_WIDTH-1:0]     i_aux,
    output logic                     o_stall,
    output logic                     o_ack,
    output logic [DATA_BITS-1:0]     o_data,
    output logic [AUX_WIDTH-1:0]     o_aux,

    // ── Debug port ──────────────────────────────────────────
    input  logic                     i_dbg_stb,
    input  logic                     i_dbg_we,
    input  logic [DBG_ADDR_BITS-1:0] i_dbg_addr,
    input  logic [31:0]              i_dbg_data,
    output logic                     o_dbg_stall,
    output logic                     o_dbg_ack,
    output logic [31:0]              o_dbg_data,

    output logic                     o_calib_complete,
    output logic [4:0]               o_calib_state,

    // ── DDR3 pins ───────────────────────────────────────────
    output logic                     o_ddr3_ck_p,
    output logic                     o_ddr3_ck_n,
    output logic                     o_ddr3_reset_n,
    output logic                     o_ddr3_cke,
    output logic                     o_ddr3_ras_n,
    output logic                     o_ddr3_cas_n,
    output logic                     o_ddr3_we_n,
    output logic [ROW_BITS-1:0]      o_ddr3_addr,
    output logic [BA_BITS-1:0]       o_ddr3_ba,
    inout  wire  [8*BYTE_LANES-1:0]  io_ddr3_dq,
    inout  wire  [BYTE_LANES-1:0]    io_ddr3_dqs_p,
    inout  wire  [BYTE_LANES-1:0]    io_ddr3_dqs_n,
    output logic [BYTE_LANES-1:0]    o_ddr3_dm,
    output logic                     o_ddr3_odt
);
    logic [31:0] debug1;

    /* verilator lint_off PINCONNECTEMPTY */
    ddr3_top #(
        .CONTROLLER_CLK_PERIOD (12_000),
        .DDR3_CLK_PERIOD       (3_000),
        .ROW_BITS              (ROW_BITS),
        .COL_BITS              (COL_BITS),
        .BA_BITS               (BA_BITS),
        .BYTE_LANES            (BYTE_LANES),
        .AUX_WIDTH             (AUX_WIDTH),
        .WB2_ADDR_BITS         (DBG_ADDR_BITS),
        .WB2_DATA_BITS         (32),
        .DUAL_RANK_DIMM        (0),
        .SPEED_BIN             (3),
        .SDRAM_CAPACITY        (4),
        .MICRON_SIM            (MICRON_SIM),
        .ODELAY_SUPPORTED      (0),
        .SECOND_WISHBONE       (1),
        .DLL_OFF               (0),
        .WB_ERROR              (0),
        .BIST_MODE             (2'(BIST_MODE)),
        .BIST_TEST_DATAMASK    (1),
        .ECC_ENABLE            (0),
        .SELF_REFRESH          (2'b00)
    ) u_ddr3 (
        .i_controller_clk    (i_ctrl_clk),
        .i_ddr3_clk          (i_ddr3_clk),
        .i_ref_clk           (i_ref_clk),
        .i_ddr3_clk_90       (i_ddr3_clk_90),
        .i_rst_n             (i_rst_n),

        .i_wb_cyc            (1'b1),
        .i_wb_stb            (i_stb),
        .i_wb_we             (i_we),
        .i_wb_addr           (i_addr),
        .i_wb_data           (i_data),
        .i_wb_sel            (i_sel),
        .i_aux               (i_aux),
        .o_wb_stall          (o_stall),
        .o_wb_ack            (o_ack),
        .o_wb_err            (),
        .o_wb_data           (o_data),
        .o_aux               (o_aux),

        .i_wb2_cyc           (1'b1),
        .i_wb2_stb           (i_dbg_stb),
        .i_wb2_we            (i_dbg_we),
        .i_wb2_addr          (i_dbg_addr),
        .i_wb2_data          (i_dbg_data),
        .i_wb2_sel           (4'hF),
        .o_wb2_stall         (o_dbg_stall),
        .o_wb2_ack           (o_dbg_ack),
        .o_wb2_data          (o_dbg_data),

        .o_ddr3_clk_p        (o_ddr3_ck_p),
        .o_ddr3_clk_n        (o_ddr3_ck_n),
        .o_ddr3_reset_n      (o_ddr3_reset_n),
        .o_ddr3_cke          (o_ddr3_cke),
        .o_ddr3_cs_n         (),                 // the board ties CS# low
        .o_ddr3_ras_n        (o_ddr3_ras_n),
        .o_ddr3_cas_n        (o_ddr3_cas_n),
        .o_ddr3_we_n         (o_ddr3_we_n),
        .o_ddr3_addr         (o_ddr3_addr),
        .o_ddr3_ba_addr      (o_ddr3_ba),
        .io_ddr3_dq          (io_ddr3_dq),
        .io_ddr3_dqs         (io_ddr3_dqs_p),
        .io_ddr3_dqs_n       (io_ddr3_dqs_n),
        .o_ddr3_dm           (o_ddr3_dm),
        .o_ddr3_odt          (o_ddr3_odt),

        .o_calib_complete    (o_calib_complete),
        .o_debug1            (debug1),
        .i_user_self_refresh (1'b0),
        .uart_tx             ()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // The calibration state machine's step, in the low bits of UberDDR3's
    // first debug word.
    assign o_calib_state = debug1[4:0];

endmodule
