// machine_penumbra2_sim — the gen2 machine with its sim devices attached.
//
// The Verilator wrapper of machine_penumbra2 (the sim-wrapper role in
// doc/internals/build-system.md): the board-independent machine plus the
// devices a program run needs:
//   - unified_bus_mem — ROM region holds the +rom_hex program at 0xFFFF_0000,
//     RAM low, so the vector table at 0x0 and the reset code never alias;
//   - a fixed-address sim UART (NS16450); and
//   - an autoconfig SD/SPI controller (sim_spi behind autoconfig_dev) on the
//     cfg daisy chain the machine's bus controller drives.
// All decode off the same external bus; their IRQs OR into the machine's
// interrupt line. The UART and SPI byte streams are brought to the top so the
// interactive testbench can bridge them to a host terminal and an SD-card
// emulator; the program runners (tb_penumbra2_prog, tb_penumbra2_intr) leave
// those idle and key only on the program-end pulse and commit port.

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

    // UART byte stream (sim_uart's TX/RX), bridged to a host terminal. The
    // program runner leaves the RX inputs unset (idle) and ignores the TX.
    output logic                  o_uart_tx_valid,
    output logic [7:0]            o_uart_tx_data,
    input  logic                  i_uart_rx_valid,
    input  logic [7:0]            i_uart_rx_data,
    output logic                  o_uart_rx_ack,

    // SPI master byte stream (sim_spi behind the autoconfig SD device), bridged
    // to the testbench's SD-card emulator. The program runner leaves the
    // response idle (i_spi_resp_valid=0 → MISO high) and ignores the rest.
    output logic                  o_spi_cmd_valid,
    output logic [7:0]            o_spi_cmd_data,
    input  logic                  i_spi_resp_valid,
    input  logic [7:0]            i_spi_resp_data,
    output logic                  o_spi_cs0,

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

    // External-bus fabric and the combined external IRQ.
    logic [31:0] mem_rdata, uart_rdata;
    logic        mem_busy, uart_busy, uart_irq, mem_claimed;
    logic        uart_sel, uart_sel_r, mem_claimed_r, irq_combined;

    // Bus-controller outputs (autoconfig cfg-enable / reset) from the machine.
    logic        bus_rst, bus_cfg_en;

    // Autoconfig SD/SPI device: autoconfig_dev wrapper + sim_spi master.
    logic [31:0] acfg_rdata, spi_dev_addr, spi_dev_wdata, spi_dev_rdata;
    // byte_en is delivered by autoconfig_dev but sim_spi is byte-oriented and
    // does not consume it.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  spi_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        ac_spi_busy, ac_spi_sel, acfg_sel_r, spi_irq;
    logic        spi_dev_we, spi_dev_re, spi_dev_busy;

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
        .o_bus_rst(bus_rst), .o_bus_cfg_en(bus_cfg_en),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data),
        .o_commit_we(o_commit_we),
        .o_retire_valid(o_retire_valid),
        .o_retire_op_class(o_retire_op_class),
        .o_prog_end(o_prog_end)
    );

    // ── Bus fabric ───────────────────────────────────────────────
    // Each slave owns its address decode: the UART claims a 4 KB page at
    // UART_BASE, the memory claims the RAM/ROM it backs (o_claimed), and the
    // autoconfig device claims config space (while cfg-enabled) then its
    // assigned base. No fabric-side map — what is mapped is discovered at boot,
    // so a no-device access is the absence of any claim (bus-protocol.md).
    localparam int UART_PAGE_SIZE = 4096;

    bus_devsel #(.BASE(UART_BASE), .SIZE(32'(UART_PAGE_SIZE)))
        u_uart_sel (.i_addr(bus_addr), .o_sel(uart_sel));

    // Registered selects align the read-data mux with the slaves' 1-cycle
    // registered read; busy and fault stay combinational (current cycle).
    always_ff @(posedge i_clk) begin
        uart_sel_r    <= uart_sel;
        acfg_sel_r    <= ac_spi_sel;
        mem_claimed_r <= mem_claimed;
    end

    // OR-combine: each slave masks its contribution by its own select, so
    // exactly one drives the bus; an access no slave claims faults.
    assign bus_rdata    = (uart_sel_r    ? uart_rdata : 32'b0)
                        | (acfg_sel_r    ? acfg_rdata : 32'b0)
                        | (mem_claimed_r ? mem_rdata  : 32'b0);
    assign bus_busy     = (uart_sel    ? uart_busy   : 1'b0)
                        | (ac_spi_sel  ? ac_spi_busy : 1'b0)
                        | (mem_claimed ? mem_busy    : 1'b0);
    assign bus_fault    = (bus_re | bus_we) & ~(uart_sel | ac_spi_sel | mem_claimed);
    assign irq_combined = i_irq | uart_irq | spi_irq;

    unified_bus_mem #(
        .REGION_WORDS(MEM_REGION_WORDS), .INIT_FILE(INIT_FILE)
    ) u_mem (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_wdata(bus_wdata), .i_byte_en(bus_byte_en),
        .i_re(bus_re), .i_we(bus_we),
        .o_rdata(mem_rdata), .o_busy(mem_busy), .o_claimed(mem_claimed)
    );

    // Sim UART (NS16450, no FIFO). Its TX/RX byte stream is brought to the top.
    sim_uart u_uart (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_wdata(bus_wdata),
        .i_we(bus_we & uart_sel), .i_re(bus_re & uart_sel),
        .o_rdata(uart_rdata), .o_busy(uart_busy),
        .o_tx_valid(o_uart_tx_valid), .o_tx_data(o_uart_tx_data),
        .i_rx_valid(i_uart_rx_valid), .i_rx_data(i_uart_rx_data),
        .o_rx_ack(o_uart_rx_ack),
        .o_irq(uart_irq)
    );

    // ── Autoconfig SD/SPI controller ─────────────────────────────
    // sim_spi (the SPI master) behind autoconfig_dev (ACFG_CLASS_SD), the sole
    // device on the cfg daisy chain the machine's bus controller drives. The
    // ROM's autoconfig discovers it, assigns it a base, then drives it; its
    // byte stream bridges to the testbench's SD-card emulator. i_cfg_in =
    // bus_cfg_en makes it the first link; o_cfg_out dangles (chain end → the
    // next probe faults → "no more devices").
    /* verilator lint_off PINCONNECTEMPTY */
    autoconfig_dev #(
        .DEV_CLASS(ACFG_CLASS_SD), .DEV_SIZE(32'd4096), .DEV_ID(32'd0),
        .DEV_NAME0(32'h00004453)   // "SD\0\0" packed LE
    ) u_ac_spi (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bus_rst(bus_rst), .i_cfg_en(bus_cfg_en), .i_cfg_in(bus_cfg_en),
        .o_cfg_out(),
        .i_addr(bus_addr), .i_wdata(bus_wdata), .i_byte_en(bus_byte_en),
        .i_we(bus_we), .i_re(bus_re),
        .o_rdata(acfg_rdata), .o_busy(ac_spi_busy), .o_sel(ac_spi_sel),
        .o_dev_addr(spi_dev_addr), .o_dev_wdata(spi_dev_wdata),
        .o_dev_byte_en(spi_dev_byte_en),
        .o_dev_we(spi_dev_we), .o_dev_re(spi_dev_re),
        .i_dev_rdata(spi_dev_rdata), .i_dev_busy(spi_dev_busy)
    );

    sim_spi u_spi (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(spi_dev_addr), .i_wdata(spi_dev_wdata),
        .i_we(spi_dev_we), .i_re(spi_dev_re),
        .o_rdata(spi_dev_rdata), .o_busy(spi_dev_busy),
        .o_cmd_valid(o_spi_cmd_valid), .o_cmd_data(o_spi_cmd_data),
        .i_resp_valid(i_spi_resp_valid), .i_resp_data(i_spi_resp_data),
        .o_cs0(o_spi_cs0), .o_cs1(), .o_irq(spi_irq)
    );
    /* verilator lint_on PINCONNECTEMPTY */

endmodule
