// Unit-test wrapper for penumbra3_bus_master (gen3 Phase-0 probe P0.4).
//
// Wires the gen3 bus master to the real sim SDRAM stack (sdram_sim:
// adapter + CDC + controller + sim PHY + behavioral chip) so the
// master's back-to-back burst runs against the actual speculative-
// prefetch adapter -- the path the gen2 register slice deadlocked on a
// gapped stream. The master's transaction interface is exposed for the
// testbench; the cache-facing bus between master and memory is internal.
module penumbra3_bus_master_test #(
    parameter int LINE_BYTES = 16,
    parameter int WORD_BYTES = 4
) (
    input  logic                                     i_clk,
    input  logic                                     i_sdram_clk,
    input  logic                                     i_rst,

    input  logic                                     i_req_valid,
    output logic                                     o_req_ready,
    input  logic                                     i_req_we,
    input  logic                                     i_req_line,
    input  logic [31:0]                              i_req_addr,
    input  logic [3:0]                               i_req_byte_en,
    input  logic [(LINE_BYTES/WORD_BYTES)-1:0][31:0] i_req_wline,
    output logic                                     o_rsp_valid,
    output logic [(LINE_BYTES/WORD_BYTES)-1:0][31:0] o_rsp_rline,
    output logic                                     o_rsp_fault
);

    // Master <-> sdram_sim cache-facing bus
    logic [31:0] bus_addr;
    logic [31:0] bus_wdata;
    logic [3:0]  bus_byte_en;
    logic        bus_re;
    logic        bus_we;
    logic [31:0] bus_rdata;
    logic        bus_busy;

    penumbra3_bus_master #(
        .LINE_BYTES (LINE_BYTES),
        .WORD_BYTES (WORD_BYTES)
    ) u_dut (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_req_valid   (i_req_valid),
        .o_req_ready   (o_req_ready),
        .i_req_we      (i_req_we),
        .i_req_line    (i_req_line),
        .i_req_addr    (i_req_addr),
        .i_req_byte_en (i_req_byte_en),
        .i_req_wline   (i_req_wline),
        .o_rsp_valid   (o_rsp_valid),
        .o_rsp_rline   (o_rsp_rline),
        .o_rsp_fault   (o_rsp_fault),
        .o_bus_addr    (bus_addr),
        .o_bus_wdata   (bus_wdata),
        .o_bus_byte_en (bus_byte_en),
        .o_bus_re      (bus_re),
        .o_bus_we      (bus_we),
        .i_bus_rdata   (bus_rdata),
        .i_bus_busy    (bus_busy),
        .i_bus_fault   (1'b0)
    );

    sdram_sim u_mem (
        .i_clk       (i_clk),
        .i_sdram_clk (i_sdram_clk),
        .i_rst       (i_rst),
        .i_addr      (bus_addr),
        .i_wdata     (bus_wdata),
        .i_byte_en   (bus_byte_en),
        .i_we        (bus_we),
        .i_re        (bus_re),
        .o_rdata     (bus_rdata),
        .o_busy      (bus_busy)
    );

endmodule
