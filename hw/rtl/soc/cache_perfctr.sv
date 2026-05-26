// Shared cache performance counters.
//
// Instantiated by every cache module that exposes the unified cache
// sysreg layout (L1 D, L1 I, L2, future L3, ...).  Four 32-bit
// free-running counters, reset to 0 on system reset.  The parent
// cache derives single-cycle event pulses from its own FSM and
// passes them in; this module owns the storage and the read-side
// of regs 10–13.
//
// Event-pulse contract — each i_event_* input must be high for
// exactly one i_clk cycle per access classified into that bucket.
// HIT/MISS classification is by tag check only, independent of
// write policy: a write-invalidate-on-hit cache, a write-back
// cache, and a write-through cache all report WRITE_HITS for the
// same set of accesses (their downstream actions differ).
//
// The parent cache is expected to gate the event pulses by
// CTRL.ENABLE and by any cacheability hint — accesses that
// pass through the cache without consulting the tag array do
// not count.  See doc/system/sysregs.md § "Performance counters"
// for the full definition.

module cache_perfctr
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // Event signals from the parent cache (single-cycle pulses)
    input  logic        i_event_read_hit,
    input  logic        i_event_read_miss,
    input  logic        i_event_write_hit,
    input  logic        i_event_write_miss,

    // Read-only sysreg interface (subset of the cache device's regs)
    input  logic [3:0]  i_sys_reg,
    output logic [31:0] o_sys_rdata
);

    logic [31:0] read_hits;
    logic [31:0] read_misses;
    logic [31:0] write_hits;
    logic [31:0] write_misses;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            read_hits    <= 32'b0;
            read_misses  <= 32'b0;
            write_hits   <= 32'b0;
            write_misses <= 32'b0;
        end else begin
            if (i_event_read_hit)    read_hits    <= read_hits    + 32'd1;
            if (i_event_read_miss)   read_misses  <= read_misses  + 32'd1;
            if (i_event_write_hit)   write_hits   <= write_hits   + 32'd1;
            if (i_event_write_miss)  write_misses <= write_misses + 32'd1;
        end
    end

    always_comb begin
        case (i_sys_reg)
            SYSREG_CACHE_READ_HITS:    o_sys_rdata = read_hits;
            SYSREG_CACHE_READ_MISSES:  o_sys_rdata = read_misses;
            SYSREG_CACHE_WRITE_HITS:   o_sys_rdata = write_hits;
            SYSREG_CACHE_WRITE_MISSES: o_sys_rdata = write_misses;
            default:                   o_sys_rdata = 32'b0;
        endcase
    end

endmodule
