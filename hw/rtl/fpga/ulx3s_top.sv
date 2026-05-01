// ULX3S Board Top — Penumbra CPU on ECP5-85F
//
// Wires the Penumbra CPU to BRAM (boot ROM + working RAM) and a
// real NS16450 UART on the FTDI serial port. Runs at 25 MHz from
// the on-board crystal oscillator (no PLL).
//
// This is the board-specific integration module. It handles:
//   - Pin mapping (FPGA I/O to board peripherals)
//   - Reset generation (power-on counter)
//   - ESP32 disable (free the UART)
//   - LED indicators
//   - Machine wiring (same pattern as machine_sim.sv)
//
// For other boards, copy this file and adjust pins/clocking/RAM.

// ── SDRAM clock-out phase shift (CLKOS2) ──────────────────────────
// The SDRAM-clock pin is forwarded via ODDRX1F clocked from CLKOS2,
// which is phase-shifted relative to the controller clock so the
// SDRAM samples our drives near the centre of the data window.  The
// phase value is empirical — different ULX3S boards / SDRAM variants
// see slightly different working windows.  The build-time sweep
// procedure (`make fpga PHASE_DEG=N TOP=ulx3s_top` for each N in 0,
// 45, 90, …, 315) finds the contiguous arc that boots and passes
// `_ram_check`; pick its centre.  See doc/internals/sdram-controller.md
// § Step-5 phase sweep for the canonical procedure and per-board
// results.
`ifndef SDRAM_PHASE_DEG
`define SDRAM_PHASE_DEG 270
`endif

module ulx3s_top (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    input  logic [6:0] btn,
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
    // Single source of truth for system clock frequency.
    // Update this if PLL parameters change.
    localparam int CLK_FREQ = 12_500_000;   // Hz (derived from PLL below)

    // ── SDRAM chip preset (one-line preset swap) ─────────────────
    // Change this RHS to switch SDRAM variants — e.g., for a board
    // with IS42S16160G or AS4C16M16SA, drop in that struct here.
    // The controller, PHY, and pin-width logic all read SDP.FIELD,
    // so timings and geometry update in lockstep with the change.
    localparam sdram_params_t SDP = W9825_100;

    // ── ESP32 disable ──────────────────────────────────────────
    assign wifi_en = 1'b0;

    // ── PLL: 25 MHz → 3 outputs sharing one VCO ──────────────────
    // fCLKOP  = fCLKI × CLKFB_DIV / CLKI_DIV = 25 × 1 / 2 = 12.5 MHz
    // fVCO    = fCLKOP × CLKOP_DIV = 12.5 × 48 = 600 MHz (400-800 OK)
    // fCLKOS  = fVCO / CLKOS_DIV  = 600 / 6  = 100 MHz   (SDRAM fabric)
    // fCLKOS2 = fVCO / CLKOS2_DIV = 600 / 6  = 100 MHz   (SDRAM pin clock)
    //
    // CPHASE/FPHASE convention: 0° = CPHASE = (DIV - 1), FPHASE = 0;
    // each FPHASE step = 1/8 VCO cycle; CPHASE counts in whole VCO
    // cycles.  Phase shift φ from 0° subtracts (φ × DIV / 360°) VCO
    // cycles from CPHASE.  For DIV=6 and φ=270°: shift = 4.5 VCO
    // cycles → CPHASE=0, FPHASE=4.
    //
    // CLKOP @ 12.5 MHz drives the CPU/system bus — capped here by the
    // current CPU long-path (~16 MHz).  Will rise on its own schedule
    // once the CPU is optimised; the SDRAM is decoupled via sdram_cdc.
    //
    // CLKOS  @ 100 MHz, 0° phase   — SDRAM controller fabric clock.
    //                               Drives sdram_ctrl, sdram_phy_ecp5
    //                               IOB flops, and the SDRAM-side of
    //                               sdram_cdc.
    // CLKOS2 @ 100 MHz, SDRAM_PHASE_DEG° — SDRAM pin clock, forwarded
    //                               via the PHY's ODDRX1F.  Default
    //                               270° (step-4 baseline); override
    //                               at build time with
    //                                 make fpga PHASE_DEG=N TOP=ulx3s_top
    //                               for the step-5 sweep.

    // ── Phase lookup table for CLKOS2 (CLKOS2_DIV = 6) ──────────────
    // 8-point sweep grid at every 45°.  CPHASE/FPHASE values were
    // derived from the convention above.  The 315° entry wraps the
    // CPHASE around DIV (i.e., 5 + 6/8 of a cycle late = 315° early).
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

    logic clk;            // CLKOP — 12.5 MHz system clock
    logic clk_sdram;      // CLKOS — 100 MHz SDRAM fabric clock
    logic clk_sdram_pin;  // CLKOS2 — 100 MHz, phase-shifted, to ODDR
    logic pll_lock;

    (* keep *) EHXPLLL #(
        .CLKI_DIV      (2),
        .CLKFB_DIV     (1),
        .CLKOP_DIV     (48),
        .CLKOP_ENABLE  ("ENABLED"),
        .CLKOP_CPHASE  (47),
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
    // Hold reset until PLL locks, then count 2^18 clocks.
    // 2^18 / 12.5 MHz ≈ 21 ms — exceeds 10 ms minimum for
    // power-on reset (see doc/hardware/bus-protocol.md Reset Timing).
    // Pressing btn[1] reasserts reset (synchronizer for btn input).
    logic btn1_sync1, btn1_sync2;
    always_ff @(posedge clk) begin
        btn1_sync1 <= btn[1];
        btn1_sync2 <= btn1_sync1;
    end

    logic [17:0] rst_cnt = '0;
    logic        rst;

    always_ff @(posedge clk) begin
        if (!pll_lock || btn1_sync2)
            rst_cnt <= '0;
        else if (!rst_cnt[17])
            rst_cnt <= rst_cnt + 1;
    end
    assign rst = !rst_cnt[17];

    // ── SDRAM-domain reset: 2-FF synchronizer of `rst` into clk_sdram.
    // Both reset edges (assert and deassert) cross the boundary; the
    // synchronizer turns the deassert into a clean edge on the SDRAM
    // clock so sdram_ctrl / sdram_phy_ecp5 / sdram_cdc(sdram side) all
    // come out of reset together.  Asynchronous-assert / synchronous-
    // deassert is the canonical pattern for cross-domain resets.
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

    logic halted;

    assign led[0] = hb_cnt[24];   // ~0.75 Hz heartbeat
    assign led[1] = !halted;      // ON while CPU is running
    assign led[2] = pll_lock;     // DEBUG: PLL locked
    assign led[3] = rst;          // DEBUG: reset active
    // led[4] and led[5] assigned below after UART instantiation
    // led[6] and led[7] assigned below after SDRAM instantiation

    // Debug: toggle on THR write (proves CPU writes to UART)
    logic dbg_thr_toggle;
    logic dbg_thr_write;
    always_ff @(posedge clk) begin
        if (rst)
            dbg_thr_toggle <= 1'b0;
        else if (dbg_thr_write)
            dbg_thr_toggle <= ~dbg_thr_toggle;
    end
    // thr_write = bus write to UART register 0 with DLAB=0
    // We detect it from the bus signals directly
    assign dbg_thr_write = mem_we & uart_sel;

    // ══════════════════════════════════════════════════════════
    // CPU ↔ memory bus
    // ══════════════════════════════════════════════════════════
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

    // ── IRQ ─────────────────────────────────────────────────
    logic uart_irq;
    logic spi_irq;
    logic timer_irq;

    // ── Timer tick prescaler (CLK_FREQ → ~1 MHz toggle) ───────
    // Tick freq = CLK_FREQ / (2 * PRESCALE_DIV).
    // Choose divisor closest to 1 MHz: round(CLK_FREQ / 2_000_000).
    localparam int PRESCALE_DIV  = (CLK_FREQ + 1_000_000) / 2_000_000;
    localparam int TICK_FREQ     = CLK_FREQ / (2 * PRESCALE_DIV);
    logic [$clog2(PRESCALE_DIV)-1:0] prescale_cnt;
    logic timer_tick;

    always_ff @(posedge clk) begin
        if (rst) begin
            prescale_cnt <= '0;
            timer_tick   <= 1'b0;
        end else if (prescale_cnt == ($bits(prescale_cnt))'(PRESCALE_DIV - 1)) begin
            prescale_cnt <= '0;
            timer_tick   <= ~timer_tick;
        end else begin
            prescale_cnt <= prescale_cnt + 1;
        end
    end

    // ══════════════════════════════════════════════════════════
    // CPU Core
    // ══════════════════════════════════════════════════════════
    cpu_core u_cpu (
        .i_clk          (clk),
        .i_rst          (rst),

        .o_mem_addr     (mem_addr),
        .o_mem_wdata    (mem_wdata),
        .o_mem_byte_en  (mem_byte_en),
        .o_mem_we       (mem_we),
        .o_mem_re       (mem_re),
        .i_mem_rdata    (mem_rdata),
        .i_mem_busy     (mem_busy),
        .i_bus_fault    (bus_fault),

        .o_sys_dev      (sys_dev),
        .o_sys_reg      (sys_reg),
        .o_sys_wdata    (sys_wdata),
        .o_sys_cycle    (sys_cycle),
        .o_sys_we       (sys_we),
        .i_sys_rdata    (sys_rdata),

        .i_timer_irq    (timer_irq),
        .i_irq          (uart_irq | spi_irq),

        .o_pc           (),
        .o_halted       (halted),
        .i_dbg_reg_addr (4'd0),
        .o_dbg_reg_data (),

        .o_trace_valid  (),
        .o_trace_sr     ()
    );

    // ══════════════════════════════════════════════════════════
    // Memory bus — device-side address decode (OR-combine)
    // ══════════════════════════════════════════════════════════

    // ── Device parameters ──────────────────────────────────
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
    // Step-4 dual-domain configuration:
    //   • Bus adapter runs on the system clock (12.5 MHz today).
    //   • CDC bridge crosses 12.5 MHz ↔ 100 MHz with toggle
    //     synchronizers and a quasi-static payload.
    //   • Controller + IOB-flop side of the PHY run on CLKOS @ 100 MHz.
    //   • SDRAM clock pin is forwarded via ODDRX1F clocked by CLKOS2
    //     (100 MHz, 270°) so the chip samples our drives near the
    //     centre of the data window.
    // Timing parameters come from the W9825_100 preset (CL=2 with the
    // 2-cycle PHY round-trip absorbed by PHY_OUT/IN_LATENCY).
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
        // Sys side @ 12.5 MHz
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
    logic                        sdram_init_done;

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
        .o_dbg_init_done (sdram_init_done)
    );

    sdram_phy_ecp5 #(
        .ROW_BITS (SDP.ROW_BITS),
        .BA_BITS  (SDP.BA_BITS),
        .DQ_BITS  (SDP.DQ_BITS)
    ) u_sdram_phy (
        .i_clk        (clk_sdram),       // CLKOS — 100 MHz, 0°, IOB flops
        .i_clk_sdram  (clk_sdram_pin),   // CLKOS2 — 100 MHz, 270°, ODDR
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

    assign led[6] = sdram_init_done;
    assign led[7] = ram_busy_raw;

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
        // REF_FREQ defaults to 1.8432 MHz (16450 standard crystal),
        // giving 115200 baud at divisor=1 regardless of CLK_FREQ.
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

    assign led[4] = mem_re;           // any bus read?
    assign led[5] = mem_we;           // any bus write?

    // ══════════════════════════════════════════════════════════
    // Autoconfig device chain
    //
    // Same pattern as machine_sim.sv: busctl_cfg_en starts the
    // daisy chain, each device's cfg_out feeds the next cfg_in.
    // Bus fault at chain end signals "no more devices" to the
    // ROM autoconfig loop.
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
        .FIFO_DEPTH (512),
        // 12.5 MHz / (2*(15+1)) ≈ 390 kHz — safe for SD card init (needs <400 kHz)
        .SLOW_DIV   (16'd15),
        // 12.5 MHz / (2*(0+1)) = 6.25 MHz — operational speed
        .FAST_DIV   (16'd0)
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
    assign sd_d[3] = spi_cs0;     // CS (active low, directly from SPI control)
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

    // ── Bus response OR-combine ─────────────────────────────
    assign mem_rdata = (ram_sel_r  ? ram_rdata_raw  : 32'b0) |
                       (rom_sel_r  ? rom_rdata_raw  : 32'b0) |
                       (uart_sel_r ? uart_rdata_raw : 32'b0) |
                       (acfg_sel_r ? acfg_rdata     : 32'b0);

    assign mem_busy = (ram_sel  ? ram_busy_raw  : 1'b0) |
                      (rom_sel  ? rom_busy_raw  : 1'b0) |
                      (uart_sel ? uart_busy_raw : 1'b0) |
                      acfg_busy;

    assign bus_fault = (mem_re | mem_we) & ~(ram_sel | rom_sel | uart_sel | acfg_sel);

    // ══════════════════════════════════════════════════════════
    // Sysreg devices
    // ══════════════════════════════════════════════════════════

    // ── Machine identity (device 8) ─────────────────────────
    // SYSDEV_CPU (device 1) lives inside cpu_core (CPU identity + perfctrs).
    logic [31:0] machid_rdata;

    machid #(
        .MACH_NAME0 (32'h33584C55),   // "ULX3"
        .MACH_NAME1 (32'h00000053),   // "S\0\0\0"
        .MACH_NAME2 (32'h00000000),
        .CPU_FREQ   (CLK_FREQ)
    ) u_machid (
        .i_sys_reg  (sys_reg),
        .o_sys_rdata(machid_rdata)
    );

    // ── Bus Controller (device 4) ───────────────────────────
    logic [31:0] busctl_rdata;
    logic        busctl_bus_rst;
    logic        busctl_cfg_en;

    busctl u_busctl (
        .i_clk       (clk),
        .i_rst       (rst),
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
        .i_clk       (clk),
        .i_rst       (rst),
        .i_tick      (timer_tick),
        .i_sys_reg   (sys_reg),
        .i_sys_wdata (sys_wdata),
        .i_sys_we    (sys_we & sys_cycle & (sys_dev == SYSDEV_TIMER)),
        .o_sys_rdata (timer_rdata),
        .o_irq       (timer_irq)
    );

    // ── Sysreg read mux ────────────────────────────────────
    always_comb begin
        case (sys_dev)
            SYSDEV_BUS:   sys_rdata = busctl_rdata;
            SYSDEV_TIMER: sys_rdata = timer_rdata;
            SYSDEV_MACH:  sys_rdata = machid_rdata;
            default:      sys_rdata = 32'b0;
        endcase
    end

endmodule
