// Sim-only SDRAM subsystem: adapter + CDC + controller + sim PHY + chip.
//
// Bus interface mirrors simple_mem / fpga_ram so machine_sim can swap
// in this stack at the RAM region without other plumbing changes.
// Internally instantiates the full v2 SDRAM stack for end-to-end
// verification of the controller against the behavioral model.
//
// As of step 4 the CDC bridge sits between the bus adapter and the
// controller.  In sim we tie sys_clk == sdram_clk so the CDC paths
// are exercised end-to-end (toggle launch, 2-FF sync, edge detect,
// done propagation) but at the same rate — the actual two-domain
// operation only happens on hardware where the PLL hands the
// controller its own 100 MHz CLKOS.  Same-rate sim is enough to
// catch FSM/handshake bugs; metastability behaviour is a hardware
// concern outside Verilator's model.
//
// Uses the W9825-100MHz preset with the small `W9825_SIM_T_POWERUP`
// so init completes in a few cycles instead of 200 µs of sim time.

module sdram_sim
    import sdram_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy
);

    // ── Adapter ↔ CDC (sys side) ───────────────────────────
    logic        sys_req_valid, sys_req_we, sys_req_ready;
    logic [31:0] sys_req_addr, sys_req_wdata;
    logic [3:0]  sys_req_byte_en;
    logic        sys_rsp_valid, sys_rsp_ready, sys_done;
    logic [31:0] sys_rsp_data;

    sdram_bus_adapter u_adp (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_addr        (i_addr),
        .i_wdata       (i_wdata),
        .i_byte_en     (i_byte_en),
        .i_we          (i_we),
        .i_re          (i_re),
        .o_rdata       (o_rdata),
        .o_busy        (o_busy),
        .o_req_valid   (sys_req_valid),
        .o_req_we      (sys_req_we),
        .o_req_addr    (sys_req_addr),
        .o_req_wdata   (sys_req_wdata),
        .o_req_byte_en (sys_req_byte_en),
        .i_req_ready   (sys_req_ready),
        .i_rsp_valid   (sys_rsp_valid),
        .i_rsp_data    (sys_rsp_data),
        .o_rsp_ready   (sys_rsp_ready),
        .i_done        (sys_done)
    );

    // ── CDC ↔ controller (sdram side) ───────────────────────
    logic        req_valid, req_we, req_ready;
    logic [31:0] req_addr, req_wdata;
    logic [3:0]  req_byte_en;
    logic        rsp_valid, rsp_ready, done;
    logic [31:0] rsp_data;

    sdram_cdc u_cdc (
        // Sys side (single-clock for sim — same i_clk on both ports)
        .i_sys_clk         (i_clk),
        .i_sys_rst         (i_rst),
        .i_sys_req_valid   (sys_req_valid),
        .i_sys_req_we      (sys_req_we),
        .i_sys_req_addr    (sys_req_addr),
        .i_sys_req_wdata   (sys_req_wdata),
        .i_sys_req_byte_en (sys_req_byte_en),
        .o_sys_req_ready   (sys_req_ready),
        .o_sys_rsp_valid   (sys_rsp_valid),
        .o_sys_rsp_data    (sys_rsp_data),
        .i_sys_rsp_ready   (sys_rsp_ready),
        .o_sys_done        (sys_done),
        // Sdram side
        .i_sd_clk          (i_clk),
        .i_sd_rst          (i_rst),
        .o_sd_req_valid    (req_valid),
        .o_sd_req_we       (req_we),
        .o_sd_req_addr     (req_addr),
        .o_sd_req_wdata    (req_wdata),
        .o_sd_req_byte_en  (req_byte_en),
        .i_sd_req_ready    (req_ready),
        .i_sd_rsp_valid    (rsp_valid),
        .i_sd_rsp_data     (rsp_data),
        .o_sd_rsp_ready    (rsp_ready),
        .i_sd_done         (done)
    );

    // ── Controller ↔ PHY ───────────────────────────────────
    logic [3:0]                       ctrl_cmd;
    logic                             ctrl_cke;
    logic [W9825_100_ROW_BITS-1:0]    ctrl_a;
    logic [W9825_100_BA_BITS-1:0]     ctrl_ba;
    logic [W9825_100_DQ_BITS/8-1:0]   ctrl_dqm;
    logic [W9825_100_DQ_BITS-1:0]     ctrl_dq_out;
    logic                             ctrl_dq_oe;
    logic [W9825_100_DQ_BITS-1:0]     ctrl_dq_in;
    logic                             init_done_unused;

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
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_req_valid     (req_valid),
        .i_req_we        (req_we),
        .i_req_addr      (req_addr),
        .i_req_wdata     (req_wdata),
        .i_req_byte_en   (req_byte_en),
        .o_req_ready     (req_ready),
        .o_rsp_valid     (rsp_valid),
        .o_rsp_data      (rsp_data),
        .i_rsp_ready     (rsp_ready),
        .o_done          (done),
        .o_phy_cmd       (ctrl_cmd),
        .o_phy_cke       (ctrl_cke),
        .o_phy_a         (ctrl_a),
        .o_phy_ba        (ctrl_ba),
        .o_phy_dqm       (ctrl_dqm),
        .o_phy_dq_out    (ctrl_dq_out),
        .o_phy_dq_oe     (ctrl_dq_oe),
        .i_phy_dq_in     (ctrl_dq_in),
        .o_dbg_init_done (init_done_unused)
    );

    // ── PHY → chip pin signals ────────────────────────────
    logic                              sd_clk, sd_cke, sd_csn, sd_rasn, sd_casn, sd_wen;
    logic [W9825_100_ROW_BITS-1:0]     sd_a;
    logic [W9825_100_BA_BITS-1:0]      sd_ba;
    logic [W9825_100_DQ_BITS/8-1:0]    sd_dqm;
    wire  [W9825_100_DQ_BITS-1:0]      sd_d;

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

endmodule
