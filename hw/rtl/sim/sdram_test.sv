// SDRAM controller unit-test wrapper.
//
// Wires sdram_ctrl + sdram_phy_sim + sdram_model into a single DUT
// for the C++ testbench (`tb_sdram_test.cpp`).  Uses the W9825-100MHz
// chip preset but with a tiny T_POWERUP so init completes in a few
// cycles instead of 200 µs of sim time.
//
// The testbench drives the controller's req/rsp interface directly;
// no bus adapter or CDC bridge in the loop.

module sdram_test
    import sdram_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic        i_req_valid,
    input  logic        i_req_we,
    input  logic [31:0] i_req_addr,
    input  logic [31:0] i_req_wdata,
    input  logic [3:0]  i_req_byte_en,
    output logic        o_req_ready,

    output logic        o_rsp_valid,
    output logic [31:0] o_rsp_data,
    input  logic        i_rsp_ready,

    output logic        o_init_done,
    output logic        o_done,
    output logic [31:0] o_protocol_errors
);

    // ── Controller ↔ PHY signals ───────────────────────────
    logic [3:0]                              ctrl_cmd;
    logic                                    ctrl_cke;
    logic [W9825_100_ROW_BITS-1:0]           ctrl_a;
    logic [W9825_100_BA_BITS-1:0]            ctrl_ba;
    logic [W9825_100_DQ_BITS/8-1:0]          ctrl_dqm;
    logic [W9825_100_DQ_BITS-1:0]            ctrl_dq_out;
    logic                                    ctrl_dq_oe;
    logic [W9825_100_DQ_BITS-1:0]            ctrl_dq_in;

    sdram_ctrl #(
        .ROW_BITS    (W9825_100_ROW_BITS),
        .COL_BITS    (W9825_100_COL_BITS),
        .BA_BITS     (W9825_100_BA_BITS),
        .DQ_BITS     (W9825_100_DQ_BITS),
        .T_RCD       (W9825_100_T_RCD),
        .T_RP        (W9825_100_T_RP),
        .T_RFC       (W9825_100_T_RFC),
        .T_WR        (W9825_100_T_WR),
        .T_MRD       (W9825_100_T_MRD),
        .T_REFI      (W9825_100_T_REFI),
        .T_POWERUP   (W9825_SIM_T_POWERUP),
        .CAS_LATENCY (W9825_100_CAS_LATENCY)
    ) u_ctrl (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_req_valid   (i_req_valid),
        .i_req_we      (i_req_we),
        .i_req_addr    (i_req_addr),
        .i_req_wdata   (i_req_wdata),
        .i_req_byte_en (i_req_byte_en),
        .o_req_ready   (o_req_ready),
        .o_rsp_valid   (o_rsp_valid),
        .o_rsp_data    (o_rsp_data),
        .i_rsp_ready   (i_rsp_ready),
        .o_done        (o_done),
        .o_phy_cmd     (ctrl_cmd),
        .o_phy_cke     (ctrl_cke),
        .o_phy_a       (ctrl_a),
        .o_phy_ba      (ctrl_ba),
        .o_phy_dqm     (ctrl_dqm),
        .o_phy_dq_out  (ctrl_dq_out),
        .o_phy_dq_oe   (ctrl_dq_oe),
        .i_phy_dq_in   (ctrl_dq_in),
        .o_dbg_init_done (o_init_done)
    );

    // ── PHY ↔ chip pin signals ─────────────────────────────
    logic                              sd_clk, sd_cke, sd_csn, sd_rasn, sd_casn, sd_wen;
    logic [W9825_100_ROW_BITS-1:0]    sd_a;
    logic [W9825_100_BA_BITS-1:0]     sd_ba;
    logic [W9825_100_DQ_BITS/8-1:0]   sd_dqm;
    wire  [W9825_100_DQ_BITS-1:0]     sd_d;

    sdram_phy_sim #(
        .ROW_BITS (W9825_100_ROW_BITS),
        .BA_BITS  (W9825_100_BA_BITS),
        .DQ_BITS  (W9825_100_DQ_BITS)
    ) u_phy (
        .i_clk        (i_clk),
        .i_phy_cmd    (ctrl_cmd),
        .i_phy_cke    (ctrl_cke),
        .i_phy_a      (ctrl_a),
        .i_phy_ba     (ctrl_ba),
        .i_phy_dqm    (ctrl_dqm),
        .i_phy_dq_out (ctrl_dq_out),
        .i_phy_dq_oe  (ctrl_dq_oe),
        .o_phy_dq_in  (ctrl_dq_in),
        .o_sdram_clk  (sd_clk),
        .o_sdram_cke  (sd_cke),
        .o_sdram_csn  (sd_csn),
        .o_sdram_rasn (sd_rasn),
        .o_sdram_casn (sd_casn),
        .o_sdram_wen  (sd_wen),
        .o_sdram_a    (sd_a),
        .o_sdram_ba   (sd_ba),
        .o_sdram_dqm  (sd_dqm),
        .io_sdram_d   (sd_d)
    );

    sdram_model #(
        .ROW_BITS (W9825_100_ROW_BITS),
        .COL_BITS (W9825_100_COL_BITS),
        .BA_BITS  (W9825_100_BA_BITS),
        .DQ_BITS  (W9825_100_DQ_BITS)
    ) u_model (
        .i_clk  (sd_clk),
        .i_cke  (sd_cke),
        .i_csn  (sd_csn),
        .i_rasn (sd_rasn),
        .i_casn (sd_casn),
        .i_wen  (sd_wen),
        .i_a    (sd_a),
        .i_ba   (sd_ba),
        .i_dqm  (sd_dqm),
        .io_d   (sd_d)
    );

    assign o_protocol_errors = u_model.protocol_errors;

endmodule
