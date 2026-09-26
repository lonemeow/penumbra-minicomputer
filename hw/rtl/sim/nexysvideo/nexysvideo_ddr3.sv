// nexysvideo_ddr3 — the Nexys Video's DDR3 controller, as simulation sees it
//
// Stands in for hw/rtl/fpga/nexysvideo/nexysvideo_ddr3.sv, and for
// UberDDR3 behind it, wherever a Nexys top is built for Verilator: the
// board top's testbench and fpga-lint put this directory ahead of the
// board's, so this file shadows the wrapper by name. Synthesis always
// uses the wrapper. The parameters and ports are the wrapper's, so the
// top is built against its real interface; keep the two in step.
//
// The memory port is wb_line_mem's model of the controller's: random
// stall and latency, calibration first after the controller's reset,
// and all 512 MiB. The debug port never answers and the pins sit idle.
//
// The real controller is out of reach here twice over. Its PHY is 7-series
// I/O serializers and delay lines, which need a DRAM model and an
// event-driven simulator; and at the board's configuration its vendored
// source does not elaborate for lint or simulation (a replication count
// its ECC-only branch makes negative at a 4-bit aux, which Vivado
// accepts in dead code).

/* verilator lint_off UNUSEDSIGNAL */
/* verilator lint_off UNUSEDPARAM */
/* verilator lint_off UNDRIVEN */

module nexysvideo_ddr3 #(
    parameter int BIST_MODE  = 0,
    parameter bit MICRON_SIM = 1'b0,

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
    input  logic                     i_cyc,
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
    // Long enough that a machine not held in reset through it would
    // fault its first RAM access on the bus watchdog.
    localparam int CALIB_CYCLES = 4096;

    wb_line_mem #(
        .ADDR_BITS        (ADDR_BITS),
        .AUX_WIDTH        (AUX_WIDTH),
        .CALIB_CYCLES     (CALIB_CYCLES)
    ) u_mem (
        .i_clk            (i_ctrl_clk),
        .i_rst            (!i_rst_n),
        .i_cyc            (i_cyc),
        .i_stb            (i_stb),
        .i_we             (i_we),
        .i_addr           (i_addr),
        .i_data           (i_data),
        .i_sel            (i_sel),
        .i_aux            (i_aux),
        .o_stall          (o_stall),
        .o_ack            (o_ack),
        .o_data           (o_data),
        .o_aux            (o_aux),
        .o_calib_complete (o_calib_complete)
    );

    assign o_dbg_stall    = 1'b1;
    assign o_dbg_ack      = 1'b0;
    assign o_dbg_data     = '0;
    assign o_calib_state  = '0;
    assign o_ddr3_ck_p    = 1'b0;
    assign o_ddr3_ck_n    = 1'b1;
    assign o_ddr3_reset_n = 1'b0;
    assign o_ddr3_cke     = 1'b0;
    assign o_ddr3_ras_n   = 1'b1;
    assign o_ddr3_cas_n   = 1'b1;
    assign o_ddr3_we_n    = 1'b1;
    assign o_ddr3_addr    = '0;
    assign o_ddr3_ba      = '0;
    assign o_ddr3_dm      = '0;
    assign o_ddr3_odt     = 1'b0;
endmodule
