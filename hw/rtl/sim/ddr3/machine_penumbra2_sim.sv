// machine_penumbra2_sim — the gen2 machine on the Nexys Video's DDR3 memory.
//
// A fork of hw/rtl/sim/machine_penumbra2_sim.sv that differs only in its
// RAM, which the build puts first on its path when it simulates the
// DDR3 memory system (SIM_MEM=ddr3 in the Makefile). The Verilator
// wrapper of machine_penumbra2 (the sim-wrapper role in
// doc/internals/build-system.md): the board-independent machine plus the
// devices a program run needs:
//   - boot_rom — the ROM region at 0xFFFF_0000 holds the +rom_hex program;
//   - ddr3_sim — the Nexys Video's memory stack (line adapter + CDC + the
//     DDR3 controller's Wishbone port) on a line-memory model standing in
//     for the controller, backing all 512 MiB of the RAM region low. It
//     runs on i_sdram_clk, driven faster than i_clk, and calibrates before
//     it takes a request, so the machine is held in reset until it has;
//   - a fixed-address sim UART (NS16450); and
//   - an autoconfig SD/SPI controller (sim_spi behind autoconfig_dev) on the
//     cfg daisy chain the machine's bus controller drives.
// All decode off the same external bus; their IRQs OR into the machine's
// interrupt line. The UART and SPI byte streams are brought to the top so the
// interactive testbench can bridge them to a host terminal and an SD-card
// emulator; the program runners (tb_penumbra2_prog, tb_penumbra2_intr) leave
// those idle and key only on the program-end pulse and commit port.

