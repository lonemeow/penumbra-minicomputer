// machine_penumbra2_sim — the gen2 machine with its sim memory attached.
//
// The Verilator wrapper of machine_penumbra2 (the sim-wrapper role in
// doc/internals/build-system.md): the board-independent machine plus the
// devices a program run needs: unified_bus_mem (ROM region holds the +rom_hex
// program at 0xFFFF_0000, RAM low, so the vector table at 0x0 and the reset
// code never alias) plus a fixed-address sim UART decoded off the same bus,
// its IRQ ORed into the machine's external interrupt line. The program
// runners (tb_penumbra2_prog, tb_penumbra2_intr) drive clock/reset/IRQs and
// key on the machine's program-end pulse and commit port — the runner
// contract — so they carry no knowledge of what is inside.

module machine_penumbra2_sim
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
#(
    parameter logic [31:0] RESET_PC         = 32'hFFFF_0000,
    // Sim RAM/ROM region size. Big enough that conformance programs' data and
    // stacks fit without an unbacked (faulting) access — the widest gen2 test
    // walks four pages up to 0x5000. The bus-fault tests pick 0x0200_0000 as
    // their "no device" address precisely because it is far above this.
    parameter int          MEM_REGION_WORDS = 16384,   // 64 KiB per region
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
    logic        bus_re, bus_we, bus_busy, bus_fault;

    // External-bus fabric (memory + UART) and the combined external IRQ.
    logic [31:0] mem_rdata, uart_rdata;
    logic        mem_busy, uart_busy, uart_irq, mem_claimed;
    logic        uart_sel, uart_sel_r, irq_combined;

    machine_penumbra2 #(
        .RESET_PC(RESET_PC),
        // Sim machine identity: name "Simulator", 25 MHz — matches the
        // runners' clock and what isa/test_machid checks.
        .MACH_NAME0(32'h756D6953),   // "Simu"
        .MACH_NAME1(32'h6F74616C),   // "lato"
        .MACH_NAME2(32'h00000072),   // "r\0\0\0"
        .CPU_FREQ  (32'd25_000_000)
    ) u_machine (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_irq(irq_combined), .i_timer_irq(i_timer_irq),
        .o_bus_addr(bus_addr), .o_bus_wdata(bus_wdata),
        .o_bus_byte_en(bus_byte_en),
        .o_bus_re(bus_re), .o_bus_we(bus_we),
        .i_bus_rdata(bus_rdata), .i_bus_busy(bus_busy),
        .i_bus_fault(bus_fault),
        // Bus autoconfig control: no autoconfig chain on this wrapper yet, so
        // busctl's outputs are observed nowhere (the conformance suite never
        // drives SYSDEV_BUS). Left open until the chain + a discoverable
        // device are wired here.
        /* verilator lint_off PINCONNECTEMPTY */
        .o_bus_rst(), .o_bus_cfg_en(),
        /* verilator lint_on PINCONNECTEMPTY */
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data),
        .o_commit_we(o_commit_we),
        .o_retire_valid(o_retire_valid),
        .o_retire_op_class(o_retire_op_class),
        .o_prog_end(o_prog_end)
    );

    // ── Bus fabric: memory + a fixed-address UART ────────────────
    // Each slave owns its own address decode: the UART claims a 4 KB page at
    // UART_BASE (its register window), the memory claims the RAM/ROM addresses
    // it actually backs (o_claimed, from its own size). No fabric-side address
    // map — on the external async bus what is mapped is discovered at boot, so
    // a no-device access is detected as the *absence of any slave's claim*
    // (the canonical no-device fault — see bus-protocol.md), not by a master
    // that pretends to know the layout. busy follows the live select; read
    // data follows the registered select (the registered-read contract). The
    // UART IRQ joins the machine's external i_irq line.
    localparam int UART_PAGE_SIZE = 4096;

    bus_devsel #(.BASE(UART_BASE), .SIZE(32'(UART_PAGE_SIZE)))
        u_uart_sel (.i_addr(bus_addr), .o_sel(uart_sel));
    always_ff @(posedge i_clk) uart_sel_r <= uart_sel;

    assign bus_busy     = uart_sel   ? uart_busy  : mem_busy;
    assign bus_rdata    = uart_sel_r ? uart_rdata : mem_rdata;
    assign bus_fault    = (bus_re | bus_we) & ~(uart_sel | mem_claimed);
    assign irq_combined = i_irq | uart_irq;

    unified_bus_mem #(
        .REGION_WORDS(MEM_REGION_WORDS), .INIT_FILE(INIT_FILE)
    ) u_mem (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_wdata(bus_wdata), .i_byte_en(bus_byte_en),
        .i_re(bus_re), .i_we(bus_we),
        .o_rdata(mem_rdata), .o_busy(mem_busy), .o_claimed(mem_claimed)
    );

    // Sim UART (NS16450, no FIFO). TX/RX are observe-only here — the
    // conformance program only exercises the register file and TX-busy timing
    // — so the TX outputs and RX-ack are open and RX is idle.
    /* verilator lint_off PINCONNECTEMPTY */
    sim_uart u_uart (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_wdata(bus_wdata),
        .i_we(bus_we & uart_sel), .i_re(bus_re & uart_sel),
        .o_rdata(uart_rdata), .o_busy(uart_busy),
        .o_tx_valid(), .o_tx_data(),
        .i_rx_valid(1'b0), .i_rx_data(8'b0), .o_rx_ack(),
        .o_irq(uart_irq)
    );
    /* verilator lint_on PINCONNECTEMPTY */

endmodule
