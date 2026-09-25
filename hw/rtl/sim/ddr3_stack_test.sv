// Test wrapper: ddr3_stack on a Wishbone burst memory model.
//
// The stack as the Nexys machine uses it, its Wishbone port served by
// wb_line_mem in place of the DDR3 controller: the bus side on the
// system clock, the memory on the controller's. The memory has a reset
// of its own, apart from the stack's controller-side one, as the machine
// resets its stack without recalibrating the DDR3 controller: a stack
// reset reaches the memory only as the dropped bus cycle.

module ddr3_stack_test (
    input  logic        i_sys_clk,
    input  logic        i_sys_rst,
    input  logic        i_ctrl_clk,
    input  logic        i_ctrl_rst,
    input  logic        i_mem_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack,
    // The memory port, watched by the testbench.
    output logic        o_wb_cyc,
    output logic        o_wb_stb,
    output logic        o_wb_stall
);
    localparam int ADDR_BITS = 25;
    localparam int AUX_WIDTH = 4;

    logic                 wb_we, wb_ack;
    logic [ADDR_BITS-1:0] wb_addr;
    logic [127:0]         wb_wdata, wb_rdata;
    logic [15:0]          wb_sel;
    logic [AUX_WIDTH-1:0] wb_aux_out, wb_aux_in;

    ddr3_stack #(
        .ADDR_BITS  (ADDR_BITS),
        .AUX_WIDTH  (AUX_WIDTH)
    ) u_stack (
        .i_sys_clk  (i_sys_clk),
        .i_sys_rst  (i_sys_rst),
        .i_ctrl_clk (i_ctrl_clk),
        .i_ctrl_rst (i_ctrl_rst),
        .i_addr     (i_addr),
        .i_wdata    (i_wdata),
        .i_byte_en  (i_byte_en),
        .i_we       (i_we),
        .i_re       (i_re),
        .o_rdata    (o_rdata),
        .o_ack      (o_ack),
        .o_wb_cyc   (o_wb_cyc),
        .o_wb_stb   (o_wb_stb),
        .o_wb_we    (wb_we),
        .o_wb_addr  (wb_addr),
        .o_wb_data  (wb_wdata),
        .o_wb_sel   (wb_sel),
        .o_wb_aux   (wb_aux_out),
        .i_wb_stall (o_wb_stall),
        .i_wb_ack   (wb_ack),
        .i_wb_data  (wb_rdata),
        .i_wb_aux   (wb_aux_in)
    );

    wb_line_mem #(
        .ADDR_BITS (ADDR_BITS),
        .AUX_WIDTH (AUX_WIDTH)
    ) u_mem (
        .i_clk     (i_ctrl_clk),
        .i_rst     (i_mem_rst),
        .i_cyc     (o_wb_cyc),
        .i_stb     (o_wb_stb),
        .i_we      (wb_we),
        .i_addr    (wb_addr),
        .i_data    (wb_wdata),
        .i_sel     (wb_sel),
        .i_aux     (wb_aux_out),
        .o_stall   (o_wb_stall),
        .o_ack     (wb_ack),
        .o_data    (wb_rdata),
        .o_aux     (wb_aux_in)
    );

endmodule
