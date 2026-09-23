// SDRAM memory stack: bus adapter → CDC → controller.
//
// Everything between a system-bus slave port and the PHY command
// interface, as one unit.  Board tops and the sim wrapper instantiate
// it and supply the PHY themselves: the PHY is the FPGA-family- (or
// simulation-) specific layer, while this stack is identical wherever
// the SDRAM appears, so the adapter/CDC/controller wiring lives in one
// place instead of once per integration.
//
// The stack spans both clock domains: the adapter and the CDC's
// request side run on the system clock, the CDC's controller side and
// the controller on the SDRAM clock.
//
// No keep_hierarchy here: the stack is pure wiring and must flatten
// into its parent so the adapter keeps sharing logic with the bus
// decode around it.  sdram_cdc and sdram_ctrl carry their own
// attribute, so their instances stay addressable as u_cdc / u_ctrl
// under this stack's instance — the name a placement floorplan matches.

module sdram_stack
    import sdram_pkg::*;
#(
    parameter sdram_params_t SDP = W9825_100,

    // Separately overridable so a simulation can skip the JEDEC
    // power-up wait without restating the chip preset.
    parameter int T_POWERUP = SDP.T_POWERUP,

    // Registering-PHY pipeline depth — see sdram_ctrl.
    parameter int PHY_OUT_LATENCY = 0,
    parameter int PHY_IN_LATENCY  = 0
) (
    input  logic                      i_sys_clk,
    input  logic                      i_sys_rst,
    input  logic                      i_sd_clk,
    input  logic                      i_sd_rst,

    // ── System-bus slave port (system clock) ─────────────────
    input  logic [31:0]               i_addr,
    input  logic [31:0]               i_wdata,
    input  logic [3:0]                i_byte_en,
    input  logic                      i_we,
    input  logic                      i_re,
    output logic [31:0]               o_rdata,
    output logic                      o_ack,

    // ── PHY command interface (SDRAM clock) ──────────────────
    output logic [3:0]                o_phy_cmd,
    output logic                      o_phy_cke,
    output logic [SDP.ROW_BITS-1:0]   o_phy_a,
    output logic [SDP.BA_BITS-1:0]    o_phy_ba,
    output logic [SDP.DQ_BITS/8-1:0]  o_phy_dqm,
    output logic [SDP.DQ_BITS-1:0]    o_phy_dq_out,
    output logic                      o_phy_dq_oe,
    input  logic [SDP.DQ_BITS-1:0]    i_phy_dq_in,

    output logic                      o_dbg_init_done
);

    // ── Adapter ↔ CDC (system clock) ─────────────────────────
    logic        sys_req_valid, sys_req_we, sys_req_ready;
    logic [31:0] sys_req_addr,  sys_req_wdata;
    logic [3:0]  sys_req_byte_en;
    logic        sys_rsp_valid, sys_rsp_ready, sys_done;
    logic [31:0] sys_rsp_data;

    sdram_bus_adapter u_adp (
        .i_clk         (i_sys_clk),
        .i_rst         (i_sys_rst),
        .i_addr        (i_addr),
        .i_wdata       (i_wdata),
        .i_byte_en     (i_byte_en),
        .i_we          (i_we),
        .i_re          (i_re),
        .o_rdata       (o_rdata),
        .o_ack         (o_ack),
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

    // ── CDC ↔ controller (SDRAM clock) ───────────────────────
    logic        sd_req_valid, sd_req_we, sd_req_ready;
    logic [31:0] sd_req_addr,  sd_req_wdata;
    logic [3:0]  sd_req_byte_en;
    logic        sd_rsp_valid, sd_rsp_ready, sd_done;
    logic [31:0] sd_rsp_data;

    sdram_cdc u_cdc (
        .i_sys_clk         (i_sys_clk),
        .i_sys_rst         (i_sys_rst),
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
        .i_sd_clk          (i_sd_clk),
        .i_sd_rst          (i_sd_rst),
        .o_sd_req_valid    (sd_req_valid),
        .o_sd_req_we       (sd_req_we),
        .o_sd_req_addr     (sd_req_addr),
        .o_sd_req_wdata    (sd_req_wdata),
        .o_sd_req_byte_en  (sd_req_byte_en),
        .i_sd_req_ready    (sd_req_ready),
        .i_sd_rsp_valid    (sd_rsp_valid),
        .i_sd_rsp_data     (sd_rsp_data),
        .o_sd_rsp_ready    (sd_rsp_ready),
        .i_sd_done         (sd_done)
    );

    sdram_ctrl #(
        .ROW_BITS        (SDP.ROW_BITS),
        .COL_BITS        (SDP.COL_BITS),
        .BA_BITS         (SDP.BA_BITS),
        .DQ_BITS         (SDP.DQ_BITS),
        .T_RCD           (SDP.T_RCD),
        .T_RP            (SDP.T_RP),
        .T_RFC           (SDP.T_RFC),
        .T_WR            (SDP.T_WR),
        .T_MRD           (SDP.T_MRD),
        .T_REFI          (SDP.T_REFI),
        .T_POWERUP       (T_POWERUP),
        .CAS_LATENCY     (SDP.CAS_LATENCY),
        .PHY_OUT_LATENCY (PHY_OUT_LATENCY),
        .PHY_IN_LATENCY  (PHY_IN_LATENCY)
    ) u_ctrl (
        .i_clk           (i_sd_clk),
        .i_rst           (i_sd_rst),
        .i_req_valid     (sd_req_valid),
        .i_req_we        (sd_req_we),
        .i_req_addr      (sd_req_addr),
        .i_req_wdata     (sd_req_wdata),
        .i_req_byte_en   (sd_req_byte_en),
        .o_req_ready     (sd_req_ready),
        .o_rsp_valid     (sd_rsp_valid),
        .o_rsp_data      (sd_rsp_data),
        .i_rsp_ready     (sd_rsp_ready),
        .o_done          (sd_done),
        .o_phy_cmd       (o_phy_cmd),
        .o_phy_cke       (o_phy_cke),
        .o_phy_a         (o_phy_a),
        .o_phy_ba        (o_phy_ba),
        .o_phy_dqm       (o_phy_dqm),
        .o_phy_dq_out    (o_phy_dq_out),
        .o_phy_dq_oe     (o_phy_dq_oe),
        .i_phy_dq_in     (i_phy_dq_in),
        .o_dbg_init_done (o_dbg_init_done)
    );

endmodule
