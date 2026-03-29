// Penumbra Simulation Machine — cpu_core + RAM + boot ROM + UART + sysreg devices
//
// Integration top for Verilator simulation. Wires the CPU core to
// simple synchronous memory, boot ROM, a 16450-compatible simulation
// UART, and sysreg peripherals (sysid).
//
// Address decode matches the physical memory map:
//   0x0000_0000 – 0x01FF_FFFF   RAM (16 MB, wrapping)
//   0xFF00_0000 – 0xFF00_0FFF   UART (4 KB page, MMIO)
//   0xFFFF_E000 – 0xFFFF_FFFF   Boot ROM (8 KB)
//   everything else              → RAM (wraps, no bus fault in sim)
//
// CPU boots from ROM at 0xFFFF_E000 (default RESET_PC), same as
// real hardware. Programs are assembled with --org 0xFFFFE000.
//
// The real hardware equivalent (machine_ulx3s) would replace
// simple_mem with an SDRAM controller, boot_rom with real flash,
// sim_uart with a baud-rate UART, and add timer/IRQ controller.

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

    // ── Debug / observation ports ────────────────────────────
    output logic [31:0] o_pc,
    output logic        o_halted,
    input  logic [3:0]  i_dbg_reg_addr,
    output logic [31:0] o_dbg_reg_data
);

    // ── CPU ↔ memory bus ────────────────────────────────────
    logic [31:0] mem_addr, mem_wdata;
    logic [3:0]  mem_byte_en;
    logic        mem_we, mem_re;
    logic [31:0] mem_rdata;
    logic        mem_busy;

    // ── CPU ↔ sysreg bus ────────────────────────────────────
    logic [3:0]  sys_dev, sys_reg;
    logic [31:0] sys_wdata;
    logic        sys_cycle, sys_we;
    logic [31:0] sys_rdata;

    // ── Internal IRQ (UART IRQ OR external testbench IRQ) ───
    logic uart_irq;
    logic combined_irq;
    assign combined_irq = i_irq | uart_irq;

    // ══════════════════════════════════════════════════════════
    // CPU Core (default RESET_PC = 0xFFFF_E000)
    // ══════════════════════════════════════════════════════════
    cpu_core u_cpu (
        .i_clk          (i_clk),
        .i_rst          (i_rst),

        // Memory bus
        .o_mem_addr     (mem_addr),
        .o_mem_wdata    (mem_wdata),
        .o_mem_byte_en  (mem_byte_en),
        .o_mem_we       (mem_we),
        .o_mem_re       (mem_re),
        .i_mem_rdata    (mem_rdata),
        .i_mem_busy     (mem_busy),

        // Sysreg bus (external devices)
        .o_sys_dev      (sys_dev),
        .o_sys_reg      (sys_reg),
        .o_sys_wdata    (sys_wdata),
        .o_sys_cycle    (sys_cycle),
        .o_sys_we       (sys_we),
        .i_sys_rdata    (sys_rdata),

        // Interrupt
        .i_irq          (combined_irq),

        // Debug
        .o_pc           (o_pc),
        .o_halted       (o_halted),
        .i_dbg_reg_addr (i_dbg_reg_addr),
        .o_dbg_reg_data (o_dbg_reg_data)
    );

    // ══════════════════════════════════════════════════════════
    // Address decode
    // ══════════════════════════════════════════════════════════
    //   sel_rom:  0xFFFF_E000 – 0xFFFF_FFFF  (8 KB boot ROM)
    //   sel_uart: 0xFF00_0000 – 0xFF00_0FFF  (4 KB UART page)
    //   sel_ram:  everything else (wraps in simple_mem)
    logic sel_rom, sel_uart, sel_ram;

    assign sel_rom  = (mem_addr[31:13] == 19'h7FFFF);       // 0xFFFF_Exxx
    assign sel_uart = (mem_addr[31:12] == 20'hFF000);       // 0xFF00_0xxx
    assign sel_ram  = ~sel_rom & ~sel_uart;

    // Gate enables to the selected device
    logic ram_re, ram_we, rom_re, uart_re, uart_we;
    assign ram_re  = mem_re & sel_ram;
    assign ram_we  = mem_we & sel_ram;
    assign rom_re  = mem_re & sel_rom;
    assign uart_re = mem_re & sel_uart;
    assign uart_we = mem_we & sel_uart;

    // ══════════════════════════════════════════════════════════
    // RAM (16 MB at 0x0000_0000)
    // ══════════════════════════════════════════════════════════
    logic [31:0] ram_rdata;
    logic        ram_busy;

    simple_mem #(.MEM_WORDS(4 * 1024 * 1024)) u_ram (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_addr    (mem_addr),
        .i_wdata   (mem_wdata),
        .i_byte_en (mem_byte_en),
        .i_we      (ram_we),
        .i_re      (ram_re),
        .o_rdata   (ram_rdata),
        .o_busy    (ram_busy)
    );

    // ══════════════════════════════════════════════════════════
    // Boot ROM (8 KB at 0xFFFF_E000)
    // ══════════════════════════════════════════════════════════
    logic [31:0] rom_rdata;
    logic        rom_busy;

    boot_rom #(.ROM_WORDS(2048)) u_rom (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_addr    (mem_addr),
        .i_re      (rom_re),
        .o_rdata   (rom_rdata),
        .o_busy    (rom_busy)
    );

    // ══════════════════════════════════════════════════════════
    // UART (4 KB at 0xFF00_0000, MMIO)
    // ══════════════════════════════════════════════════════════
    logic [31:0] uart_rdata;
    logic        uart_busy;

    sim_uart u_uart (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_addr      (mem_addr),
        .i_wdata     (mem_wdata),
        .i_we        (uart_we),
        .i_re        (uart_re),
        .o_rdata     (uart_rdata),
        .o_busy      (uart_busy),
        .o_tx_valid  (o_uart_tx_valid),
        .o_tx_data   (o_uart_tx_data),
        .i_rx_valid  (i_uart_rx_valid),
        .i_rx_data   (i_uart_rx_data),
        .o_rx_ack    (o_uart_rx_ack),
        .o_irq       (uart_irq)
    );

    // ── Memory read data mux ────────────────────────────────
    // Registered select tracks which device was addressed on
    // the previous cycle (when the read data becomes valid).
    logic sel_rom_r, sel_uart_r;

    always_ff @(posedge i_clk) begin
        sel_rom_r  <= sel_rom;
        sel_uart_r <= sel_uart;
    end

    always_comb begin
        if (sel_rom_r)
            mem_rdata = rom_rdata;
        else if (sel_uart_r)
            mem_rdata = uart_rdata;
        else
            mem_rdata = ram_rdata;
    end

    // ── Busy mux (combinational — current cycle) ────────────
    always_comb begin
        if (sel_rom)
            mem_busy = rom_busy;
        else if (sel_uart)
            mem_busy = uart_busy;
        else
            mem_busy = ram_busy;
    end

    // ══════════════════════════════════════════════════════════
    // Sysreg devices (external, dev_id >= 1)
    // ══════════════════════════════════════════════════════════

    // ── System ID (device 1, read-only) ──────────────────────
    logic [31:0] sysid_rdata;

    sysid u_sysid (
        .i_sys_reg  (sys_reg),
        .o_sys_rdata(sysid_rdata)
    );

    // ── Sysreg read mux ─────────────────────────────────────
    // Routes read data from external devices back to cpu_core.
    // Device 0 (MMU) is handled inside cpu_core.
    always_comb begin
        case (sys_dev)
            SYSDEV_SYS: sys_rdata = sysid_rdata;
            default:    sys_rdata = 32'b0;
        endcase
    end

endmodule
// verilator lint_on UNUSEDSIGNAL
