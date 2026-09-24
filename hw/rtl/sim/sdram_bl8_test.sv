// SDRAM controller unit-test wrapper at an eight-beat read burst.
//
// The same chain sdram_test builds, elaborated for the burst length a
// line-shaped read uses: one command returns four words instead of
// one.  Verilator takes a top module rather than a parameter set, so
// the configuration under test is named by its own module; the wiring
// itself lives once, in sdram_test.
//
// Driven by tb_sdram_bl8_test.cpp, which tests what the burst adds --
// ordering across the four words, and alignment from any word within
// the group -- rather than repeating the single-word suite.

module sdram_bl8_test (
    input  logic         i_clk,
    input  logic         i_rst,

    input  logic         i_req_valid,
    input  logic         i_req_we,
    input  logic [31:0]  i_req_addr,
    input  logic [31:0]  i_req_wdata,
    input  logic [3:0]   i_req_byte_en,
    output logic         o_req_ready,

    output logic         o_rsp_valid,
    output logic [127:0] o_rsp_data,
    input  logic         i_rsp_ready,

    output logic         o_init_done,
    output logic         o_done,
    output logic [31:0]  o_protocol_errors
);

    sdram_test #(
        .BURST_LEN (8)
    ) u_chain (
        .i_clk             (i_clk),
        .i_rst             (i_rst),
        .i_req_valid       (i_req_valid),
        .i_req_we          (i_req_we),
        .i_req_addr        (i_req_addr),
        .i_req_wdata       (i_req_wdata),
        .i_req_byte_en     (i_req_byte_en),
        .o_req_ready       (o_req_ready),
        .o_rsp_valid       (o_rsp_valid),
        .o_rsp_data        (o_rsp_data),
        .i_rsp_ready       (i_rsp_ready),
        .o_init_done       (o_init_done),
        .o_done            (o_done),
        .o_protocol_errors (o_protocol_errors)
    );

endmodule
