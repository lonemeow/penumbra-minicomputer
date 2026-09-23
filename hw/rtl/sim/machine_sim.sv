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
    // Separate SDRAM clock so the testbench can drive it faster than
    // i_clk to match hardware's 25 MHz CPU / 100 MHz SDRAM ratio.
    // Tie to i_clk for a same-rate single-clock setup.
    input  logic        i_sdram_clk,
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

    // ── USB host controller (device port for the TB's UsbDeviceSim) ──
    // Separate USB clock, like the SDRAM clock: drive faster than i_clk
    // to match hardware's 25 MHz CPU / 60 MHz USB ratio, or tie to
    // i_clk for a same-rate setup.
    input  logic        i_usb_clk,
    output logic [7:0]  o_usb_pkt_data,
    output logic        o_usb_pkt_valid,
    output logic        o_usb_pkt_end,
    output logic        o_usb_keepalive,
    output logic        o_usb_bus_reset,
    output logic        o_usb_resume,
    output logic        o_usb_rx_ready,
    input  logic        i_usb_rx_valid,
    input  logic [7:0]  i_usb_rx_data,
    input  logic        i_usb_rx_last,
    input  logic        i_usb_dev_connect,
    input  logic [1:0]  i_usb_dev_speed,

    // ── Debug / observation ports ────────────────────────────
    output logic [31:0] o_pc,
    output logic        o_halted,
    input  logic [3:0]  i_dbg_reg_addr,
    output logic [31:0] o_dbg_reg_data,

    // ── Instruction trace ────────────────────────────────────
    output logic        o_trace_valid,
    output logic [31:0] o_trace_sr,
    output logic        o_trace_except_entry,
    output logic [3:0]  o_trace_vector,
    output logic        o_trace_eret
);

    // ── CPU ↔ memory bus signals ──────────────────────────
    //
    // Two segments: cpu_mem_* (cpu_core → L2), and mem_* (the
    // shared system bus that all devices decode).  The L2 cache
    // is always instantiated; software opts out by leaving
    // CTRL.enable=0 (the reset state), in which case L2 is a
    // combinational pass-through.
    logic [31:0] cpu_mem_addr, cpu_mem_wdata;
    logic [3:0]  cpu_mem_byte_en;
    logic        cpu_mem_we, cpu_mem_re;
    logic        cpu_mem_cacheable;
    logic [31:0] cpu_mem_rdata;
    logic        cpu_mem_busy;

    logic [31:0] mem_addr, mem_wdata;
    logic [3:0]  mem_byte_en;
    logic        mem_we, mem_re;
    logic [31:0] mem_rdata;
    logic        mem_ack;
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
    assign combined_irq = i_irq | uart_irq | spi_irq | usb_irq;

    // ── Timer tick prescaler (CLK_FREQ → ~1 MHz toggle) ─────
    // Tick freq = CLK_FREQ / (2 * PRESCALE_DIV).  The factor of two
    // comes from edge detection in timer.sv, which fires once per full
    // toggle period.  Derive everything from CLK_FREQ so a clock bump
    // does not silently desync the timer from wall time.
    localparam int CLK_FREQ      = 25_000_000;
    localparam int PRESCALE_DIV  = (CLK_FREQ + 1_000_000) / 2_000_000;
    localparam int TICK_FREQ     = CLK_FREQ / (2 * PRESCALE_DIV);
    logic [$clog2(PRESCALE_DIV)-1:0] prescale_cnt;
    logic timer_tick;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            prescale_cnt <= '0;
            timer_tick   <= 1'b0;
        end else if (prescale_cnt == ($bits(prescale_cnt))'(PRESCALE_DIV - 1)) begin
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

        // Memory bus (cpu side — goes through L2 cache to shared bus)
        .o_mem_addr     (cpu_mem_addr),
        .o_mem_wdata    (cpu_mem_wdata),
        .o_mem_byte_en  (cpu_mem_byte_en),
        .o_mem_we       (cpu_mem_we),
        .o_mem_re       (cpu_mem_re),
        .o_mem_cacheable(cpu_mem_cacheable),
        .i_mem_rdata    (cpu_mem_rdata),
        .i_mem_busy     (cpu_mem_busy),
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
        .o_trace_valid        (o_trace_valid),
        .o_trace_sr           (o_trace_sr),
        .o_trace_except_entry (o_trace_except_entry),
        .o_trace_vector       (o_trace_vector),
        .o_trace_eret         (o_trace_eret)
    );

    // ══════════════════════════════════════════════════════════
    // L2 cache — always instantiated
    //
    // 64 KiB, 4-way, tree-PLRU, 2-cycle hit pipeline,
    // write-invalidate-on-hit.  Disabled at reset; software
    // must explicitly WRSYS SYSDEV_L2_CACHE CTRL=1 to enable.
    // While disabled, every access is a combinational
    // pass-through to the shared bus.
    // ══════════════════════════════════════════════════════════
    logic [31:0] l2_rdata;

    l2_cache u_l2 (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_addr         (cpu_mem_addr),
        .i_wdata        (cpu_mem_wdata),
        .i_byte_en      (cpu_mem_byte_en),
        .i_we           (cpu_mem_we),
        .i_re           (cpu_mem_re),
        .i_cacheable    (cpu_mem_cacheable),
        .o_rdata        (cpu_mem_rdata),
        .o_busy         (cpu_mem_busy),
        // gen1 carries the no-device fault to the core as a sideband, not
        // through L2 — so L2 sees no bus fault and its fault output is unused.
        /* verilator lint_off PINCONNECTEMPTY */
        .o_fault        (),
        /* verilator lint_on PINCONNECTEMPTY */
        .o_mem_addr     (mem_addr),
        .o_mem_wdata    (mem_wdata),
        .o_mem_byte_en  (mem_byte_en),
        .o_mem_we       (mem_we),
        .o_mem_re       (mem_re),
        .i_mem_rdata    (mem_rdata),
        .i_mem_ack      (mem_ack),
        .i_mem_fault    (1'b0),
        .i_sys_reg      (sys_reg),
        .i_sys_wdata    (sys_wdata),
        .i_sys_we       (sys_we & sys_cycle & (sys_dev == SYSDEV_L2_CACHE)),
        .o_sys_rdata    (l2_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Memory bus — device-side address decode
    //
    // Each device decodes its own address range from the shared
    // bus signals (mem_addr, mem_wdata, mem_byte_en, mem_we,
    // mem_re) and responds only when selected.
    //
    // bus_response_fabric merges the responses: the FPGA
    // equivalent of a wired-OR acknowledge line and tri-stated
    // data lines on a discrete backplane. An address no device
    // claims is acknowledged by the fabric with bus_fault.
    // ══════════════════════════════════════════════════════════

    // ── Device parameters ──────────────────────────────────
    localparam int RAM_WORDS      = 8 * 1024 * 1024;  // 32 MB (match ULX3S)
    localparam int ROM_WORDS      = 16384;             // 64 KB
    localparam int UART_PAGE_SIZE = 4096;              // 4 KB

    // ── RAM ─────────────────────────────────────────────────
    logic        ram_sel;
    logic [31:0] ram_rdata_raw;
    logic        ram_busy_raw;

    bus_devsel #(.BASE(RAM_BASE), .SIZE(32'(RAM_WORDS * 4)))
        u_ram_sel (.i_addr(mem_addr), .o_sel(ram_sel));

    // Full v2 SDRAM stack (adapter + controller + sim PHY +
    // behavioral chip).  Drop-in for simple_mem.  See
    // doc/internals/sdram-controller.md.
    sdram_sim u_ram (
        .i_clk       (i_clk),
        .i_sdram_clk (i_sdram_clk),
        .i_rst       (i_rst),
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
    logic [31:0] rom_rdata_raw;
    logic        rom_ack;

    bus_devsel #(.BASE(ROM_BASE), .SIZE(32'(ROM_WORDS * 4)))
        u_rom_sel (.i_addr(mem_addr), .o_sel(rom_sel));

    boot_rom #(.ROM_WORDS(ROM_WORDS)) u_rom (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_addr    (mem_addr),
        .i_re      (mem_re & rom_sel),
        .i_we      (mem_we & rom_sel),
        .o_rdata   (rom_rdata_raw),
        .o_ack     (rom_ack)
    );

    // ── UART ────────────────────────────────────────────────
    logic        uart_sel;
    logic [31:0] uart_rdata_raw;
    logic        uart_ack;

    bus_devsel #(.BASE(UART_BASE), .SIZE(32'(UART_PAGE_SIZE)))
        u_uart_sel (.i_addr(mem_addr), .o_sel(uart_sel));

    sim_uart u_uart (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_addr      (mem_addr),
        .i_wdata     (mem_wdata),
        .i_we        (mem_we & uart_sel),
        .i_re        (mem_re & uart_sel),
        .o_rdata     (uart_rdata_raw),
        .o_ack       (uart_ack),
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

    // ── USB host controller (second in chain) ──────────────
    logic [31:0] usb_dev_addr, usb_dev_wdata;
    logic [3:0]  usb_dev_byte_en;
    logic        usb_dev_we, usb_dev_re;
    logic [31:0] usb_dev_rdata;
    logic        usb_dev_busy;

    logic [31:0] ac_usb_rdata;
    logic        ac_usb_busy, ac_usb_sel;
    logic        ac_usb_cfg_out;
    logic        usb_irq;

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_USBHC),
        .DEV_SIZE  (32'd4096),
        .DEV_ID    (32'd0),
        // "USB\0" packed LE
        .DEV_NAME0 (32'h00425355)
    ) u_ac_usb (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_bus_rst   (busctl_bus_rst),
        .i_cfg_en    (busctl_cfg_en),
        .i_cfg_in    (ac_spi_cfg_out),
        .o_cfg_out   (ac_usb_cfg_out),
        .i_addr      (mem_addr),
        .i_wdata     (mem_wdata),
        .i_byte_en   (mem_byte_en),
        .i_we        (mem_we),
        .i_re        (mem_re),
        .o_rdata     (ac_usb_rdata),
        .o_busy      (ac_usb_busy),
        .o_sel       (ac_usb_sel),
        .o_dev_addr  (usb_dev_addr),
        .o_dev_wdata (usb_dev_wdata),
        .o_dev_byte_en(usb_dev_byte_en),
        .o_dev_we    (usb_dev_we),
        .o_dev_re    (usb_dev_re),
        .i_dev_rdata (usb_dev_rdata),
        .i_dev_busy  (usb_dev_busy)
    );

    // Word-strided device: byte enables carry no information here.
    logic [3:0] unused_usb_byte_en;
    assign unused_usb_byte_en = usb_dev_byte_en;

    // The MAC-PHY seam between the controller and its sim PHY.
    logic [7:0] usb_tx_data;
    logic       usb_tx_valid, usb_tx_ready;
    logic [7:0] usb_rx_data;
    logic       usb_rx_valid, usb_rx_active, usb_rx_error;
    logic [1:0] usb_opmode, usb_xcvr_sel;
    logic       usb_term_sel;
    logic [1:0] usb_line_state;
    logic [2:0] usb_caps;
    logic       usb_phy_clk;
    logic       unused_usb_phy_clk;
    assign unused_usb_phy_clk = usb_phy_clk;

    logic [15:0] usb_phy_dbg;

    usbhc u_usbhc (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_addr       (usb_dev_addr),
        .i_wdata      (usb_dev_wdata),
        .i_we         (usb_dev_we),
        .i_re         (usb_dev_re),
        .o_rdata      (usb_dev_rdata),
        .o_busy       (usb_dev_busy),
        .o_irq        (usb_irq),
        .i_usb_clk    (i_usb_clk),
        .i_usb_rst    (i_rst),
        .o_tx_data    (usb_tx_data),
        .o_tx_valid   (usb_tx_valid),
        .i_tx_ready   (usb_tx_ready),
        .i_rx_data    (usb_rx_data),
        .i_rx_valid   (usb_rx_valid),
        .i_rx_active  (usb_rx_active),
        .i_rx_error   (usb_rx_error),
        .i_line_state (usb_line_state),
        .i_caps       (usb_caps),
        .o_xcvr_sel   (usb_xcvr_sel),
        .o_term_sel   (usb_term_sel),
        .o_opmode     (usb_opmode),
        /* verilator lint_off PINCONNECTEMPTY */
        .o_port_power (),
        /* verilator lint_on PINCONNECTEMPTY */
        .i_dbg        (usb_phy_dbg)
    );

    usb_phy_sim u_usb_phy (
        .i_clk          (i_usb_clk),
        .i_rst          (i_rst),
        .o_clk          (usb_phy_clk),
        .i_tx_data      (usb_tx_data),
        .i_tx_valid     (usb_tx_valid),
        .o_tx_ready     (usb_tx_ready),
        .o_rx_data      (usb_rx_data),
        .o_rx_valid     (usb_rx_valid),
        .o_rx_active    (usb_rx_active),
        .o_rx_error     (usb_rx_error),
        .i_opmode       (usb_opmode),
        .i_xcvr_sel     (usb_xcvr_sel),
        .i_term_sel     (usb_term_sel),
        .o_line_state   (usb_line_state),
        .o_caps         (usb_caps),
        .o_pkt_data     (o_usb_pkt_data),
        .o_pkt_valid    (o_usb_pkt_valid),
        .o_pkt_end      (o_usb_pkt_end),
        .o_keepalive    (o_usb_keepalive),
        .o_bus_reset    (o_usb_bus_reset),
        .o_resume       (o_usb_resume),
        .o_rx_ready     (o_usb_rx_ready),
        .i_rx_valid     (i_usb_rx_valid),
        .i_rx_data      (i_usb_rx_data),
        .i_rx_last      (i_usb_rx_last),
        .i_dev_connect  (i_usb_dev_connect),
        .i_dev_speed    (i_usb_dev_speed),
        .o_dbg          (usb_phy_dbg)
    );

    // ── Autoconfig chain end (after last device) ────────────
    // ac_usb_cfg_out passes through when the USBHC is configured.
    // No more devices after it → bus fault terminates probe.
    logic unused_ac_usb_cfg_out;
    assign unused_ac_usb_cfg_out = ac_usb_cfg_out;

    // ── Bus response fabric ─────────────────────────────────
    // One slot per slave: its select (its own decode), its ack and
    // its read data. RAM and the autoconfig devices report completion
    // as a busy-drop, so their acks are the slave's held strobe on a
    // busy-low cycle.
    localparam int SLV_RAM  = 0;
    localparam int SLV_ROM  = 1;
    localparam int SLV_UART = 2;
    localparam int SLV_SPI  = 3;
    localparam int SLV_USB  = 4;
    localparam int NUM_SLV  = 5;

    logic                     mem_strobe;
    logic [NUM_SLV-1:0]       slv_sel;
    logic [NUM_SLV-1:0]       slv_ack;
    logic [NUM_SLV-1:0][31:0] slv_rdata;

    assign mem_strobe = mem_re | mem_we;

    assign slv_sel[SLV_RAM]    = ram_sel;
    assign slv_ack[SLV_RAM]    = mem_strobe & ram_sel & ~ram_busy_raw;
    assign slv_rdata[SLV_RAM]  = ram_rdata_raw;

    assign slv_sel[SLV_ROM]    = rom_sel;
    assign slv_ack[SLV_ROM]    = rom_ack;
    assign slv_rdata[SLV_ROM]  = rom_rdata_raw;

    assign slv_sel[SLV_UART]   = uart_sel;
    assign slv_ack[SLV_UART]   = uart_ack;
    assign slv_rdata[SLV_UART] = uart_rdata_raw;

    assign slv_sel[SLV_SPI]    = ac_spi_sel;
    assign slv_ack[SLV_SPI]    = mem_strobe & ac_spi_sel & ~ac_spi_busy;
    assign slv_rdata[SLV_SPI]  = ac_spi_rdata;

    assign slv_sel[SLV_USB]    = ac_usb_sel;
    assign slv_ack[SLV_USB]    = mem_strobe & ac_usb_sel & ~ac_usb_busy;
    assign slv_rdata[SLV_USB]  = ac_usb_rdata;

    bus_response_fabric #(
        .NUM_SLAVES (NUM_SLV)
    ) u_bus_fabric (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_re    (mem_re),
        .i_we    (mem_we),
        .i_sel   (slv_sel),
        .i_ack   (slv_ack),
        .i_rdata (slv_rdata),
        .o_ack   (mem_ack),
        .o_rdata (mem_rdata),
        .o_fault (bus_fault)
    );

    // ══════════════════════════════════════════════════════════
    // Sysreg devices (external, dev_id >= 1)
    // ══════════════════════════════════════════════════════════

    // ── Machine identity (device 8, read-only) ───────────────
    // SYSDEV_CPU (device 1) lives inside cpu_core (CPU identity + perfctrs).
    logic [31:0] machid_rdata;

    // Machine name: "Simulator"
    machid #(
        .MACH_NAME0 (32'h756D6953),   // "Simu"
        .MACH_NAME1 (32'h6F74616C),   // "lato"
        .MACH_NAME2 (32'h00000072),   // "r\0\0\0"
        .CPU_FREQ   (CLK_FREQ)
    ) u_machid (
        .i_sys_reg  (sys_reg),
        .o_sys_rdata(machid_rdata)
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
        .TICK_FREQ_HZ (TICK_FREQ)
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
            SYSDEV_BUS:   sys_rdata = busctl_rdata;
            SYSDEV_TIMER: sys_rdata = timer_rdata;
            SYSDEV_MACH:  sys_rdata = machid_rdata;
            SYSDEV_L2_CACHE: sys_rdata = l2_rdata;
            default:      sys_rdata = 32'b0;
        endcase
    end

endmodule
// verilator lint_on UNUSEDSIGNAL