`ifndef PENUMBRA_CPU_VARIANT
`define PENUMBRA_CPU_VARIANT 0    // 0 = gen2 baseline, 1 = gen2.5
`endif

module machine_penumbra2_sim
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
#(
    parameter logic [31:0] RESET_PC  = 32'hFFFF_0000,
    parameter int          ROM_WORDS = 16384,            // 64 KiB boot ROM
    // 512 MiB DDR3 region, the Nexys Video's part.
    parameter int          RAM_WORDS = 128 * 1024 * 1024
)(
    input  logic                  i_clk,
    // The DDR3 controller's clock, under the SDRAM clock's name so the gen2
    // testbenches drive it unchanged. Any ratio to i_clk is valid: the stack
    // crosses between them asynchronously.
    input  logic                  i_sdram_clk,
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
    output logic [31:0]           o_retire_pc,       // retiring instruction's PC (trace)
    output logic [31:0]           o_retire_sr,       // its architectural SR (trace)
    output logic                  o_fault_commit,    // a fault is taken this cycle (trace marker)
    output logic [3:0]            o_fault_vec,       // its vector number
    output logic                  o_eret_commit,     // an ERET is committing (trace marker)
    output logic                  o_dc_commit,       // a drain-commit (EI/DI/WRSYS/ERET) retires (trace)
    output logic [31:0]           o_dc_commit_pc,    // its PC
    output logic [OPC_W-1:0]      o_dc_commit_op_class, // its op_class
    output logic                  o_branch_taken,    // EX branch resolve (trace)
    output logic [31:0]           o_branch_target,   // its target
    output logic [31:0]           o_branch_pc,       // the branch's own PC
    output logic [31:0]           o_ex_pc,           // pipeline occupancy: EX slot (trace)
    output logic                  o_ex_valid,
    output logic [31:0]           o_mem_pc,          // MEM slot
    output logic                  o_mem_valid,
    output logic                  o_prog_end
);

    logic [31:0] bus_addr, bus_wdata, bus_rdata;
    logic [3:0]  bus_byte_en;
    logic        bus_re, bus_we, bus_ack, bus_fault;

    // Per-slave read data / busy and address selects. The RAM region is backed
    // by the DDR3 memory system (ddr3_sim) and the ROM region by boot_rom, so
    // — unlike the old single compressed memory — each owns its own decode.
    logic [31:0] ram_rdata, rom_rdata, uart_rdata;
    logic        ram_ack, rom_ack, uart_ack, uart_irq;
    logic        ram_sel, rom_sel, uart_sel;
    logic        irq_combined;

    // Bus-controller outputs (autoconfig cfg-enable / reset) from the machine.
    logic        bus_rst, bus_cfg_en;

    // Autoconfig SD/SPI device: autoconfig_dev wrapper + sim_spi master.
    logic [31:0] acfg_rdata, spi_dev_addr, spi_dev_wdata, spi_dev_rdata;
    // byte_en is delivered by autoconfig_dev but sim_spi is byte-oriented and
    // does not consume it.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  spi_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        ac_spi_ack, ac_spi_sel, spi_irq;
    logic        spi_dev_we, spi_dev_re, spi_dev_ack;

    // ── Resets ───────────────────────────────────────────────────
    // The memory controller's reset is its own, from power-on, and the
    // machine's never reaches it. In simulation the power-on is the
    // testbench's reset, which a run asserts once at its start.
    logic mem_por_sync1, mem_por_sync2;
    always_ff @(posedge i_sdram_clk) begin
        mem_por_sync1 <= i_rst;
        mem_por_sync2 <= mem_por_sync1;
    end

    // The machine's reset: the testbench's, held on until the memory has
    // calibrated (mem_calibrated, in the i_sdram_clk domain).
    logic mem_calibrated;

    logic mem_calibrated_sync1, mem_calibrated_sync2;
    always_ff @(posedge i_clk) begin
        mem_calibrated_sync1 <= mem_calibrated;
        mem_calibrated_sync2 <= mem_calibrated_sync1;
    end

    logic cpu_rst;
    assign cpu_rst = i_rst || !mem_calibrated_sync2;

    machine_penumbra2 #(
        .RESET_PC(RESET_PC),
        .CPU_VARIANT(`PENUMBRA_CPU_VARIANT),
        // Sim machine identity: name "Simulator", 25 MHz — matches the
        // runners' clock and what isa/test_machid checks.
        .MACH_NAME0(32'h756D6953),   // "Simu"
        .MACH_NAME1(32'h6F74616C),   // "lato"
        .MACH_NAME2(32'h00000072),   // "r\0\0\0"
        .CPU_FREQ  (32'd25_000_000)
    ) u_machine (
        .i_clk(i_clk), .i_rst(cpu_rst),
        .i_irq(irq_combined), .i_timer_irq(i_timer_irq),
        .o_bus_addr(bus_addr), .o_bus_wdata(bus_wdata),
        .o_bus_byte_en(bus_byte_en),
        .o_bus_re(bus_re), .o_bus_we(bus_we),
        .i_bus_rdata(bus_rdata), .i_bus_ack(bus_ack),
        .i_bus_fault(bus_fault),
        .o_bus_rst(bus_rst), .o_bus_cfg_en(bus_cfg_en),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data),
        .o_commit_we(o_commit_we),
        .o_retire_valid(o_retire_valid),
        .o_retire_op_class(o_retire_op_class),
        .o_retire_pc(o_retire_pc), .o_retire_sr(o_retire_sr),
        .o_fault_commit(o_fault_commit), .o_fault_vec(o_fault_vec),
        .o_eret_commit(o_eret_commit),
        .o_dc_commit(o_dc_commit), .o_dc_commit_pc(o_dc_commit_pc),
        .o_dc_commit_op_class(o_dc_commit_op_class),
        .o_branch_taken(o_branch_taken), .o_branch_target(o_branch_target),
        .o_branch_pc(o_branch_pc),
        .o_ex_pc(o_ex_pc), .o_ex_valid(o_ex_valid),
        .o_mem_pc(o_mem_pc), .o_mem_valid(o_mem_valid),
        .o_prog_end(o_prog_end)
    );

    // ── Bus fabric ───────────────────────────────────────────────
    // Each slave owns its address decode: the SDRAM-backed RAM region at
    // RAM_BASE, the boot ROM at ROM_BASE, the UART's 4 KB page at UART_BASE,
    // and the autoconfig device (config space while cfg-enabled, then its
    // assigned base). No fabric-side map — what is mapped is discovered at
    // boot, so a no-device access is one nothing acknowledges, and the
    // fabric's watchdog faults it (bus-protocol.md).
    localparam int UART_PAGE_SIZE = 4096;

    bus_devsel #(.BASE(RAM_BASE),  .SIZE(32'(RAM_WORDS * 4)))
        u_ram_sel  (.i_addr(bus_addr), .o_sel(ram_sel));
    bus_devsel #(.BASE(ROM_BASE),  .SIZE(32'(ROM_WORDS * 4)))
        u_rom_sel  (.i_addr(bus_addr), .o_sel(rom_sel));
    bus_devsel #(.BASE(UART_BASE), .SIZE(32'(UART_PAGE_SIZE)))
        u_uart_sel (.i_addr(bus_addr), .o_sel(uart_sel));

    assign irq_combined = i_irq | uart_irq | spi_irq;

    // ── Bus response fabric ─────────────────────────────────
    // One slot per slave: its select (its own decode), its ack and
    // its read data.
    localparam int SLV_RAM  = 0;
    localparam int SLV_ROM  = 1;
    localparam int SLV_UART = 2;
    localparam int SLV_SPI  = 3;
    localparam int NUM_SLV  = 4;

    logic [NUM_SLV-1:0]       slv_sel;
    logic [NUM_SLV-1:0]       slv_ack;
    logic [NUM_SLV-1:0][31:0] slv_rdata;

    assign slv_sel[SLV_RAM]    = ram_sel;
    assign slv_ack[SLV_RAM]    = ram_ack;
    assign slv_rdata[SLV_RAM]  = ram_rdata;

    assign slv_sel[SLV_ROM]    = rom_sel;
    assign slv_ack[SLV_ROM]    = rom_ack;
    assign slv_rdata[SLV_ROM]  = rom_rdata;

    assign slv_sel[SLV_UART]   = uart_sel;
    assign slv_ack[SLV_UART]   = uart_ack;
    assign slv_rdata[SLV_UART] = uart_rdata;

    assign slv_sel[SLV_SPI]    = ac_spi_sel;
    assign slv_ack[SLV_SPI]    = ac_spi_ack;
    assign slv_rdata[SLV_SPI]  = acfg_rdata;

    bus_response_fabric #(
        .NUM_SLAVES (NUM_SLV)
    ) u_bus_fabric (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_re    (bus_re),
        .i_we    (bus_we),
        .i_sel   (slv_sel),
        .i_ack   (slv_ack),
        .i_rdata (slv_rdata),
        .o_ack   (bus_ack),
        .o_rdata (bus_rdata),
        .o_fault (bus_fault)
    );

    // ── RAM: the DDR3 memory stack on a line-memory model of the
    // controller. The stack resets with the machine; calibration does not.
    ddr3_sim u_ram (
        .i_clk(i_clk), .i_rst(cpu_rst),
        .i_mem_clk(i_sdram_clk), .i_mem_rst(mem_por_sync2),
        .o_calib_complete(mem_calibrated),
        .i_addr(bus_addr), .i_wdata(bus_wdata), .i_byte_en(bus_byte_en),
        .i_we(bus_we & ram_sel), .i_re(bus_re & ram_sel),
        .o_rdata(ram_rdata), .o_ack(ram_ack)
    );

    // ── Boot ROM: holds the +rom_hex program at ROM_BASE (0xFFFF_0000).
    boot_rom #(.ROM_WORDS(ROM_WORDS)) u_rom (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_re(bus_re & rom_sel), .i_we(bus_we & rom_sel),
        .o_rdata(rom_rdata), .o_ack(rom_ack)
    );

    // Sim UART (NS16450, no FIFO). Its TX/RX byte stream is brought to the top.
    sim_uart u_uart (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(bus_addr), .i_wdata(bus_wdata),
        .i_we(bus_we & uart_sel), .i_re(bus_re & uart_sel),
        .o_rdata(uart_rdata), .o_ack(uart_ack),
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
        .o_rdata(acfg_rdata), .o_ack(ac_spi_ack), .o_sel(ac_spi_sel),
        .o_dev_addr(spi_dev_addr), .o_dev_wdata(spi_dev_wdata),
        .o_dev_byte_en(spi_dev_byte_en),
        .o_dev_we(spi_dev_we), .o_dev_re(spi_dev_re),
        .i_dev_rdata(spi_dev_rdata), .i_dev_ack(spi_dev_ack)
    );

    sim_spi u_spi (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(spi_dev_addr), .i_wdata(spi_dev_wdata),
        .i_we(spi_dev_we), .i_re(spi_dev_re),
        .o_rdata(spi_dev_rdata), .o_ack(spi_dev_ack),
        .o_cmd_valid(o_spi_cmd_valid), .o_cmd_data(o_spi_cmd_data),
        .i_resp_valid(i_spi_resp_valid), .i_resp_data(i_spi_resp_data),
        .o_cs0(o_spi_cs0), .o_cs1(), .o_irq(spi_irq)
    );
    /* verilator lint_on PINCONNECTEMPTY */

endmodule
