// Penumbra Simulation Machine — cpu_core + simple_mem + sysreg devices
//
// Integration top for Verilator simulation. Wires the CPU core to
// simple synchronous memory and sysreg peripherals (sysid).
//
// The real hardware equivalent (machine_ulx3s) would replace
// simple_mem with an SDRAM controller, and add UART/timer/IRQ
// controller on the main bus or sysreg bus.

// verilator lint_off UNUSEDSIGNAL
module machine_sim
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── External interrupt ──────────────────────────────────
    input  logic        i_irq,

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

    // ══════════════════════════════════════════════════════════
    // CPU Core
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
        .i_irq          (i_irq),

        // Debug
        .o_pc           (o_pc),
        .o_halted       (o_halted),
        .i_dbg_reg_addr (i_dbg_reg_addr),
        .o_dbg_reg_data (o_dbg_reg_data)
    );

    // ══════════════════════════════════════════════════════════
    // Simple synchronous memory (simulation backing store)
    // ══════════════════════════════════════════════════════════
    simple_mem #(.MEM_WORDS(4 * 1024 * 1024)) u_mem (   // 16 MB
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_addr    (mem_addr),
        .i_wdata   (mem_wdata),
        .i_byte_en (mem_byte_en),
        .i_we      (mem_we),
        .i_re      (mem_re),
        .o_rdata   (mem_rdata),
        .o_busy    (mem_busy)
    );

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
