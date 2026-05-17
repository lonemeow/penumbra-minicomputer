// L2 cache phase-0 stub — literal wire-through.
//
// Sits at the same point in the memory hierarchy where the real
// `l2_cache` will eventually live (between `cpu_core.o_mem_*` and
// the shared system bus / `bus_devsel`).  This module exists to:
//
//   1. Validate the bus shape the real L2 will need — same ports,
//      same direction, same protocol on both sides — before any of
//      L2's storage or tag logic is written.
//   2. Provide an A/B target for `HAS_L2` builds: regression tests
//      passing through `l2_passthrough` confirm that the L2 layer
//      is transparent to the rest of the system, so any later
//      observable change is attributable to the real cache, not to
//      the plumbing.
//
// Phase 0 behaviour: every cpu-side signal is forwarded combinationally
// to the corresponding bus-side signal, and every bus-side response
// is forwarded back.  `i_cacheable` is consumed (the real L2 will
// use it to decide cache/install/bypass per access) but not forwarded
// downstream — the existing system bus devices don't have a
// cacheable port and ignore the bit anyway.
//
// See `doc/internals/l2-cache.md` for the design plan this module
// is the first phase of.

// verilator lint_off UNUSEDSIGNAL

module l2_passthrough
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── CPU side (upstream — from cpu_core.o_mem_*) ────────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    input  logic        i_cacheable,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Bus side (downstream — to bus_devsel / devices) ─────
    output logic [31:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,
    output logic [3:0]  o_mem_byte_en,
    output logic        o_mem_we,
    output logic        o_mem_re,
    input  logic [31:0] i_mem_rdata,
    input  logic        i_mem_busy
);

    // Pure combinational pass-through in both directions.  No
    // registers, no state — the timing/handshake the cpu_core
    // sees through l2_passthrough is identical to what it would
    // see wired directly to the bus.
    assign o_mem_addr    = i_addr;
    assign o_mem_wdata   = i_wdata;
    assign o_mem_byte_en = i_byte_en;
    assign o_mem_we      = i_we;
    assign o_mem_re      = i_re;
    assign o_rdata       = i_mem_rdata;
    assign o_busy        = i_mem_busy;

    // i_cacheable is consumed (will be used by the real L2 to
    // decide cache/bypass per access) but intentionally not
    // forwarded downstream.  Suppress the unused-input lint.
    /* verilator lint_off UNUSED */
    logic _unused_cacheable = i_cacheable;
    /* verilator lint_on UNUSED */

    // Reset/clock are tied in for the real L2's storage path; the
    // passthrough doesn't need them but they're part of the port
    // shape so the real L2 can drop in without re-wiring.
    /* verilator lint_off UNUSED */
    logic _unused_clk = i_clk;
    logic _unused_rst = i_rst;
    /* verilator lint_on UNUSED */

endmodule

// verilator lint_on UNUSEDSIGNAL
