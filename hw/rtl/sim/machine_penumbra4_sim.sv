// machine_penumbra4_sim — the gen4 machine with its sim devices attached.
//
// The Verilator wrapper of machine_penumbra4 (the sim-wrapper role in
// doc/internals/build-system.md), a fork of machine_penumbra3_sim that
// differs in the machine and its observability ports: the gen4 machine
// keeps the gen3 one's bus, interrupt and bus-control ports, so the
// devices attach to it the same way:
//   - boot_rom — the ROM region at 0xFFFF_0000 holds the +rom_hex program;
//   - sdram_sim — the full SDRAM stack (adapter, CDC, controller, sim PHY
//     and behavioral chip) backs the RAM region low, on i_sdram_clk, which
//     the testbench drives faster than i_clk;
//   - a fixed-address sim UART (NS16450); and
//   - an autoconfig SD/SPI controller (sim_spi behind autoconfig_dev) on the
//     cfg daisy chain the machine's bus controller drives.
// All decode off the same external bus; their IRQs OR into the machine's
// interrupt line. The UART and SPI byte streams are brought to the top so an
// interactive testbench can bridge them to a host terminal and an SD-card
// emulator; the program runner leaves those idle and keys only on the
// program-end pulse and the commit ports, which also name the ops as they
// commit, retiring or trapping, for a runner's failure report and the
// console's trace.

