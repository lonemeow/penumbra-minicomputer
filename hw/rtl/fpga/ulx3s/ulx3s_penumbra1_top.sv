// ULX3S Board Top (Penumbra/1) — gen1 CPU system on ECP5-85F
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
// procedure (`make fpga PHASE_DEG=N BOARD=ulx3s CORE=penumbra1` for each N in 0,
// 45, 90, …, 315) finds the contiguous arc that boots and passes
// `_ram_check`; pick its centre.  See doc/internals/sdram-controller.md
// § Step-5 phase sweep for the canonical procedure and per-board
// results.
`ifndef SDRAM_PHASE_DEG
`define SDRAM_PHASE_DEG 180
`endif

module ulx3s_penumbra1_top (
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

    // ── USB host (US2 socket, D+/D- direct to FPGA) ────────
    // The bd pair is the bidirectional single-ended view of D+/D-:
    // sensed while the PHY listens (both levels are needed to tell
    // SE0 from J/K) and driven while it owns the bus.  The pu pair
    // works the board's pull network — driven low for the host's
    // D+/D- pull-downs, released otherwise.  (The board's separate
    // differential-input pair on the same copper stays unused.)
    inout  wire        usb_fpga_bd_dp,
    inout  wire        usb_fpga_bd_dn,
    inout  wire        usb_fpga_pu_dp,
    inout  wire        usb_fpga_pu_dn,

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
    import ecp5_pll_pkg::*;

    // ── Clock targets → PLL config (computed, not hand-picked) ────
    // Change CPU_HZ to retarget the CPU/system clock; ecp5_pll_compute derives
    // the EHXPLLL dividers and CLK_FREQ together off the same struct, so they
    // cannot drift. The SDRAM stays at 100 MHz — the W9825_100 preset and the
    // CLKOS2 phase table below assume CLKOS_DIV==6, asserted after the PLL.
    localparam longint CPU_HZ   = 25_000_000;
    localparam longint SDRAM_HZ = 100_000_000;
    localparam ecp5_pll_cfg_t PLL = ecp5_pll_compute(25_000_000, CPU_HZ, SDRAM_HZ);
    localparam int CLK_FREQ = int'(PLL.clk_hz);   // derived from the PLL config

    // ── SDRAM chip preset (one-line preset swap) ─────────────────
    // Change this RHS to switch SDRAM variants — e.g., for a board
    // with IS42S16160G or AS4C16M16SA, drop in that struct here.
    // The controller, PHY, and pin-width logic all read SDP.FIELD,
    // so timings and geometry update in lockstep with the change.
    localparam sdram_params_t SDP = W9825_100;

    // ── ESP32 disable ──────────────────────────────────────────
    assign wifi_en = 1'b0;

    // ── PLL: 25 MHz crystal → 3 outputs sharing one VCO ──────────
    // Dividers are computed by ecp5_pll_compute from CPU_HZ / SDRAM_HZ
    // above; at 25 MHz CPU / 100 MHz SDRAM they resolve to the historical
    // {CLKI 1, CLKFB 1, CLKOP 24, CLKOS 6} on a 600 MHz VCO:
    // fCLKOP  = fCLKI × CLKFB_DIV / CLKI_DIV = 25 × 1 / 1 = 25 MHz
    // fVCO    = fCLKOP × CLKOP_DIV = 25 × 24 = 600 MHz (400-800 OK)
    // fCLKOS  = fVCO / CLKOS_DIV  = 600 / 6  = 100 MHz   (SDRAM fabric)
    // fCLKOS2 = fVCO / CLKOS2_DIV = 600 / 6  = 100 MHz   (SDRAM pin clock)
    //
    // CPHASE/FPHASE convention: 0° = CPHASE = (DIV - 1), FPHASE = 0;
    // each FPHASE step = 1/8 VCO cycle; CPHASE counts in whole VCO
    // cycles.  Phase shift φ from 0° subtracts (φ × DIV / 360°) VCO
    // cycles from CPHASE.  For DIV=6 and φ=270°: shift = 4.5 VCO
    // cycles → CPHASE=0, FPHASE=4.
    //
    // CLKOP @ 25 MHz drives the CPU/system bus.  Empirical fmax on
    // ECP5-85F speed grade 6 is ~28–30 MHz after the TLB and regfile
    // were moved to distributed RAM; 25 MHz leaves ~14% margin.
    // The SDRAM is decoupled via sdram_cdc and stays at 100 MHz.
    //
    // CLKOS  @ 100 MHz, 0° phase   — SDRAM controller fabric clock.
    //                               Drives sdram_ctrl, sdram_phy_ecp5
    //                               IOB flops, and the SDRAM-side of
    //                               sdram_cdc.
    // CLKOS2 @ 100 MHz, SDRAM_PHASE_DEG° — SDRAM pin clock, forwarded
    //                               via the PHY's ODDRX1F.  Default
    //                               270° (step-4 baseline); override
    //                               at build time with
    //                                 make fpga PHASE_DEG=N BOARD=ulx3s CORE=penumbra1
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

    // ── USB clock (CLKOS3) ─────────────────────────────────────────
    // The USB host tier runs at 60 MHz (usb_pkg's 5×/40× oversample
    // base), served off the shared VCO like every other output —
    // exact only: a CPU/SDRAM retarget that moves the VCO off a
    // 60 MHz multiple must fail the build here, not detune USB.
    localparam longint USB_HZ = 60_000_000;
    localparam int CLKOS3_DIV_VAL = ecp5_pll_aux_div(PLL.vco_hz, USB_HZ);

    logic clk;            // CLKOP — 25 MHz system clock
    logic clk_sdram;      // CLKOS — 100 MHz SDRAM fabric clock
    logic clk_sdram_pin;  // CLKOS2 — 100 MHz, phase-shifted, to ODDR
    logic clk_usb;        // CLKOS3 — 60 MHz USB host clock
    logic pll_lock;

    (* keep *) EHXPLLL #(
        .CLKI_DIV      (PLL.clki_div),
        .CLKFB_DIV     (PLL.clkfb_div),
        .CLKOP_DIV     (PLL.clkop_div),
        .CLKOP_ENABLE  ("ENABLED"),
        .CLKOP_CPHASE  (PLL.clkop_cphase),
        .CLKOP_FPHASE  (0),
        .CLKOS_DIV     (PLL.clkos_div),
        .CLKOS_ENABLE  ("ENABLED"),
        .CLKOS_CPHASE  (PLL.clkos_cphase),   // 0° phase
        .CLKOS_FPHASE  (0),
        .CLKOS2_DIV    (PLL.clkos_div),      // same freq as CLKOS
        .CLKOS2_ENABLE ("ENABLED"),
        .CLKOS2_CPHASE (CLKOS2_CPHASE_VAL),  // SDRAM_PHASE_DEG° phase
        .CLKOS2_FPHASE (CLKOS2_FPHASE_VAL),
        .CLKOS3_DIV    (CLKOS3_DIV_VAL),
        .CLKOS3_ENABLE ("ENABLED"),
        .CLKOS3_CPHASE (CLKOS3_DIV_VAL - 1), // 0° phase
        .CLKOS3_FPHASE (0),
        .FEEDBK_PATH   ("CLKOP")
    ) u_pll (
        .CLKI         (clk_25mhz),
        .CLKFB        (clk),
        .CLKOP        (clk),
        .CLKOS        (clk_sdram),
        .CLKOS2       (clk_sdram_pin),
        .CLKOS3       (clk_usb),
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
        .ENCLKOS3     (1'b1)
    );

    // The CLKOS2 phase table above is calibrated for CLKOS_DIV==6 (the SDRAM
    // at fVCO/6). ecp5_pll_compute is free to pick a different SDRAM divider
    // for some target; if it does, those phase constants no longer apply, so
    // refuse to build rather than forward a wrong SDRAM pin clock.
    initial begin
        assert (PLL.valid)
            else $fatal(1, "ecp5_pll: no legal PLL config for CPU_HZ=%0d", CPU_HZ);
        assert (PLL.clkos_div == 6)
            else $fatal(1, "ecp5_pll: CLKOS_DIV=%0d != 6; SDRAM phase table invalid",
                        PLL.clkos_div);
        assert (CLKOS3_DIV_VAL != 0)
            else $fatal(1, "ecp5_pll: VCO %0d cannot serve the 60 MHz USB clock",
                        PLL.vco_hz);
    end

    // ── Reset: PLL lock + btn[1] (FIRE1) manual reset ──────────
    // Hold reset until PLL locks, then count 2^19 clocks.
    // 2^19 / 25 MHz ≈ 21 ms — exceeds 10 ms minimum for power-on
    // reset (see doc/hardware/bus-protocol.md Reset Timing).
    // Pressing btn[1] reasserts reset (synchronizer for btn input).
    logic btn1_sync1, btn1_sync2;
    always_ff @(posedge clk) begin
        btn1_sync1 <= btn[1];
        btn1_sync2 <= btn1_sync1;
    end

    // Init value at sim-time is harmless and ensures rst_cnt
    // starts at 0 before pll_lock asserts; the always_ff covers
    // post-reset behaviour.  Suppress the procedural-init warn.
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

    // Promote rst onto a global net via DCCA. With ~5700-way fanout
    // through general fabric, rst was the dominant routing-congestion
    // source on this design — verified via hw/tools/list_fanout.py
    // before this change. ECP5 has 16 global nets; only 3 are in use
    // (PLL clock outputs), so headroom is ample. CE=1 keeps the buffer
    // always-on; CLKO is combinationally equal to CLKI. (* keep *)
    // prevents yosys from optimizing the buffer away on the grounds
    // that CLKO == CLKI functionally — we *want* the DCCA so PnR
    // routes the output via global net, not the input.
    (* keep *) DCCA rst_dcca (
        .CLKI (rst_raw),
        .CE   (1'b1),
        .CLKO (rst)
    );

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

    // ── USB-domain reset: the same 2-FF pattern into clk_usb. ──
    logic rst_usb_sync1, rst_usb_sync2;
    always_ff @(posedge clk_usb) begin
        rst_usb_sync1 <= rst;
        rst_usb_sync2 <= rst_usb_sync1;
    end
    wire rst_usb = rst_usb_sync2;

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
    // CPU ↔ memory bus
    //
    // Two segments: cpu_mem_* (cpu_core → L2) and mem_* (the
    // shared system bus that all devices decode).  The L2 cache
    // is always instantiated; software opts out by leaving
    // CTRL.enable=0 (the reset state).
    // ══════════════════════════════════════════════════════════
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

        .o_mem_addr     (cpu_mem_addr),
        .o_mem_wdata    (cpu_mem_wdata),
        .o_mem_byte_en  (cpu_mem_byte_en),
        .o_mem_we       (cpu_mem_we),
        .o_mem_re       (cpu_mem_re),
        .o_mem_cacheable(cpu_mem_cacheable),
        .i_mem_rdata    (cpu_mem_rdata),
        .i_mem_busy     (cpu_mem_busy),
        .i_bus_fault    (bus_fault),

        .o_sys_dev      (sys_dev),
        .o_sys_reg      (sys_reg),
        .o_sys_wdata    (sys_wdata),
        .o_sys_cycle    (sys_cycle),
        .o_sys_we       (sys_we),
        .i_sys_rdata    (sys_rdata),

        .i_timer_irq    (timer_irq),
        .i_irq          (uart_irq | spi_irq | usb_irq),

        .o_pc           (),
        .o_halted       (),
        .i_dbg_reg_addr (4'd0),
        .o_dbg_reg_data (),

        .o_trace_valid  (),
        .o_trace_sr     ()
    );

    // ══════════════════════════════════════════════════════════
    // L2 cache — always instantiated
    //
    // 64 KiB, 4-way, tree-PLRU, 2-cycle hit pipeline,
    // write-invalidate-on-hit.  Disabled at reset; software
    // brings it up via WRSYS SYSDEV_L2_CACHE CTRL=1.  While
    // disabled, every access is a combinational pass-through.
    // ══════════════════════════════════════════════════════════
    logic [31:0] l2_rdata;

    l2_cache u_l2 (
        .i_clk          (clk),
        .i_rst          (rst),
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
        .i_mem_busy     (mem_busy),
        .i_mem_fault    (1'b0),
        .i_sys_reg      (sys_reg),
        .i_sys_wdata    (sys_wdata),
        .i_sys_we       (sys_we & sys_cycle & (sys_dev == SYSDEV_L2_CACHE)),
        .o_sys_rdata    (l2_rdata)
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
    //   • Bus adapter runs on the system clock (25 MHz today).
    //   • CDC bridge crosses 25 MHz ↔ 100 MHz with toggle
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
    // SPI is byte-oriented; byte_en from the autoconfig wrapper
    // isn't consumed by the SPI device itself.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  spi_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        spi_dev_we, spi_dev_re;
    logic [31:0] spi_dev_rdata;
    logic        spi_dev_busy;

    logic [31:0] ac_spi_rdata;
    logic        ac_spi_busy, ac_spi_sel;
    // cfg_out is the daisy-chain to the next autoconfig device (USB).
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
        .CLK_FREQ   (CLK_FREQ),
        .FIFO_DEPTH (512)
        // SLOW_DIV / FAST_DIV derive from CLK_FREQ + the SCLK targets in
        // spi.sv.  At 25 MHz that is SLOW=31 (≈391 kHz init) and FAST=1
        // (6.25 MHz operational) — unchanged from the prior hardcoding,
        // but now they track CLK_FREQ instead of silently going out of
        // spec if the system clock is bumped for fmax.
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

    // ── USB host controller (second device in chain) ─────────
    // Same block as machine_sim's, with usb_phy_ecp5 on the seam in
    // place of the sim PHY — the swap the PHY tier split exists for.
    logic [31:0] usb_dev_addr, usb_dev_wdata;
    // Word-strided device: byte enables carry no information here.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0]  usb_dev_byte_en;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        usb_dev_we, usb_dev_re;
    logic [31:0] usb_dev_rdata;
    logic        usb_dev_busy;

    logic [31:0] ac_usb_rdata;
    logic        ac_usb_busy, ac_usb_sel;
    // Last device in the chain: the dangling cfg_out is the
    // "no more devices" end the ROM's autoconfig loop probes.
    /* verilator lint_off UNUSEDSIGNAL */
    logic        ac_usb_cfg_out;
    /* verilator lint_on UNUSEDSIGNAL */
    logic        usb_irq;

    autoconfig_dev #(
        .DEV_CLASS (ACFG_CLASS_USBHC),
        .DEV_SIZE  (32'd4096),
        .DEV_ID    (32'd0),
        .DEV_NAME0 (32'h00425355)     // "USB\0" packed LE
    ) u_ac_usb (
        .i_clk       (clk),
        .i_rst       (rst),
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

    // The MAC-PHY seam between the controller and the board PHY.
    logic [7:0] usb_tx_data;
    logic       usb_tx_valid, usb_tx_ready;
    logic [7:0] usb_rx_data;
    logic       usb_rx_valid, usb_rx_active, usb_rx_error;
    logic [1:0] usb_opmode, usb_xcvr_sel;
    logic       usb_term_sel;
    logic [1:0] usb_line_state;
    logic [2:0] usb_caps;
    logic       usb_tx_dp, usb_tx_dn, usb_tx_oe;
    logic       usb_pull_dp, usb_pull_dn;
    // The PHY forwards its clock for integrations that want it; this
    // top clocks the controller from the PLL output directly.
    /* verilator lint_off UNUSEDSIGNAL */
    logic       usb_phy_clk;
    /* verilator lint_on UNUSEDSIGNAL */

    logic [15:0] usb_phy_dbg;

    usbhc u_usbhc (
        .i_clk        (clk),
        .i_rst        (rst),
        .i_addr       (usb_dev_addr),
        .i_wdata      (usb_dev_wdata),
        .i_we         (usb_dev_we),
        .i_re         (usb_dev_re),
        .o_rdata      (usb_dev_rdata),
        .o_busy       (usb_dev_busy),
        .o_irq        (usb_irq),
        .i_usb_clk    (clk_usb),
        .i_usb_rst    (rst_usb),
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

    usb_phy_ecp5 u_usb_phy (
        .i_clk        (clk_usb),
        .i_rst        (rst_usb),
        .o_clk        (usb_phy_clk),
        .i_tx_data    (usb_tx_data),
        .i_tx_valid   (usb_tx_valid),
        .o_tx_ready   (usb_tx_ready),
        .o_rx_data    (usb_rx_data),
        .o_rx_valid   (usb_rx_valid),
        .o_rx_active  (usb_rx_active),
        .o_rx_error   (usb_rx_error),
        .i_opmode     (usb_opmode),
        .i_xcvr_sel   (usb_xcvr_sel),
        .i_term_sel   (usb_term_sel),
        .o_line_state (usb_line_state),
        .o_caps       (usb_caps),
        .i_dp         (usb_fpga_bd_dp),
        .i_dn         (usb_fpga_bd_dn),
        .o_tx_dp      (usb_tx_dp),
        .o_tx_dn      (usb_tx_dn),
        .o_tx_oe      (usb_tx_oe),
        .o_pull_dp    (usb_pull_dp),
        .o_pull_dn    (usb_pull_dn),
        .o_dbg        (usb_phy_dbg)
    );

    // Pin mapping: transmit drives the bidirectional pads only while
    // the PHY owns the bus; the pull pads work the board's resistor
    // network — driven low for the host's D+/D- pull-downs, released
    // to high-Z otherwise.
    assign usb_fpga_bd_dp = usb_tx_oe ? usb_tx_dp : 1'bz;
    assign usb_fpga_bd_dn = usb_tx_oe ? usb_tx_dn : 1'bz;
    assign usb_fpga_pu_dp = usb_pull_dp ? 1'b0 : 1'bz;
    assign usb_fpga_pu_dn = usb_pull_dn ? 1'b0 : 1'bz;

    // Autoconfig combined bus signals
    logic [31:0] acfg_rdata;
    logic        acfg_busy;
    logic        acfg_sel;
    logic        acfg_sel_r;
    logic        ac_spi_sel_r, ac_usb_sel_r;
    assign acfg_rdata = (ac_spi_sel_r ? ac_spi_rdata : 32'b0) |
                        (ac_usb_sel_r ? ac_usb_rdata : 32'b0);
    assign acfg_busy  = ac_spi_busy | ac_usb_busy;
    assign acfg_sel   = ac_spi_sel | ac_usb_sel;
    always_ff @(posedge clk) begin
        ac_spi_sel_r <= ac_spi_sel;
        ac_usb_sel_r <= ac_usb_sel;
        acfg_sel_r   <= acfg_sel;
    end

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
            SYSDEV_L2_CACHE: sys_rdata = l2_rdata;
            default:      sys_rdata = 32'b0;
        endcase
    end

endmodule
