// Test wrapper for autoconfig_dev — wraps a tiny echo memory
//
// Instantiates autoconfig_dev around a 16-word (64-byte) simple_mem.
// Used by tb_autoconfig.cpp to test the config chain, address
// latching, and bus forwarding.

// verilator lint_off UNUSEDSIGNAL

module autoconfig_test
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Bus controller signals ─────────────────────────────
    input  logic        i_bus_rst,
    input  logic        i_cfg_en,

    // ── Memory bus ─────────────────────────────────────────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,

    // ── Responses (directly from autoconfig wrappers) ──────
    output logic [31:0] o_rdata,
    output logic        o_busy,
    output logic        o_sel,

    // ── For testbench observation ──────────────────────────
    output logic        o_dev0_cfg_out,
    output logic        o_dev1_cfg_out
);

    // ── Device 0: "SPI" (CLASS_SPI, 4 KB) ──────────────────
    logic [31:0] dev0_addr, dev0_wdata;
    logic [3:0]  dev0_byte_en;
    logic        dev0_we, dev0_re;
    logic [31:0] dev0_rdata;
    logic        dev0_busy;
    logic [31:0] ac0_rdata;
    logic        ac0_busy, ac0_sel;

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_SPI),
        .DEV_SIZE  (32'd4096),
        .DEV_ID    (32'd0),
        // "SPI\0" = 0x00495053
        .DEV_NAME0 (32'h00495053)
    ) u_ac0 (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_bus_rst   (i_bus_rst),
        .i_cfg_en    (i_cfg_en),
        .i_cfg_in    (i_cfg_en),       // first in chain
        .o_cfg_out   (o_dev0_cfg_out),
        .i_addr      (i_addr),
        .i_wdata     (i_wdata),
        .i_byte_en   (i_byte_en),
        .i_we        (i_we),
        .i_re        (i_re),
        .o_rdata     (ac0_rdata),
        .o_busy      (ac0_busy),
        .o_sel       (ac0_sel),
        .o_dev_addr  (dev0_addr),
        .o_dev_wdata (dev0_wdata),
        .o_dev_byte_en(dev0_byte_en),
        .o_dev_we    (dev0_we),
        .o_dev_re    (dev0_re),
        .i_dev_rdata (dev0_rdata),
        .i_dev_busy  (dev0_busy)
    );

    simple_mem #(.MEM_WORDS(16)) u_mem0 (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_addr    (dev0_addr),
        .i_wdata   (dev0_wdata),
        .i_byte_en (dev0_byte_en),
        .i_we      (dev0_we),
        .i_re      (dev0_re),
        .o_rdata   (dev0_rdata),
        .o_busy    (dev0_busy)
    );

    // ── Device 1: "Exp. RAM" (CLASS_MEMORY, 64 bytes) ──────
    logic [31:0] dev1_addr, dev1_wdata;
    logic [3:0]  dev1_byte_en;
    logic        dev1_we, dev1_re;
    logic [31:0] dev1_rdata;
    logic        dev1_busy;
    logic [31:0] ac1_rdata;
    logic        ac1_busy, ac1_sel;

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_MEMORY),
        .DEV_SIZE  (32'd64),
        .DEV_ID    (32'd0),
        // "RAM\0" = 0x004D4152
        .DEV_NAME0 (32'h004D4152)
    ) u_ac1 (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_bus_rst   (i_bus_rst),
        .i_cfg_en    (i_cfg_en),
        .i_cfg_in    (o_dev0_cfg_out), // second in chain
        .o_cfg_out   (o_dev1_cfg_out),
        .i_addr      (i_addr),
        .i_wdata     (i_wdata),
        .i_byte_en   (i_byte_en),
        .i_we        (i_we),
        .i_re        (i_re),
        .o_rdata     (ac1_rdata),
        .o_busy      (ac1_busy),
        .o_sel       (ac1_sel),
        .o_dev_addr  (dev1_addr),
        .o_dev_wdata (dev1_wdata),
        .o_dev_byte_en(dev1_byte_en),
        .o_dev_we    (dev1_we),
        .o_dev_re    (dev1_re),
        .i_dev_rdata (dev1_rdata),
        .i_dev_busy  (dev1_busy)
    );

    simple_mem #(.MEM_WORDS(16)) u_mem1 (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_addr    (dev1_addr),
        .i_wdata   (dev1_wdata),
        .i_byte_en (dev1_byte_en),
        .i_we      (dev1_we),
        .i_re      (dev1_re),
        .o_rdata   (dev1_rdata),
        .o_busy    (dev1_busy)
    );

    // ── OR-combine responses ───────────────────────────────
    assign o_rdata = ac0_rdata | ac1_rdata;
    assign o_busy  = ac0_busy  | ac1_busy;
    assign o_sel   = ac0_sel   | ac1_sel;

endmodule