module machine_penumbra4_sim
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
#(
    parameter int          ROM_WORDS = 16384,            // 64 KiB boot ROM
    // 32 MiB SDRAM region, the ULX3S part the sim models.
    parameter int          RAM_WORDS = 8 * 1024 * 1024
)(
    input  logic                 i_clk,
    input  logic                 i_sdram_clk,
    input  logic                 i_rst,
    input  logic                 i_irq,
    input  logic                 i_timer_irq,

    // UART byte stream (sim_uart's TX/RX), bridged to a host terminal.
    output logic                 o_uart_tx_valid,
    output logic [7:0]           o_uart_tx_data,
    input  logic                 i_uart_rx_valid,
    input  logic [7:0]           i_uart_rx_data,
    output logic                 o_uart_rx_ack,

    // SPI master byte stream (sim_spi behind the autoconfig SD device),
    // bridged to the testbench's SD-card emulator. An idle response
    // (i_spi_resp_valid low) reads as MISO high.
    output logic                 o_spi_cmd_valid,
    output logic [7:0]           o_spi_cmd_data,
    input  logic                 i_spi_resp_valid,
    input  logic [7:0]           i_spi_resp_data,
    output logic                 o_spi_cs0,

    output logic       [1:0]     o_commit_valid,
    output vaddr_t     [1:0]     o_commit_pc,
    output op_class_e  [1:0]     o_commit_op_class,
    output logic       [1:0]     o_commit_trap,
    output vector_t    [1:0]     o_commit_vector,
    output slot_e                o_commit_older,
    output sr_t                  o_sr,
    output logic       [1:0]     o_commit_we,
    output phys_reg_t  [1:0]     o_commit_phys,
    output word_t      [1:0]     o_commit_data,
    output logic       [1:0]     o_retire_count,
    output logic                 o_prog_end
);

    paddr_t      bus_addr;
    word_t       bus_wdata, bus_rdata;
    logic [3:0]  bus_byte_en;
    logic        bus_re, bus_we, bus_ack, bus_fault;

    word_t       ram_rdata, rom_rdata, uart_rdata;
    logic        ram_ack, rom_ack, uart_ack, uart_irq;
    logic        ram_sel, rom_sel, uart_sel;
    logic        irq_combined;

    logic        bus_rst, bus_cfg_en;

    paddr_t      spi_dev_addr;
    word_t       acfg_rdata, spi_dev_wdata, spi_dev_rdata;
    // sim_spi is byte-oriented and takes no byte lanes.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  spi_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        ac_spi_ack, ac_spi_sel, spi_irq;
    logic        spi_dev_we, spi_dev_re, spi_dev_ack;

    // Sim machine identity: name "Simulator", 25 MHz, which matches the
    // runners' clock and what isa/test_machid checks.
    machine_penumbra4 #(
        .MACH_NAME0 (32'h756D6953),   // "Simu"
        .MACH_NAME1 (32'h6F74616C),   // "lato"
        .MACH_NAME2 (32'h00000072),   // "r\0\0\0"
        .CPU_FREQ   (32'd25_000_000)
    ) u_machine (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_irq          (irq_combined),
        .i_timer_irq    (i_timer_irq),
        .o_bus_addr     (bus_addr),
        .o_bus_wdata    (bus_wdata),
        .o_bus_byte_en  (bus_byte_en),
        .o_bus_re       (bus_re),
        .o_bus_we       (bus_we),
        .i_bus_rdata    (bus_rdata),
        .i_bus_ack      (bus_ack),
        .i_bus_fault    (bus_fault),
        .o_bus_rst      (bus_rst),
        .o_bus_cfg_en   (bus_cfg_en),

        .o_commit_valid     (o_commit_valid),
        .o_commit_pc        (o_commit_pc),
        .o_commit_op_class  (o_commit_op_class),
        .o_commit_trap      (o_commit_trap),
        .o_commit_vector    (o_commit_vector),
        .o_commit_older     (o_commit_older),
        .o_sr               (o_sr),
        .o_commit_we        (o_commit_we),
        .o_commit_phys      (o_commit_phys),
        .o_commit_data      (o_commit_data),
        .o_retire_count     (o_retire_count),
        .o_prog_end         (o_prog_end)
    );

    // ── Bus fabric ───────────────────────────────────────────────
    // Each slave owns its address decode: the SDRAM-backed RAM region at
    // RAM_BASE, the boot ROM at ROM_BASE, the UART's 4 KB page at UART_BASE,
    // and the autoconfig device (config space while cfg-enabled, then its
    // assigned base). What is mapped is discovered at boot, so a no-device
    // access is one nothing acknowledges, and the fabric's watchdog faults
    // it (bus-protocol.md).
    localparam int UART_PAGE_SIZE = 4096;

    bus_devsel #(
        .BASE (RAM_BASE),
        .SIZE (32'(RAM_WORDS * 4))
    ) u_ram_sel (
        .i_addr (bus_addr),
        .o_sel  (ram_sel)
    );

    bus_devsel #(
        .BASE (ROM_BASE),
        .SIZE (32'(ROM_WORDS * 4))
    ) u_rom_sel (
        .i_addr (bus_addr),
        .o_sel  (rom_sel)
    );

    bus_devsel #(
        .BASE (UART_BASE),
        .SIZE (32'(UART_PAGE_SIZE))
    ) u_uart_sel (
        .i_addr (bus_addr),
        .o_sel  (uart_sel)
    );

    assign irq_combined = i_irq | uart_irq | spi_irq;

    // ── Bus response fabric ──────────────────────────────────────
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

    // ── Devices ──────────────────────────────────────────────────
    sdram_sim u_ram (
        .i_clk       (i_clk),
        .i_sdram_clk (i_sdram_clk),
        .i_rst       (i_rst),
        .i_addr      (bus_addr),
        .i_wdata     (bus_wdata),
        .i_byte_en   (bus_byte_en),
        .i_we        (bus_we & ram_sel),
        .i_re        (bus_re & ram_sel),
        .o_rdata     (ram_rdata),
        .o_ack       (ram_ack)
    );

    boot_rom #(
        .ROM_WORDS (ROM_WORDS)
    ) u_rom (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_addr  (bus_addr),
        .i_re    (bus_re & rom_sel),
        .i_we    (bus_we & rom_sel),
        .o_rdata (rom_rdata),
        .o_ack   (rom_ack)
    );

    sim_uart u_uart (
        .i_clk      (i_clk),
        .i_rst      (i_rst),
        .i_addr     (bus_addr),
        .i_wdata    (bus_wdata),
        .i_we       (bus_we & uart_sel),
        .i_re       (bus_re & uart_sel),
        .o_rdata    (uart_rdata),
        .o_ack      (uart_ack),
        .o_tx_valid (o_uart_tx_valid),
        .o_tx_data  (o_uart_tx_data),
        .i_rx_valid (i_uart_rx_valid),
        .i_rx_data  (i_uart_rx_data),
        .o_rx_ack   (o_uart_rx_ack),
        .o_irq      (uart_irq)
    );

    // The sole device on the cfg daisy chain: i_cfg_in = bus_cfg_en makes it
    // the first link, and o_cfg_out ends the chain, so the next probe faults
    // and the ROM's autoconfig finds no more devices.
    /* verilator lint_off PINCONNECTEMPTY */
    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_SD),
        .DEV_SIZE  (32'd4096),
        .DEV_ID    (32'd0),
        .DEV_NAME0 (32'h00004453)   // "SD\0\0" packed LE
    ) u_ac_spi (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_bus_rst     (bus_rst),
        .i_cfg_en      (bus_cfg_en),
        .i_cfg_in      (bus_cfg_en),
        .o_cfg_out     (),
        .i_addr        (bus_addr),
        .i_wdata       (bus_wdata),
        .i_byte_en     (bus_byte_en),
        .i_we          (bus_we),
        .i_re          (bus_re),
        .o_rdata       (acfg_rdata),
        .o_ack         (ac_spi_ack),
        .o_sel         (ac_spi_sel),
        .o_dev_addr    (spi_dev_addr),
        .o_dev_wdata   (spi_dev_wdata),
        .o_dev_byte_en (spi_dev_byte_en),
        .o_dev_we      (spi_dev_we),
        .o_dev_re      (spi_dev_re),
        .i_dev_rdata   (spi_dev_rdata),
        .i_dev_ack     (spi_dev_ack)
    );

    sim_spi u_spi (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_addr       (spi_dev_addr),
        .i_wdata      (spi_dev_wdata),
        .i_we         (spi_dev_we),
        .i_re         (spi_dev_re),
        .o_rdata      (spi_dev_rdata),
        .o_ack        (spi_dev_ack),
        .o_cmd_valid  (o_spi_cmd_valid),
        .o_cmd_data   (o_spi_cmd_data),
        .i_resp_valid (i_spi_resp_valid),
        .i_resp_data  (i_spi_resp_data),
        .o_cs0        (o_spi_cs0),
        .o_cs1        (),
        .o_irq        (spi_irq)
    );
    /* verilator lint_on PINCONNECTEMPTY */

endmodule
