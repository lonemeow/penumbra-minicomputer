// Sim-only DDR3 memory system: ddr3_stack on a Wishbone line memory.
//
// The Nexys Video's RAM as the machine sees it, with wb_line_mem in the
// DDR3 controller's place: the bus side on the system clock, the memory
// on the controller's. The bus port has sdram_sim's shape, so a machine
// wrapper swaps one for the other at its RAM region; the model backs the
// board's full 512 MiB.
//
// The memory calibrates after its reset before it takes a request, as
// the controller does, and reports it on o_calib_complete in its own
// clock domain. Until then the port stalls, and a bus access would wait
// out the response fabric's watchdog and fault: the machine has to hold
// itself in reset until calibration completes, which is the wrapper's
// job, as it is the board top's. CALIB_CYCLES is long enough that a
// machine which does not would fault its first RAM access.
//
// Two resets, as on the board. i_mem_rst is the controller's own, from
// power-on; nothing else restarts calibration. i_rst is the machine's,
// in the system clock domain, and resets the stack: its controller side
// takes it through a two-flop synchronizer. It does not reach the
// memory, which goes on to answer whatever it had taken.

module ddr3_sim #(
    parameter int CALIB_CYCLES = 4096   // memory clocks
) (
    input  logic        i_clk,          // system / CPU clock
    input  logic        i_rst,          // the machine's reset (i_clk)
    input  logic        i_mem_clk,      // controller clock
    input  logic        i_mem_rst,      // the controller's reset (i_mem_clk)
    output logic        o_calib_complete,

    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack
);
    localparam int ADDR_BITS = 25;      // 512 MiB in 16-byte bursts
    localparam int AUX_WIDTH = 4;

    // The stack's controller-side reset: asserted asynchronously to the
    // controller clock, released on its edge.
    logic mem_rst_sync1, mem_rst_sync2;
    always_ff @(posedge i_mem_clk) begin
        mem_rst_sync1 <= i_rst;
        mem_rst_sync2 <= mem_rst_sync1;
    end

    logic                 wb_stb, wb_we, wb_stall, wb_ack;
    logic [ADDR_BITS-1:0] wb_addr;
    logic [127:0]         wb_wdata, wb_rdata;
    logic [15:0]          wb_sel;
    logic [AUX_WIDTH-1:0] wb_aux_out, wb_aux_in;

    ddr3_stack #(
        .ADDR_BITS  (ADDR_BITS),
        .AUX_WIDTH  (AUX_WIDTH)
    ) u_stack (
        .i_sys_clk  (i_clk),
        .i_sys_rst  (i_rst),
        .i_ctrl_clk (i_mem_clk),
        .i_ctrl_rst (mem_rst_sync2),
        .i_addr     (i_addr),
        .i_wdata    (i_wdata),
        .i_byte_en  (i_byte_en),
        .i_we       (i_we),
        .i_re       (i_re),
        .o_rdata    (o_rdata),
        .o_ack      (o_ack),
        .o_wb_stb   (wb_stb),
        .o_wb_we    (wb_we),
        .o_wb_addr  (wb_addr),
        .o_wb_data  (wb_wdata),
        .o_wb_sel   (wb_sel),
        .o_wb_aux   (wb_aux_out),
        .i_wb_stall (wb_stall),
        .i_wb_ack   (wb_ack),
        .i_wb_data  (wb_rdata),
        .i_wb_aux   (wb_aux_in)
    );

    wb_line_mem #(
        .ADDR_BITS        (ADDR_BITS),
        .AUX_WIDTH        (AUX_WIDTH),
        .CALIB_CYCLES     (CALIB_CYCLES)
    ) u_mem (
        .i_clk            (i_mem_clk),
        .i_rst            (i_mem_rst),
        .i_stb            (wb_stb),
        .i_we             (wb_we),
        .i_addr           (wb_addr),
        .i_data           (wb_wdata),
        .i_sel            (wb_sel),
        .i_aux            (wb_aux_out),
        .o_stall          (wb_stall),
        .o_ack            (wb_ack),
        .o_data           (wb_rdata),
        .o_aux            (wb_aux_in),
        .o_calib_complete (o_calib_complete)
    );

endmodule
