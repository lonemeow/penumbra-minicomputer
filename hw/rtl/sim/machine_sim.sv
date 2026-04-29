// Penumbra Simulation Machine — cpu_core + RAM + boot ROM + UART + sysreg devices
//
// Integration top for Verilator simulation. Wires the CPU core to
// a shared memory bus with device-side address decoding: each device
// recognizes its own address range and responds only when selected.
// Responses are OR-combined (FPGA equivalent of tri-state bus).
//
// This models how the real Penumbra Bus works — each device has its
// own address comparator (DIP switches on ISA, slot decode on GIO,
// or 74x85 comparators on Penumbra). The bus carries no decode
// logic; it is electrically passive.
//
// Adding a device: add a decode/attach block, instantiate the
// device, and OR its masked response into the bus response lines.
//
// Address map (device-side decode):
//   0x0000_0000 – (RAM_WORDS*4-1)  RAM (simple_mem, parameterized)
//   0xFF00_0000 – 0xFF00_0FFF      UART (sim_uart, 4 KB page)
//   0xFFFF_0000 – 0xFFFF_FFFF      Boot ROM (boot_rom, 64 KB)
//   everything else                 unmapped (bus fault)
//
// CPU boots from ROM at 0xFFFF_0000 (default RESET_PC).

// verilator lint_off UNUSEDSIGNAL
module machine_sim
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── External interrupt (directly from testbench) ────────
    input  logic        i_irq,

    // ── UART external signals (directly accessible by TB) ───
    output logic        o_uart_tx_valid,
    output logic [7:0]  o_uart_tx_data,
    input  logic        i_uart_rx_valid,
    input  logic [7:0]  i_uart_rx_data,
    output logic        o_uart_rx_ack,

    // ── SPI signals (directly accessible by TB for SD card) ──
    output logic        o_spi_cmd_valid,
    output logic [7:0]  o_spi_cmd_data,
    input  logic        i_spi_resp_valid,
    input  logic [7:0]  i_spi_resp_data,
    output logic        o_spi_cs0,

    // ── Debug / observation ports ────────────────────────────
    output logic [31:0] o_pc,
    output logic        o_halted,
    input  logic [3:0]  i_dbg_reg_addr,
    output logic [31:0] o_dbg_reg_data,

    // ── Instruction trace ────────────────────────────────────
    output logic        o_trace_valid,
    output logic [31:0] o_trace_sr
);

    // ── CPU ↔ memory bus (shared, directly from CPU) ────────
    logic [31:0] mem_addr, mem_wdata;
    logic [3:0]  mem_byte_en;
    logic        mem_we, mem_re;
    logic [31:0] mem_rdata;
    logic        mem_busy;
    logic        bus_fault;

    // ── CPU ↔ sysreg bus ────────────────────────────────────
    logic [3:0]  sys_dev, sys_reg;
    logic [31:0] sys_wdata;
    logic        sys_cycle, sys_we;
    logic [31:0] sys_rdata;

    // ── Internal IRQ (UART + SPI + external testbench IRQ) ──
    logic uart_irq;
    logic spi_irq;
    logic combined_irq;
    assign combined_irq = i_irq | uart_irq | spi_irq;

    // ── Timer tick prescaler (CPU clock → 1 MHz) ────────────
    // Generates a toggle signal at 1 MHz from the CPU clock.
    // The timer module synchronises and edge-detects internally.
    localparam int PRESCALE_DIV = 25;   // 25 MHz / 25 = 1 MHz
    logic [$clog2(PRESCALE_DIV)-1:0] prescale_cnt;
    logic timer_tick;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            prescale_cnt <= '0;
            timer_tick   <= 1'b0;
        end else if (prescale_cnt == PRESCALE_DIV[4:0] - 5'd1) begin
            prescale_cnt <= '0;
            timer_tick   <= ~timer_tick;  // Toggle for edge detection
        end else begin
            prescale_cnt <= prescale_cnt + 1;
        end
    end

    // ── Timer IRQ ───────────────────────────────────────────
    logic timer_irq;

    // ══════════════════════════════════════════════════════════
    // CPU Core (default RESET_PC = 0xFFFF_0000)
    // ══════════════════════════════════════════════════════════
    cpu_core u_cpu (
        .i_clk          (i_clk),
        .i_rst          (i_rst),

        // Memory bus (shared — all devices see these signals)
        .o_mem_addr     (mem_addr),
        .o_mem_wdata    (mem_wdata),
        .o_mem_byte_en  (mem_byte_en),
        .o_mem_we       (mem_we),
        .o_mem_re       (mem_re),
        .i_mem_rdata    (mem_rdata),
        .i_mem_busy     (mem_busy),
        .i_bus_fault    (bus_fault),

        // Sysreg bus (external devices)
        .o_sys_dev      (sys_dev),
        .o_sys_reg      (sys_reg),
        .o_sys_wdata    (sys_wdata),
        .o_sys_cycle    (sys_cycle),
        .o_sys_we       (sys_we),
        .i_sys_rdata    (sys_rdata),

        // Interrupts
        .i_timer_irq    (timer_irq),
        .i_irq          (combined_irq),

        // Debug
        .o_pc           (o_pc),
        .o_halted       (o_halted),
        .i_dbg_reg_addr (i_dbg_reg_addr),
        .o_dbg_reg_data (o_dbg_reg_data),

        // Trace
        .o_trace_valid  (o_trace_valid),
        .o_trace_sr     (o_trace_sr)
    );

    // ══════════════════════════════════════════════════════════
    // Memory bus — device-side address decode
    //
    // Each device decodes its own address range from the shared
    // bus signals (mem_addr, mem_wdata, mem_byte_en, mem_we,
    // mem_re) and responds only when selected. Devices that are
    // not selected output zero on rdata and deassert busy.
    //
    // Responses are OR-combined: the FPGA equivalent of
    // tri-state or open-collector on a discrete backplane bus.
    // Only the addressed device drives non-zero data.
    //
    // Unmapped addresses: no device claims them, so the OR
    // yields rdata=0 and busy=0. The bus_fault signal detects
    // this condition for future exception handling.
    // ══════════════════════════════════════════════════════════

    // ── Device parameters ──────────────────────────────────
    localparam int RAM_WORDS      = 4 * 1024 * 1024;  // 16 MB
    localparam int ROM_WORDS      = 16384;             // 64 KB
    localparam int UART_PAGE_SIZE = 4096;              // 4 KB

    // ── RAM ─────────────────────────────────────────────────
    logic        ram_sel;
    logic        ram_sel_r;
    logic [31:0] ram_rdata_raw;
    logic        ram_busy_raw;

    bus_devsel #(.BASE(RAM_BASE), .SIZE(32'(RAM_WORDS * 4)))
        u_ram_sel (.i_addr(mem_addr), .o_sel(ram_sel));
    always_ff @(posedge i_clk) ram_sel_r <= ram_sel;

    // Full v2 SDRAM stack (adapter + controller + sim PHY +
    // behavioral chip).  Drop-in for simple_mem.  See
    // doc/internals/sdram-controller.md.
    sdram_sim u_ram (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_addr    (mem_addr),
        .i_wdata   (mem_wdata),
        .i_byte_en (mem_byte_en),
        .i_we      (mem_we & ram_sel),
        .i_re      (mem_re & ram_sel),
        .o_rdata   (ram_rdata_raw),
        .o_busy    (ram_busy_raw)
    );

    // ── Boot ROM ────────────────────────────────────────────
    logic        rom_sel;
    logic        rom_sel_r;
    logic [31:0] rom_rdata_raw;
    logic        rom_busy_raw;

    bus_devsel #(.BASE(ROM_BASE), .SIZE(32'(ROM_WORDS * 4)))
        u_rom_sel (.i_addr(mem_addr), .o_sel(rom_sel));
    always_ff @(posedge i_clk) rom_sel_r <= rom_sel;

    boot_rom #(.ROM_WORDS(ROM_WORDS)) u_rom (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_addr    (mem_addr),
        .i_re      (mem_re & rom_sel),
        .o_rdata   (rom_rdata_raw),
        .o_busy    (rom_busy_raw)
    );

    // ── UART ────────────────────────────────────────────────
    logic        uart_sel;
    logic        uart_sel_r;
    logic [31:0] uart_rdata_raw;
    logic        uart_busy_raw;

    bus_devsel #(.BASE(UART_BASE), .SIZE(32'(UART_PAGE_SIZE)))
        u_uart_sel (.i_addr(mem_addr), .o_sel(uart_sel));
    always_ff @(posedge i_clk) uart_sel_r <= uart_sel;

    sim_uart u_uart (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_addr      (mem_addr),
        .i_wdata     (mem_wdata),
        .i_we        (mem_we & uart_sel),
        .i_re        (mem_re & uart_sel),
        .o_rdata     (uart_rdata_raw),
        .o_busy      (uart_busy_raw),
        .o_tx_valid  (o_uart_tx_valid),
        .o_tx_data   (o_uart_tx_data),
        .i_rx_valid  (i_uart_rx_valid),
        .i_rx_data   (i_uart_rx_data),
        .o_rx_ack    (o_uart_rx_ack),
        .o_irq       (uart_irq)
    );

    // ══════════════════════════════════════════════════════════
    // Autoconfig device chain
    //
    // The cfg daisy chain starts at busctl_cfg_en and passes
    // through each autoconfigured device. Each device's cfg_out
    // feeds the next device's cfg_in. Bus fault at chain end
    // signals "no more devices" to the ROM autoconfig loop.
    // ══════════════════════════════════════════════════════════

    // ── SPI controller (first device in chain) ──────────────
    logic [31:0] spi_dev_addr, spi_dev_wdata;
    logic [3:0]  spi_dev_byte_en;
    logic        spi_dev_we, spi_dev_re;
    logic [31:0] spi_dev_rdata;
    logic        spi_dev_busy;

    logic [31:0] ac_spi_rdata;
    logic        ac_spi_busy, ac_spi_sel;
    logic        ac_spi_cfg_out;

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_SD),
        .DEV_SIZE  (32'd4096),
        .DEV_ID    (32'd0),
        // "SD\0\0" packed LE
        .DEV_NAME0 (32'h00004453)
    ) u_ac_spi (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_bus_rst   (busctl_bus_rst),
        .i_cfg_en    (busctl_cfg_en),
        .i_cfg_in    (busctl_cfg_en),      // first in chain
        .o_cfg_out   (ac_spi_cfg_out),
        .i_addr      (mem_addr),
        .i_wdata     (mem_wdata),
        .i_byte_en   (mem_byte_en),
        .i_we        (mem_we),
        .i_re        (mem_re),
        .o_rdata     (ac_spi_rdata),
        .o_busy      (ac_spi_busy),
        .o_sel       (ac_spi_sel),
        .o_dev_addr  (spi_dev_addr),
        .o_dev_wdata (spi_dev_wdata),
        .o_dev_byte_en(spi_dev_byte_en),
        .o_dev_we    (spi_dev_we),
        .o_dev_re    (spi_dev_re),
        .i_dev_rdata (spi_dev_rdata),
        .i_dev_busy  (spi_dev_busy)
    );

    /* verilator lint_off PINCONNECTEMPTY */
    sim_spi u_spi (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_addr      (spi_dev_addr),
        .i_wdata     (spi_dev_wdata),
        .i_we        (spi_dev_we),
        .i_re        (spi_dev_re),
        .o_rdata     (spi_dev_rdata),
        .o_busy      (spi_dev_busy),
        .o_cmd_valid (o_spi_cmd_valid),
        .o_cmd_data  (o_spi_cmd_data),
        .i_resp_valid(i_spi_resp_valid),
        .i_resp_data (i_spi_resp_data),
        .o_cs0       (o_spi_cs0),
        .o_cs1       (),
        .o_irq       (spi_irq)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // ── Autoconfig chain end (after last device) ────────────
    // ac_spi_cfg_out passes through when SPI is configured.
    // No more devices after SPI → bus fault terminates probe.

    // Autoconfig device bus signals (OR'd into bus response)
    logic [31:0] acfg_rdata;
    logic        acfg_busy;
    logic        acfg_sel;
    logic        acfg_sel_r;
    assign acfg_rdata = ac_spi_rdata;
    assign acfg_busy  = ac_spi_busy;
    assign acfg_sel   = ac_spi_sel;
    always_ff @(posedge i_clk) acfg_sel_r <= acfg_sel;

    // ── Bus response OR-combine ─────────────────────────────
    // Read data: masked by registered select (one-cycle delay
    // aligns with synchronous slave read latency). Only the
    // selected device contributes; all others masked to zero.
    //
    // Discrete equivalent: tri-state buffers on each device's
    // data output, active-low OE driven by the device's select.
    assign mem_rdata = (ram_sel_r  ? ram_rdata_raw  : 32'b0) |
                       (rom_sel_r  ? rom_rdata_raw  : 32'b0) |
                       (uart_sel_r ? uart_rdata_raw : 32'b0) |
                       (acfg_sel_r ? acfg_rdata     : 32'b0);

    // Busy: combinational (current cycle) so the CPU stalls
    // immediately when the addressed device needs time.
    assign mem_busy = (ram_sel  ? ram_busy_raw  : 1'b0) |
                      (rom_sel  ? rom_busy_raw  : 1'b0) |
                      (uart_sel ? uart_busy_raw : 1'b0) |
                      acfg_busy;

    // ── Bus fault detection ─────────────────────────────────
    // Active request with no device claiming the address.
    // Real hardware: detected via timeout (no ACK within deadline).
    // Simulation: combinational — wired to cpu_core as exception.
    assign bus_fault = (mem_re | mem_we) & ~(ram_sel | rom_sel | uart_sel | acfg_sel);

    // ══════════════════════════════════════════════════════════
    // Sysreg devices (external, dev_id >= 1)
    // ══════════════════════════════════════════════════════════

    // ── System ID (device 1, read-only) ──────────────────────
    logic [31:0] sysid_rdata;

    // Machine name: "Simulator"
    sysid #(
        .MACH_NAME0 (32'h756D6953),   // "Simu"
        .MACH_NAME1 (32'h6F74616C),   // "lato"
        .MACH_NAME2 (32'h00000072),   // "r\0\0\0"
        .CPU_FREQ   (32'd25_000_000)  // Simulated at 25 MHz
    ) u_sysid (
        .i_sys_reg  (sys_reg),
        .o_sys_rdata(sysid_rdata)
    );

    // ── Bus Controller (device 4) ──────────────────────────
    logic [31:0] busctl_rdata;
    logic        busctl_bus_rst;
    logic        busctl_cfg_en;

    busctl u_busctl (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_sys_reg   (sys_reg),
        .i_sys_wdata (sys_wdata),
        .i_sys_we    (sys_we & sys_cycle & (sys_dev == SYSDEV_BUS)),
        .o_sys_rdata (busctl_rdata),
        .o_bus_rst   (busctl_bus_rst),
        .o_cfg_en    (busctl_cfg_en)
    );

    // ── Timer (device 7) ────────────────────────────────────
    logic [31:0] timer_rdata;

    timer #(
        .TICK_FREQ_HZ (32'd1_000_000)
    ) u_timer (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_tick      (timer_tick),
        .i_sys_reg   (sys_reg),
        .i_sys_wdata (sys_wdata),
        .i_sys_we    (sys_we & sys_cycle & (sys_dev == SYSDEV_TIMER)),
        .o_sys_rdata (timer_rdata),
        .o_irq       (timer_irq)
    );

    // ── Sysreg read mux ─────────────────────────────────────
    // Routes read data from external devices back to cpu_core.
    // Device 0 (MMU) is handled inside cpu_core.
    always_comb begin
        case (sys_dev)
            SYSDEV_SYS:   sys_rdata = sysid_rdata;
            SYSDEV_BUS:   sys_rdata = busctl_rdata;
            SYSDEV_TIMER: sys_rdata = timer_rdata;
            default:      sys_rdata = 32'b0;
        endcase
    end

endmodule
// verilator lint_on UNUSEDSIGNAL
