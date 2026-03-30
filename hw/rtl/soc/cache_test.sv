// Cache test wrapper — wires cache to simple_mem for unit testing
//
// Exposes the CPU-side cache interface and sysreg interface to the
// C++ testbench. Backing memory is a small simple_mem (1024 words)
// with configurable latency.
//
// The testbench can pre-load memory via the mem_* debug ports,
// then exercise the cache through its normal i_* ports.

// verilator lint_off UNUSEDSIGNAL

module cache_test
    import penumbra_pkg::*;
#(
    parameter NUM_SETS       = 16,
    parameter LINE_WORDS     = 4,
    parameter MEM_WORDS      = 1024,
    parameter READ_LATENCY   = 4,
    parameter WRITE_LATENCY  = 2
)
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── CPU-side cache interface ───────────────────────────
    input  logic [31:0] i_paddr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    input  logic        i_cacheable,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Sysreg interface ───────────────────────────────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    output logic [31:0] o_sys_rdata,

    // ── Debug: direct memory access for test setup ─────────
    input  logic [31:0] i_dbg_mem_addr,
    input  logic [31:0] i_dbg_mem_wdata,
    input  logic        i_dbg_mem_we,
    output logic [31:0] o_dbg_mem_rdata
);

    // ── Cache ↔ memory bus ─────────────────────────────────
    logic [31:0] mem_addr, mem_wdata;
    logic [3:0]  mem_byte_en;
    logic        mem_we, mem_re;
    logic [31:0] mem_rdata;
    logic        mem_busy;

    cache #(
        .NUM_SETS   (NUM_SETS),
        .LINE_WORDS (LINE_WORDS)
    ) u_cache (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_paddr      (i_paddr),
        .i_wdata      (i_wdata),
        .i_byte_en    (i_byte_en),
        .i_we         (i_we),
        .i_re         (i_re),
        .i_cacheable  (i_cacheable),
        .o_rdata      (o_rdata),
        .o_busy       (o_busy),
        .o_mem_addr   (mem_addr),
        .o_mem_wdata  (mem_wdata),
        .o_mem_byte_en(mem_byte_en),
        .o_mem_we     (mem_we),
        .o_mem_re     (mem_re),
        .i_mem_rdata  (mem_rdata),
        .i_mem_busy   (mem_busy),
        .i_sys_reg    (i_sys_reg),
        .i_sys_wdata  (i_sys_wdata),
        .i_sys_we     (i_sys_we),
        .o_sys_rdata  (o_sys_rdata)
    );

    // ── Mux between cache memory requests and debug writes ──
    logic [31:0] mux_addr, mux_wdata;
    logic [3:0]  mux_byte_en;
    logic        mux_we, mux_re;

    always_comb begin
        if (i_dbg_mem_we) begin
            mux_addr    = i_dbg_mem_addr;
            mux_wdata   = i_dbg_mem_wdata;
            mux_byte_en = 4'b1111;
            mux_we      = 1'b1;
            mux_re      = 1'b0;
        end else begin
            mux_addr    = mem_addr;
            mux_wdata   = mem_wdata;
            mux_byte_en = mem_byte_en;
            mux_we      = mem_we;
            mux_re      = mem_re;
        end
    end

    simple_mem #(
        .MEM_WORDS     (MEM_WORDS),
        .READ_LATENCY  (READ_LATENCY),
        .WRITE_LATENCY (WRITE_LATENCY)
    ) u_mem (
        .i_clk     (i_clk),
        .i_rst     (i_rst),
        .i_addr    (mux_addr),
        .i_wdata   (mux_wdata),
        .i_byte_en (mux_byte_en),
        .i_we      (mux_we),
        .i_re      (mux_re),
        .o_rdata   (mem_rdata),
        .o_busy    (mem_busy)
    );

    // Debug read: always available (simple_mem reads every cycle)
    assign o_dbg_mem_rdata = mem_rdata;

endmodule

// verilator lint_on UNUSEDSIGNAL
