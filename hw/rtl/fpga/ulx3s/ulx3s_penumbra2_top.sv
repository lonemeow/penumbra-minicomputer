// ULX3S Board Top (Penumbra/2) — gen2 CPU system on ECP5-85F
//
// Wires machine_penumbra2 (core + MMU + split VIPT L1s + I/D arbiter +
// fill sequencer + L2 + sysreg complex) to the board: 32 MB SDRAM via the
// v2 controller stack, a 64 KB boot ROM, a real NS16450 UART on the FTDI
// serial port, and an SPI/SD card behind the autoconfig daisy chain. The
// CPU runs at 25 MHz; the SDRAM runs decoupled at 100 MHz across the CDC.
//
// The machine owns the programmable timer and the bus controller
// internally, so the board attaches only the memory/peripheral devices on
// the external Penumbra Bus and routes the autoconfig chain off the
// machine's BUSCTL outputs (o_bus_rst / o_bus_cfg_en). Device-side address
// decode, the read-data OR-combine, and the no-device bus fault are
// assembled here — the same bus shape machine_penumbra2_sim presents, so
// what simulates is what synthesizes.
//
// For other boards, copy this file and adjust pins/clocking/RAM.

// ── SDRAM clock-out phase shift (CLKOS2) ──────────────────────────
// The SDRAM-clock pin is forwarded via ODDRX1F clocked from CLKOS2, which
// is phase-shifted relative to the controller clock so the SDRAM samples
// our drives near the centre of the data window. The phase value is
// empirical — different ULX3S boards / SDRAM variants see slightly
// different working windows. The build-time sweep procedure
// (`make fpga PHASE_DEG=N BOARD=ulx3s CORE=penumbra2` for each N in
// 0,45,90,…,315) finds the contiguous arc that boots and passes the ROM
// RAM check; pick its centre. See doc/internals/sdram-controller.md.
`ifndef SDRAM_PHASE_DEG
`define SDRAM_PHASE_DEG 180
`endif

module ulx3s_penumbra2_top (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    // Only btn[1] (FIRE1 = manual reset) is used; other bits are
    // physical inputs the design doesn't consume.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [6:0] btn,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic       ftdi_rxd,    // FPGA TX → FTDI RX → host
    input  logic       ftdi_txd,    // host → FTDI TX → FPGA RX
    output logic       wifi_en,     // LOW = hold ESP32 in reset

    // ── SD card (SPI mode) ─────────────────────────────────
    // Uses sd_clk, sd_cmd, sd_d[0] (MISO), sd_d[3] (CS).
    // sd_d[1:2] driven high per SD SPI spec.
    output logic       sd_clk,      // SPI SCK
    output logic       sd_cmd,      // SPI MOSI
    inout  wire  [3:0] sd_d,        // [0]=MISO in, [3]=CS out, [2:1]=high

    // ── SDRAM ──────────────────────────────────────────────
    output logic        sdram_clk,
    output logic        sdram_cke,
    output logic        sdram_csn,
    output logic        sdram_wen,
    output logic        sdram_rasn,
    output logic        sdram_casn,
    output logic [12:0] sdram_a,
    output logic [1:0]  sdram_ba,
    inout  wire  [15:0] sdram_d,
    output logic [1:0]  sdram_dqm
);
    import penumbra_pkg::*;
    import sdram_pkg::*;

    // ── Board constants ──────────────────────────────────────────
    localparam int CLK_FREQ = 25_000_000;   // Hz (derived from PLL below)

    // ── SDRAM chip preset (one-line preset swap) ─────────────────
    localparam sdram_params_t SDP = W9825_100;

    // ── ESP32 disable ──────────────────────────────────────────
    assign wifi_en = 1'b0;

    // ── PLL: 25 MHz → 25 MHz (CPU) + 2× 100 MHz (SDRAM) ──────────
    // fCLKOP  = 25 MHz   — CPU / system bus
    // fCLKOS  = 100 MHz, 0°               — SDRAM controller fabric
    // fCLKOS2 = 100 MHz, SDRAM_PHASE_DEG° — SDRAM pin clock (ODDR forward)
    // Phase convention and per-board sweep results live in
    // doc/internals/sdram-controller.md.
    localparam int CLKOS2_PHASE_DEG = `SDRAM_PHASE_DEG;
    localparam int CLKOS2_CPHASE_VAL =
        (CLKOS2_PHASE_DEG ==   0) ? 5 :
        (CLKOS2_PHASE_DEG ==  45) ? 4 :
        (CLKOS2_PHASE_DEG ==  90) ? 3 :
        (CLKOS2_PHASE_DEG == 135) ? 2 :
        (CLKOS2_PHASE_DEG == 180) ? 2 :
        (CLKOS2_PHASE_DEG == 225) ? 1 :
        (CLKOS2_PHASE_DEG == 270) ? 0 :
        (CLKOS2_PHASE_DEG == 315) ? 5 : 0;
    localparam int CLKOS2_FPHASE_VAL =
        (CLKOS2_PHASE_DEG ==   0) ? 0 :
        (CLKOS2_PHASE_DEG ==  45) ? 2 :
        (CLKOS2_PHASE_DEG ==  90) ? 4 :
        (CLKOS2_PHASE_DEG == 135) ? 6 :
        (CLKOS2_PHASE_DEG == 180) ? 0 :
        (CLKOS2_PHASE_DEG == 225) ? 2 :
        (CLKOS2_PHASE_DEG == 270) ? 4 :
        (CLKOS2_PHASE_DEG == 315) ? 6 : 0;

    logic clk;            // CLKOP — 25 MHz system clock
    logic clk_sdram;      // CLKOS — 100 MHz SDRAM fabric clock
    logic clk_sdram_pin;  // CLKOS2 — 100 MHz, phase-shifted, to ODDR
    logic pll_lock;

    (* keep *) EHXPLLL #(
        .CLKI_DIV      (1),
        .CLKFB_DIV     (1),
        .CLKOP_DIV     (24),
        .CLKOP_ENABLE  ("ENABLED"),
        .CLKOP_CPHASE  (23),
        .CLKOP_FPHASE  (0),
        .CLKOS_DIV     (6),
        .CLKOS_ENABLE  ("ENABLED"),
        .CLKOS_CPHASE  (5),                  // 0° phase
        .CLKOS_FPHASE  (0),
        .CLKOS2_DIV    (6),
        .CLKOS2_ENABLE ("ENABLED"),
        .CLKOS2_CPHASE (CLKOS2_CPHASE_VAL),  // SDRAM_PHASE_DEG° phase
        .CLKOS2_FPHASE (CLKOS2_FPHASE_VAL),
        .FEEDBK_PATH   ("CLKOP")
    ) u_pll (
        .CLKI         (clk_25mhz),
        .CLKFB        (clk),
        .CLKOP        (clk),
        .CLKOS        (clk_sdram),
        .CLKOS2       (clk_sdram_pin),
        .CLKOS3       (),
        .LOCK         (pll_lock),
        .RST          (1'b0),
        .STDBY        (1'b0),
        .PHASESEL0    (1'b0),
        .PHASESEL1    (1'b0),
        .PHASEDIR     (1'b0),
        .PHASESTEP    (1'b0),
        .PHASELOADREG (1'b0),
        .PLLWAKESYNC  (1'b0),
        .ENCLKOP      (1'b1),
        .ENCLKOS      (1'b1),
        .ENCLKOS2     (1'b1),
        .ENCLKOS3     (1'b0)
    );

    // ── Reset: PLL lock + btn[1] (FIRE1) manual reset ──────────
    // Hold reset until PLL locks, then count 2^19 clocks (~21 ms at
    // 25 MHz). Pressing btn[1] reasserts reset (input synchronizer).
    logic btn1_sync1, btn1_sync2;
    always_ff @(posedge clk) begin
        btn1_sync1 <= btn[1];
        btn1_sync2 <= btn1_sync1;
    end

    /* verilator lint_off PROCASSINIT */
    logic [18:0] rst_cnt = '0;
    /* verilator lint_on PROCASSINIT */
    logic        rst_raw;
    logic        rst;

    always_ff @(posedge clk) begin
        if (!pll_lock || btn1_sync2)
            rst_cnt <= '0;
        else if (!rst_cnt[18])
            rst_cnt <= rst_cnt + 1;
    end
    assign rst_raw = !rst_cnt[18];

    // Promote rst onto a global net via DCCA: its high fanout makes it a
    // dominant routing-congestion source through general fabric. CE=1
    // keeps the buffer always-on; (* keep *) stops yosys folding it away
    // on the grounds that CLKO == CLKI — the DCCA is what makes PnR route
    // the output via global net rather than the input.
    (* keep *) DCCA rst_dcca (
        .CLKI (rst_raw),
        .CE   (1'b1),
        .CLKO (rst)
    );

    // ── SDRAM-domain reset: 2-FF synchronizer of `rst` into clk_sdram ──
    // Asynchronous-assert / synchronous-deassert across the boundary so
    // sdram_ctrl / sdram_phy_ecp5 / sdram_cdc(sdram side) leave reset
    // together on a clean SDRAM-clock edge.
    logic rst_sd_sync1, rst_sd_sync2;
    always_ff @(posedge clk_sdram) begin
        rst_sd_sync1 <= rst;
        rst_sd_sync2 <= rst_sd_sync1;
    end
    wire rst_sd = rst_sd_sync2;

    // ── Heartbeat / debug LEDs ─────────────────────────────────
    logic [24:0] hb_cnt;
    always_ff @(posedge clk) begin
        if (rst) hb_cnt <= '0;
        else     hb_cnt <= hb_cnt + 1;
    end

    assign led[0]   = hb_cnt[24]; // ~0.75 Hz heartbeat
    assign led[1]   = pll_lock;   // PLL locked
    assign led[7:2] = '0;         // unused

    // ══════════════════════════════════════════════════════════
    // External Penumbra Bus — driven by the machine, decoded here
    //
    // machine_penumbra2 exposes a single post-L2 master port. The board
    // attaches the SDRAM, boot ROM, UART, and the autoconfig SD device on
    // this bus, then drives the read response (rdata/busy/fault) back in.
    // ══════════════════════════════════════════════════════════
    logic [31:0] mem_addr, mem_wdata, mem_rdata;
    logic [3:0]  mem_byte_en;
    logic        mem_we, mem_re;
    logic        mem_busy;
    logic        bus_fault;

    // ── IRQ aggregation ─────────────────────────────────────
    // The machine's timer is internal; the board contributes the device
    // interrupts. i_timer_irq is the direct-injection path, left idle.
    logic uart_irq;
    logic spi_irq;

    // ── Autoconfig control from the machine's bus controller ──
    logic busctl_bus_rst;
    logic busctl_cfg_en;

    // ══════════════════════════════════════════════════════════
    // The machine: core + MMU + L1s + arbiter + fill + L2 + sysregs
    // ══════════════════════════════════════════════════════════
    machine_penumbra2 #(
        .MACH_NAME0 (32'h33584C55),   // "ULX3"
        .MACH_NAME1 (32'h00000053),   // "S\0\0\0"
        .CPU_FREQ   (CLK_FREQ)
    ) u_machine (
        .i_clk         (clk),
        .i_rst         (rst),

        .i_irq         (uart_irq | spi_irq),
        .i_timer_irq   (1'b0),        // internal timer is the source

        .o_bus_addr    (mem_addr),
        .o_bus_wdata   (mem_wdata),
        .o_bus_byte_en (mem_byte_en),
        .o_bus_re      (mem_re),
        .o_bus_we      (mem_we),
        .i_bus_rdata   (mem_rdata),
        .i_bus_busy    (mem_busy),
        .i_bus_fault   (bus_fault),

        .o_bus_rst     (busctl_bus_rst),
        .o_bus_cfg_en  (busctl_cfg_en),

        // Commit / retire / trace observability — unused on the board
        // (the design is kept live by the real bus + UART + SDRAM pins).
        /* verilator lint_off PINCONNECTEMPTY */
        .o_commit_idx        (),
        .o_commit_data       (),
        .o_commit_we         (),
        .o_retire_valid      (),
        .o_retire_op_class   (),
        .o_retire_pc         (),
        .o_retire_sr         (),
        .o_fault_commit      (),
        .o_fault_vec         (),
        .o_eret_commit       (),
        .o_dc_commit         (),
        .o_dc_commit_pc      (),
        .o_dc_commit_op_class(),
        .o_branch_taken      (),
        .o_branch_target     (),
        .o_branch_pc         (),
        .o_ex_pc             (),
        .o_ex_valid          (),
        .o_mem_pc            (),
        .o_mem_valid         (),
        .o_prog_end          ()
        /* verilator lint_on PINCONNECTEMPTY */
    );

    // ══════════════════════════════════════════════════════════
    // Memory bus — device-side address decode (OR-combine)
    // ══════════════════════════════════════════════════════════
    localparam int SDRAM_BYTES = 32 * 1024 * 1024;   // 32 MB
    localparam int ROM_WORDS   = 16384;               // 64 KB boot ROM

    // ── SDRAM (system RAM) ──────────────────────────────────
    logic        ram_sel, ram_sel_r;
    logic [31:0] ram_rdata_raw;
    logic        ram_busy_raw;

    bus_devsel #(.BASE(RAM_BASE), .SIZE(SDRAM_BYTES))
        u_ram_sel (.i_addr(mem_addr), .o_sel(ram_sel));
    always_ff @(posedge clk) ram_sel_r <= ram_sel;

    // ── SDRAM v2: bus adapter → CDC → controller → ECP5 PHY → pins ─
    logic        sys_req_valid, sys_req_we, sys_req_ready;
    logic [31:0] sys_req_addr,  sys_req_wdata;
    logic [3:0]  sys_req_byte_en;
    logic        sys_rsp_valid, sys_rsp_ready, sys_done;
    logic [31:0] sys_rsp_data;

    sdram_bus_adapter u_sdram_adp (
        .i_clk         (clk),
        .i_rst         (rst),
        .i_addr        (mem_addr),
        .i_wdata       (mem_wdata),
        .i_byte_en     (mem_byte_en),
        .i_we          (mem_we & ram_sel),
        .i_re          (mem_re & ram_sel),
        .o_rdata       (ram_rdata_raw),
        .o_busy        (ram_busy_raw),
        .o_req_valid   (sys_req_valid),
        .o_req_we      (sys_req_we),
        .o_req_addr    (sys_req_addr),
        .o_req_wdata   (sys_req_wdata),
        .o_req_byte_en (sys_req_byte_en),
        .i_req_ready   (sys_req_ready),
        .i_rsp_valid   (sys_rsp_valid),
        .i_rsp_data    (sys_rsp_data),
        .o_rsp_ready   (sys_rsp_ready),
        .i_done        (sys_done)
    );

    // ── CDC ↔ controller (SDRAM domain) ─────────────────────
    logic        sd_req_valid, sd_req_we, sd_req_ready;
    logic [31:0] sd_req_addr,  sd_req_wdata;
    logic [3:0]  sd_req_byte_en;
    logic        sd_rsp_valid, sd_rsp_ready, sd_done;
    logic [31:0] sd_rsp_data;

    sdram_cdc u_sdram_cdc (
        // Sys side @ 25 MHz
        .i_sys_clk         (clk),
        .i_sys_rst         (rst),
        .i_sys_req_valid   (sys_req_valid),
        .i_sys_req_we      (sys_req_we),
        .i_sys_req_addr    (sys_req_addr),
        .i_sys_req_wdata   (sys_req_wdata),
        .i_sys_req_byte_en (sys_req_byte_en),
        .o_sys_req_ready   (sys_req_ready),
        .o_sys_rsp_valid   (sys_rsp_valid),
        .o_sys_rsp_data    (sys_rsp_data),
        .i_sys_rsp_ready   (sys_rsp_ready),
        .o_sys_done        (sys_done),
        // SDRAM side @ 100 MHz
        .i_sd_clk          (clk_sdram),
        .i_sd_rst          (rst_sd),
        .o_sd_req_valid    (sd_req_valid),
        .o_sd_req_we       (sd_req_we),
        .o_sd_req_addr     (sd_req_addr),
        .o_sd_req_wdata    (sd_req_wdata),
        .o_sd_req_byte_en  (sd_req_byte_en),
        .i_sd_req_ready    (sd_req_ready),
        .i_sd_rsp_valid    (sd_rsp_valid),
        .i_sd_rsp_data     (sd_rsp_data),
        .o_sd_rsp_ready    (sd_rsp_ready),
        .i_sd_done         (sd_done)
    );

    logic [3:0]                  sdram_phy_cmd;
    logic                        sdram_phy_cke;
    logic [SDP.ROW_BITS-1:0]     sdram_phy_a;
    logic [SDP.BA_BITS-1:0]      sdram_phy_ba;
    logic [SDP.DQ_BITS/8-1:0]    sdram_phy_dqm;
    logic [SDP.DQ_BITS-1:0]      sdram_phy_dq_out;
    logic                        sdram_phy_dq_oe;
    logic [SDP.DQ_BITS-1:0]      sdram_phy_dq_in;

    sdram_ctrl #(
        .ROW_BITS        (SDP.ROW_BITS),
        .COL_BITS        (SDP.COL_BITS),
        .BA_BITS         (SDP.BA_BITS),
        .DQ_BITS         (SDP.DQ_BITS),
        .T_RCD           (SDP.T_RCD),
        .T_RP            (SDP.T_RP),
        .T_RFC           (SDP.T_RFC),
        .T_WR            (SDP.T_WR),
        .T_MRD           (SDP.T_MRD),
        .T_REFI          (SDP.T_REFI),
        .T_POWERUP       (SDP.T_POWERUP),
        .CAS_LATENCY     (SDP.CAS_LATENCY),
        .PHY_OUT_LATENCY (1),
        .PHY_IN_LATENCY  (1)
    ) u_sdram_ctrl (
        .i_clk           (clk_sdram),
        .i_rst           (rst_sd),
        .i_req_valid     (sd_req_valid),
        .i_req_we        (sd_req_we),
        .i_req_addr      (sd_req_addr),
        .i_req_wdata     (sd_req_wdata),
        .i_req_byte_en   (sd_req_byte_en),
        .o_req_ready     (sd_req_ready),
        .o_rsp_valid     (sd_rsp_valid),
        .o_rsp_data      (sd_rsp_data),
        .i_rsp_ready     (sd_rsp_ready),
        .o_done          (sd_done),
        .o_phy_cmd       (sdram_phy_cmd),
        .o_phy_cke       (sdram_phy_cke),
        .o_phy_a         (sdram_phy_a),
        .o_phy_ba        (sdram_phy_ba),
        .o_phy_dqm       (sdram_phy_dqm),
        .o_phy_dq_out    (sdram_phy_dq_out),
        .o_phy_dq_oe     (sdram_phy_dq_oe),
        .i_phy_dq_in     (sdram_phy_dq_in),
        .o_dbg_init_done ()
    );

    sdram_phy_ecp5 #(
        .ROW_BITS (SDP.ROW_BITS),
        .BA_BITS  (SDP.BA_BITS),
        .DQ_BITS  (SDP.DQ_BITS)
    ) u_sdram_phy (
        .i_clk        (clk_sdram),       // CLKOS — 100 MHz, 0°, IOB flops
        .i_clk_sdram  (clk_sdram_pin),   // CLKOS2 — 100 MHz, phase, ODDR
        .i_rst        (rst_sd),
        .i_phy_cmd    (sdram_phy_cmd),
        .i_phy_cke    (sdram_phy_cke),
        .i_phy_a      (sdram_phy_a),
        .i_phy_ba     (sdram_phy_ba),
        .i_phy_dqm    (sdram_phy_dqm),
        .i_phy_dq_out (sdram_phy_dq_out),
        .i_phy_dq_oe  (sdram_phy_dq_oe),
        .o_phy_dq_in  (sdram_phy_dq_in),
        .o_sdram_clk  (sdram_clk),
        .o_sdram_cke  (sdram_cke),
        .o_sdram_csn  (sdram_csn),
        .o_sdram_rasn (sdram_rasn),
        .o_sdram_casn (sdram_casn),
        .o_sdram_wen  (sdram_wen),
        .o_sdram_a    (sdram_a),
        .o_sdram_ba   (sdram_ba),
        .o_sdram_dqm  (sdram_dqm),
        .io_sdram_d   (sdram_d)
    );

    // ── Boot ROM ────────────────────────────────────────────
    logic        rom_sel, rom_sel_r;
    logic [31:0] rom_rdata_raw;
    logic        rom_busy_raw;

    bus_devsel #(.BASE(ROM_BASE), .SIZE(32'(ROM_WORDS * 4)))
        u_rom_sel (.i_addr(mem_addr), .o_sel(rom_sel));
    always_ff @(posedge clk) rom_sel_r <= rom_sel;

    boot_rom #(.ROM_WORDS(ROM_WORDS)) u_rom (
        .i_clk     (clk),
        .i_rst     (rst),
        .i_addr    (mem_addr),
        .i_re      (mem_re & rom_sel),
        .o_rdata   (rom_rdata_raw),
        .o_busy    (rom_busy_raw)
    );

    // ── UART ────────────────────────────────────────────────
    logic        uart_sel, uart_sel_r;
    logic [31:0] uart_rdata_raw;
    logic        uart_busy_raw;

    bus_devsel #(.BASE(UART_BASE), .SIZE(32'd4096))
        u_uart_sel (.i_addr(mem_addr), .o_sel(uart_sel));
    always_ff @(posedge clk) uart_sel_r <= uart_sel;

    uart #(
        .CLK_FREQ  (CLK_FREQ)
    ) u_uart (
        .i_clk   (clk),
        .i_rst   (rst),
        .i_addr  (mem_addr),
        .i_wdata (mem_wdata),
        .i_we    (mem_we & uart_sel),
        .i_re    (mem_re & uart_sel),
        .o_rdata (uart_rdata_raw),
        .o_busy  (uart_busy_raw),
        .o_tx    (ftdi_rxd),
        .i_rx    (ftdi_txd),
        .o_irq   (uart_irq)
    );

    // ══════════════════════════════════════════════════════════
    // Autoconfig device chain
    //
    // busctl_cfg_en (from the machine) starts the daisy chain; each
    // device's cfg_out feeds the next cfg_in. A bus fault at chain end
    // signals "no more devices" to the ROM autoconfig loop.
    // ══════════════════════════════════════════════════════════
    logic [31:0] spi_dev_addr, spi_dev_wdata;
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  spi_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        spi_dev_we, spi_dev_re;
    logic [31:0] spi_dev_rdata;
    logic        spi_dev_busy;

    logic [31:0] ac_spi_rdata;
    logic        ac_spi_busy, ac_spi_sel;
    /* verilator lint_off UNUSEDSIGNAL */
    logic        ac_spi_cfg_out;
    /* verilator lint_on UNUSEDSIGNAL */

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_SD),
        .DEV_SIZE  (32'd4096),
        .DEV_ID    (32'd0),
        .DEV_NAME0 (32'h00004453)    // "SD\0\0" packed LE
    ) u_ac_spi (
        .i_clk       (clk),
        .i_rst       (rst),
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

    logic spi_cs0;

    spi #(
        .CLK_FREQ   (CLK_FREQ),
        .FIFO_DEPTH (512)
    ) u_spi (
        .i_clk   (clk),
        .i_rst   (rst),
        .i_addr  (spi_dev_addr),
        .i_wdata (spi_dev_wdata),
        .i_we    (spi_dev_we),
        .i_re    (spi_dev_re),
        .o_rdata (spi_dev_rdata),
        .o_busy  (spi_dev_busy),
        .o_sclk  (sd_clk),
        .o_mosi  (sd_cmd),
        .i_miso  (sd_d[0]),
        .o_cs0   (spi_cs0),
        .o_cs1   (),
        .o_irq   (spi_irq)
    );

    // SD card SPI mode pin mapping
    assign sd_d[3] = spi_cs0;     // CS (active low, from SPI control)
    assign sd_d[2] = 1'b1;        // Unused in SPI mode, pull high
    assign sd_d[1] = 1'b1;        // Unused in SPI mode, pull high

    // Autoconfig combined bus signals
    logic [31:0] acfg_rdata;
    logic        acfg_busy;
    logic        acfg_sel;
    logic        acfg_sel_r;
    assign acfg_rdata = ac_spi_rdata;
    assign acfg_busy  = ac_spi_busy;
    assign acfg_sel   = ac_spi_sel;
    always_ff @(posedge clk) acfg_sel_r <= acfg_sel;

    // ── Bus response combine ────────────────────────────────
    // The read mux uses the registered selects, so it lands the cycle each
    // slave's registered read is valid; busy and fault use the combinational
    // selects, so the core stalls — or faults on a no-device access — in the
    // same cycle the request is issued. An OR fabric with one slave per
    // address (the $onehot0 assertion below guards that mutual exclusion).

    always_comb begin
        if (ram_sel_r)       mem_rdata = ram_rdata_raw;
        else if (rom_sel_r)  mem_rdata = rom_rdata_raw;
        else if (uart_sel_r) mem_rdata = uart_rdata_raw;
        else if (acfg_sel_r) mem_rdata = acfg_rdata;
        else                 mem_rdata = 32'b0;
    end
    assign mem_busy = (ram_sel  ? ram_busy_raw  : 1'b0) |
                      (rom_sel  ? rom_busy_raw  : 1'b0) |
                      (uart_sel ? uart_busy_raw : 1'b0) |
                      acfg_busy;
    assign bus_fault = (mem_re | mem_we) & ~(ram_sel | rom_sel | uart_sel | acfg_sel);

    // ══════════════════════════════════════════════════════════
    // Assertion — exactly one device (at most) claims any address.
    // ══════════════════════════════════════════════════════════
    // One-cold-or-less: a faulting access selects nothing; a real access
    // selects exactly one device. Overlapping decodes are a wiring bug.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0] sel_onehot;
    assign sel_onehot = {ram_sel, rom_sel, uart_sel, acfg_sel};
    /* verilator lint_on UNUSEDSIGNAL */
    // synthesis translate_off
    always_ff @(posedge clk)
        if (!rst && (mem_re || mem_we))
            assert ($onehot0(sel_onehot))
                else $error("ulx3s_penumbra2_top: overlapping device decode");
    // synthesis translate_on

endmodule
