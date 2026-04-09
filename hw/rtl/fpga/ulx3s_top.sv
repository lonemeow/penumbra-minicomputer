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

module ulx3s_top (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    input  logic [6:0] btn,
    output logic       ftdi_rxd,    // FPGA TX → FTDI RX → host
    input  logic       ftdi_txd,    // host → FTDI TX → FPGA RX
    output logic       wifi_en      // LOW = hold ESP32 in reset
);
    import penumbra_pkg::*;

    // ── ESP32 disable ──────────────────────────────────────────
    assign wifi_en = 1'b0;

    // ── PLL: 25 MHz → 12.5 MHz system clock ──────────────────────
    // fCLKOP = fCLKI × CLKFB_DIV / CLKI_DIV = 25 × 1 / 2 = 12.5 MHz
    // fVCO   = fCLKOP × CLKOP_DIV = 12.5 × 48 = 600 MHz (range: 400-800)
    // Conservative for initial bring-up; increase once critical
    // path is optimized.
    logic clk;
    logic pll_lock;

    (* keep *) EHXPLLL #(
        .CLKI_DIV     (2),
        .CLKFB_DIV    (1),
        .CLKOP_DIV    (48),
        .CLKOP_ENABLE ("ENABLED"),
        .CLKOP_CPHASE (47),
        .CLKOP_FPHASE (0),
        .FEEDBK_PATH  ("CLKOP")
    ) u_pll (
        .CLKI         (clk_25mhz),
        .CLKFB        (clk),
        .CLKOP        (clk),
        .LOCK         (pll_lock),
        .RST          (1'b0),
        .STDBY        (1'b0),
        .PHASESEL0    (1'b0),
        .PHASESEL1    (1'b0),
        .PHASEDIR     (1'b0),
        .PHASESTEP    (1'b0),
        .PHASELOADREG (1'b0),
        .PLLWAKESYNC  (1'b0),
        .ENCLKOP      (1'b1),     // enable primary output
        .ENCLKOS      (1'b0),
        .ENCLKOS2     (1'b0),
        .ENCLKOS3     (1'b0)
    );

    // ── Reset: PLL lock + btn[1] (FIRE1) manual reset ──────────
    // Hold reset until PLL locks, then count 2^18 clocks.
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
    assign led[7:6] = '0;

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
    logic timer_irq;

    // ── Timer tick prescaler (25 MHz → 1 MHz toggle) ────────
    localparam int PRESCALE_DIV = 6;    // 12.5 MHz / (2×6) ≈ 1.04 MHz toggle
    logic [$clog2(PRESCALE_DIV)-1:0] prescale_cnt;
    logic timer_tick;

    always_ff @(posedge clk) begin
        if (rst) begin
            prescale_cnt <= '0;
            timer_tick   <= 1'b0;
        end else if (prescale_cnt == PRESCALE_DIV[4:0] - 5'd1) begin
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
        .i_irq          (uart_irq),

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
    localparam int RAM_WORDS = 65536;      // 256 KB BRAM
    localparam int ROM_WORDS = 16384;      // 64 KB boot ROM

    // ── RAM (BRAM) ──────────────────────────────────────────
    logic        ram_sel, ram_sel_r;
    logic [31:0] ram_rdata_raw;
    logic        ram_busy_raw;

    bus_devsel #(.BASE(RAM_BASE), .SIZE(32'(RAM_WORDS * 4)))
        u_ram_sel (.i_addr(mem_addr), .o_sel(ram_sel));
    always_ff @(posedge clk) ram_sel_r <= ram_sel;

    fpga_ram #(.MEM_WORDS(RAM_WORDS)) u_ram (
        .i_clk     (clk),
        .i_rst     (rst),
        .i_addr    (mem_addr),
        .i_wdata   (mem_wdata),
        .i_byte_en (mem_byte_en),
        .i_we      (mem_we & ram_sel),
        .i_re      (mem_re & ram_sel),
        .o_rdata   (ram_rdata_raw),
        .o_busy    (ram_busy_raw)
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
        .CLK_FREQ  (12_500_000),
        .BAUD_RATE (115_200)
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

    // ── Bus response OR-combine ─────────────────────────────
    assign mem_rdata = (ram_sel_r  ? ram_rdata_raw  : 32'b0) |
                       (rom_sel_r  ? rom_rdata_raw  : 32'b0) |
                       (uart_sel_r ? uart_rdata_raw : 32'b0);

    assign mem_busy = (ram_sel  ? ram_busy_raw  : 1'b0) |
                      (rom_sel  ? rom_busy_raw  : 1'b0) |
                      (uart_sel ? uart_busy_raw : 1'b0);

    assign bus_fault = (mem_re | mem_we) & ~(ram_sel | rom_sel | uart_sel);

    // ══════════════════════════════════════════════════════════
    // Sysreg devices
    // ══════════════════════════════════════════════════════════

    // ── System ID (device 1) ────────────────────────────────
    logic [31:0] sysid_rdata;

    sysid #(
        .MACH_NAME0 (32'h33584C55),   // "ULX3"
        .MACH_NAME1 (32'h00000053),   // "S\0\0\0"
        .MACH_NAME2 (32'h00000000)
    ) u_sysid (
        .i_sys_reg  (sys_reg),
        .o_sys_rdata(sysid_rdata)
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
        .TICK_FREQ_HZ (32'd1_000_000)
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
            SYSDEV_SYS:   sys_rdata = sysid_rdata;
            SYSDEV_BUS:   sys_rdata = busctl_rdata;
            SYSDEV_TIMER: sys_rdata = timer_rdata;
            default:      sys_rdata = 32'b0;
        endcase
    end

endmodule
