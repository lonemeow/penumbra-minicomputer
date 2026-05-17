// VIPT cache test wrapper — wires cache_vipt → cpu_bus_arbiter → simple_mem
//
// Functional twin of cache_test.sv for the VIPT cache, with one
// structural difference: an interposed cpu_bus_arbiter between the
// cache and the backing simple_mem.
//
// Why the arbiter is needed even for unit testing:
//   cache_vipt and cpu_bus_arbiter share a pipelined burst-fill
//   handshake: during S_FILL, the cache holds o_mem_re high while
//   walking o_mem_addr, and the arbiter pulses i_req_accepted each
//   time it latches the cache's current address.  That handshake
//   is what lets back-to-back fill words skip the idle cycle a
//   purely busy-driven protocol would require.  simple_mem doesn't
//   produce req_accepted, doesn't hold a latched request across
//   re-deassertion, and drops busy as soon as i_re drops — so
//   wiring cache_vipt directly to simple_mem yields broken
//   handshakes (writes occasionally skipped, reads serving
//   poisoned data).  The PIPT `cache.sv` doesn't have this
//   requirement because it pulls o_mem_re from `fill_req ||
//   fill_wait` (i_re held throughout, no separate handshake).
//   The arbiter is therefore part of cache_vipt's defined
//   interface, not a test convenience.
//
// The I-port of the arbiter is tied off (no I-cache in this unit
// test) so arbitration policy is moot here — the arbiter behaves
// as a single-port request latcher.  Arbitration-policy testing
// lives in tb_cpu_bus_arbiter.
//
// Exposes a single `i_addr` port (driving both i_vaddr and i_paddr
// identically — valid because the VIPT precondition is cache ≤ page
// size, so the low bits used for index/offset are bit-identical in
// vaddr and paddr).
//
// Adds an `i_fault` port to exercise the fault-gating side of the
// contract (no fill on faulting miss, no cache update on faulting
// write-hit).  Backing memory is a small simple_mem (1024 words) with
// configurable latency.

// verilator lint_off UNUSEDSIGNAL

module cache_vipt_test
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
    // Single i_addr drives both i_vaddr and i_paddr.  Low bits
    // (index/offset) are by definition equal under the VIPT
    // precondition; high bits (tag) likewise identical for
    // identity-mapping tests.
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    input  logic        i_cacheable,
    input  logic        i_fault,
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

    // ── Cache → arbiter (D-port) ───────────────────────────
    logic [31:0] cache_mem_addr, cache_mem_wdata;
    logic [3:0]  cache_mem_byte_en;
    logic        cache_mem_we, cache_mem_re;
    /* verilator lint_off UNUSEDSIGNAL */
    logic        cache_mem_cacheable;  // forwarded but arbiter doesn't consume it here
    /* verilator lint_on UNUSEDSIGNAL */
    logic [31:0] cache_mem_rdata;
    logic        cache_mem_busy;
    logic        cache_req_accepted;

    cache_vipt #(
        .NUM_SETS   (NUM_SETS),
        .LINE_WORDS (LINE_WORDS),
        .ADDRESSING (CACHE_ADDR_VIPT)
    ) u_cache (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_vaddr      (i_addr),
        .i_paddr      (i_addr),
        .i_wdata      (i_wdata),
        .i_byte_en    (i_byte_en),
        .i_we         (i_we),
        .i_re         (i_re),
        .i_cacheable  (i_cacheable),
        .i_fault      (i_fault),
        .o_rdata      (o_rdata),
        .o_busy       (o_busy),
        .o_mem_addr   (cache_mem_addr),
        .o_mem_wdata  (cache_mem_wdata),
        .o_mem_byte_en(cache_mem_byte_en),
        .o_mem_we     (cache_mem_we),
        .o_mem_re     (cache_mem_re),
        .o_mem_cacheable (cache_mem_cacheable),
        .i_mem_rdata  (cache_mem_rdata),
        .i_mem_busy   (cache_mem_busy),
        .i_req_accepted (cache_req_accepted),
        .i_sys_reg    (i_sys_reg),
        .i_sys_wdata  (i_sys_wdata),
        .i_sys_we     (i_sys_we),
        .o_sys_rdata  (o_sys_rdata)
    );

    // ── Arbiter (single-port: I tied off) ──────────────────
    logic [31:0] arb_mem_addr, arb_mem_wdata;
    logic [3:0]  arb_mem_byte_en;
    logic        arb_mem_we, arb_mem_re;
    logic [31:0] arb_mem_rdata;
    logic        arb_mem_busy;

    // verilator lint_off PINCONNECTEMPTY
    /* verilator lint_off UNUSEDSIGNAL */
    logic [31:0] unused_i_rdata;
    logic        unused_i_busy;
    logic        unused_i_req_accepted;
    /* verilator lint_on UNUSEDSIGNAL */
    cpu_bus_arbiter u_arb (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_d_addr    (cache_mem_addr),
        .i_d_wdata   (cache_mem_wdata),
        .i_d_byte_en (cache_mem_byte_en),
        .i_d_we      (cache_mem_we),
        .i_d_re      (cache_mem_re),
        .i_d_cacheable (cache_mem_cacheable),
        .o_d_rdata   (cache_mem_rdata),
        .o_d_busy    (cache_mem_busy),
        // I-port tied off
        .i_i_addr    (32'b0),
        .i_i_re      (1'b0),
        .i_i_cacheable (1'b0),
        .o_i_rdata   (unused_i_rdata),
        .o_i_busy    (unused_i_busy),
        // D req_accepted drives the cache's burst-fill issue
        // counter; I tied off (no I-cache in this unit test).
        .o_d_req_accepted (cache_req_accepted),
        .o_i_req_accepted (unused_i_req_accepted),
        // External bus
        .o_mem_addr      (arb_mem_addr),
        .o_mem_wdata     (arb_mem_wdata),
        .o_mem_byte_en   (arb_mem_byte_en),
        .o_mem_we        (arb_mem_we),
        .o_mem_re        (arb_mem_re),
        .o_mem_cacheable (/* unused — backing simple_mem ignores it */),
        .i_mem_rdata     (arb_mem_rdata),
        .i_mem_busy      (arb_mem_busy)
    );
    // verilator lint_on PINCONNECTEMPTY

    // ── Mux between arbiter memory requests and debug writes ──
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
            mux_addr    = arb_mem_addr;
            mux_wdata   = arb_mem_wdata;
            mux_byte_en = arb_mem_byte_en;
            mux_we      = arb_mem_we;
            mux_re      = arb_mem_re;
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
        .o_rdata   (arb_mem_rdata),
        .o_busy    (arb_mem_busy)
    );

    assign o_dbg_mem_rdata = arb_mem_rdata;

endmodule

// verilator lint_on UNUSEDSIGNAL
