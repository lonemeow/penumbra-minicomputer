// machine_penumbra2_sim — the gen2 machine with its sim memory attached.
//
// The Verilator wrapper of machine_penumbra2 (the sim-wrapper role in
// doc/internals/build-system.md): the board-independent machine plus the
// devices a program run needs — today just unified_bus_mem on the external
// bus (ROM region holds the +rom_hex program at 0xFFFF_0000, RAM low, so
// the vector table at 0x0 and the reset code never alias). The program
// runners (tb_penumbra2_prog, tb_penumbra2_intr) drive clock/reset/IRQs and
// key on the machine's program-end pulse and commit port — the runner
// contract — so they carry no knowledge of what is inside.

module machine_penumbra2_sim
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
#(
    parameter logic [31:0] RESET_PC         = 32'hFFFF_0000,
    parameter int          MEM_REGION_WORDS = 4096,
    parameter string       INIT_FILE        = "program.hex"
)(
    input  logic                  i_clk,
    input  logic                  i_rst,
    input  logic                  i_irq,
    input  logic                  i_timer_irq,

    output logic [SB_IDX_W-1:0]   o_commit_idx,
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we,
    output logic                  o_retire_valid,
    output logic [OPC_W-1:0]      o_retire_op_class,
    output logic                  o_prog_end
);

    logic [31:0] bus_addr, bus_wdata, bus_rdata;
    logic [3:0]  bus_byte_en;
    logic        bus_re, bus_we, bus_busy;

    machine_penumbra2 #(.RESET_PC(RESET_PC)) u_machine (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_irq(i_irq), .i_timer_irq(i_timer_irq),
        .o_bus_addr(bus_addr), .o_bus_wdata(bus_wdata),
        .o_bus_byte_en(bus_byte_en),
        .o_bus_re(bus_re), .o_bus_we(bus_we),
        .i_bus_rdata(bus_rdata), .i_bus_busy(bus_busy),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data),
        .o_commit_we(o_commit_we),
        .o_retire_valid(o_retire_valid),
        .o_retire_op_class(o_retire_op_class),
        .o_prog_end(o_prog_end)
    );

    unified_bus_mem #(
        .REGION_WORDS(MEM_REGION_WORDS), .INIT_FILE(INIT_FILE)
    ) u_mem (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_wdata(bus_wdata), .i_byte_en(bus_byte_en),
        .i_re(bus_re), .i_we(bus_we),
        .o_rdata(bus_rdata), .o_busy(bus_busy)
    );

endmodule
