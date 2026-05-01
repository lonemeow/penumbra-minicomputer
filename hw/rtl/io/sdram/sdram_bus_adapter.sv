// SDRAM bus adapter — synchronous bus ↔ controller req/rsp.
//
// Translates the project's standard synchronous device-bus interface
// (i_re/i_we/o_busy/o_rdata, used by simple_mem, fpga_ram, boot_rom)
// into the controller's req_valid/req_ready/rsp_valid handshake.
// Drop-in replacement for `simple_mem` at the SDRAM region.
//
// Single-word transactions, but pipelined across cache line fills:
// when the cache holds `i_re` continuously across a 4-word fill (per
// doc/hardware/bus-protocol.md § Burst Transfers), the adapter takes
// the PRESENT → BEGIN fast path instead of cycling back through IDLE.
// Each fill word still issues its own req/rsp pair to the controller
// — the win is just one fewer CPU cycle of FSM dwell between words.
//
// o_busy contract (per `hw/CLAUDE.md` § Memory Access):
//   • o_busy is high while a request is being served.
//   • o_busy drops on the *same* cycle that o_rdata is valid (reads)
//     or that the write data has been committed (writes).
//   • The cache stalls while o_busy is high; on the cycle it goes
//     low, the cache latches o_rdata and de-asserts i_re/i_we.
//
// State machine:
//   ADP_IDLE       → no transaction in flight; o_busy = (i_re|i_we)
//   ADP_BEGIN      → driving req_valid (with request fields routed
//                    combinationally from i_*, since the cache holds
//                    them stable while o_busy is high), waiting for
//                    the controller's req_ready acceptance pulse
//   ADP_WAIT_DONE  → request accepted (payload latched into req_*_r),
//                    waiting for the controller's o_done pulse (and
//                    rsp_valid for reads)
//   ADP_PRESENT    → drop o_busy, present rdata for one cycle so
//                    the cache can sample it
//
// Burst fast path: from ADP_PRESENT, if `i_re` (or `i_we`) is still
// asserted, jump straight to ADP_BEGIN on the next cycle instead of
// passing through ADP_IDLE.  By that cycle the cache has already
// advanced its `fill_count` and `i_addr` is pointing at the next
// word, so BEGIN can drive `o_req_valid` immediately.  This shaves
// one CPU cycle per inter-word transition during a cache fill.
//
// The PRESENT state still adds one cycle of latency between the
// controller's o_done and the cache observing !o_busy.  Acceptable
// overhead and keeps the data path purely registered.

module sdram_bus_adapter (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Cache-facing bus ─────────────────────────────────────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Controller-facing handshake ──────────────────────────
    output logic        o_req_valid,
    output logic        o_req_we,
    output logic [31:0] o_req_addr,
    output logic [31:0] o_req_wdata,
    output logic [3:0]  o_req_byte_en,
    input  logic        i_req_ready,

    input  logic        i_rsp_valid,
    input  logic [31:0] i_rsp_data,
    output logic        o_rsp_ready,

    input  logic        i_done
);

    typedef enum logic [1:0] {
        ADP_IDLE,
        ADP_BEGIN,
        ADP_WAIT_DONE,
        ADP_PRESENT
    } state_t;

    state_t state;

    // Latched payload — captured at BEGIN→WAIT_DONE so the response
    // phase has stable values once `i_*` is no longer guaranteed
    // stable (the cache may de-assert i_re after PRESENT).
    logic        req_we_r;
    logic [31:0] req_addr_r;
    logic [31:0] req_wdata_r;
    logic [3:0]  req_byte_en_r;
    logic [31:0] rdata_latched;

    // Drive request fields directly from the cache's combinational
    // outputs while in BEGIN — the cache holds them stable as long
    // as o_busy is high, so this is safe and avoids spending an
    // extra cycle in IDLE just to latch.  After the controller
    // accepts (i_req_ready), we latch into req_*_r for the response
    // phase to use.
    //
    // o_req_valid is gated on the cache currently holding `i_re`
    // (or `i_we`) high.  This matters for the PRESENT→BEGIN shortcut:
    // PRESENT decides at its edge based on cycle-X's `i_re`, but on
    // the *last* word of a cache fill the cache de-asserts `o_mem_re`
    // the next cycle (no more `fill_req` to follow), and we must not
    // issue a phantom request to the controller in that case.  BEGIN
    // aborts cleanly back to IDLE below.
    assign o_req_valid   = (state == ADP_BEGIN) && (i_re || i_we);
    assign o_req_we      = (state == ADP_BEGIN) ? i_we      : req_we_r;
    assign o_req_addr    = (state == ADP_BEGIN) ? i_addr    : req_addr_r;
    assign o_req_wdata   = (state == ADP_BEGIN) ? i_wdata   : req_wdata_r;
    assign o_req_byte_en = (state == ADP_BEGIN) ? i_byte_en : req_byte_en_r;
    assign o_rsp_ready   = 1'b1;            // always ready to consume
    assign o_rdata       = rdata_latched;

    // o_busy contract: high whenever a transaction is in progress, OR
    // when the cache is asking but we haven't yet accepted it.  Low
    // only in PRESENT (data being served) or in IDLE without an
    // incoming request.
    assign o_busy =
        (state == ADP_BEGIN) || (state == ADP_WAIT_DONE) ||
        ((state == ADP_IDLE) && (i_re || i_we));

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state          <= ADP_IDLE;
            req_we_r       <= 1'b0;
            req_addr_r     <= '0;
            req_wdata_r    <= '0;
            req_byte_en_r  <= '0;
            rdata_latched  <= '0;
        end else begin
            case (state)
                ADP_IDLE: begin
                    if (i_re || i_we) state <= ADP_BEGIN;
                end

                ADP_BEGIN: begin
                    if (!(i_re || i_we)) begin
                        // Cache de-asserted re/we before the controller
                        // accepted the request.  This happens at the
                        // tail of a cache line fill: PRESENT→BEGIN
                        // shortcut fired because i_re was high in
                        // PRESENT, but on the last word the cache
                        // drops o_mem_re the next cycle.  o_req_valid
                        // is gated above so the controller never saw
                        // anything; just go back to IDLE.
                        state <= ADP_IDLE;
                    end else if (i_req_ready) begin
                        // Latch the payload now that the controller
                        // has accepted it.  i_* may stop being
                        // meaningful after PRESENT, so the response
                        // phase reads from the latched copies.
                        req_we_r       <= i_we;
                        req_addr_r     <= i_addr;
                        req_wdata_r    <= i_wdata;
                        req_byte_en_r  <= i_byte_en;
                        state          <= ADP_WAIT_DONE;
                    end
                end

                ADP_WAIT_DONE: begin
                    // For reads, rsp_valid and done coincide.  For
                    // writes there's no rsp_valid — only done.
                    if (!req_we_r && i_rsp_valid) begin
                        rdata_latched <= i_rsp_data;
                        state         <= ADP_PRESENT;
                    end else if (req_we_r && i_done) begin
                        state <= ADP_PRESENT;
                    end
                end

                ADP_PRESENT: begin
                    // PRESENT→BEGIN fast path: when the cache is
                    // already asking for the next word (i_re held
                    // across a line fill), skip ADP_IDLE and start
                    // the new request the very next cycle.  By this
                    // cycle the cache has advanced fill_count and
                    // i_addr points at the next word.
                    if (i_re || i_we) begin
                        state <= ADP_BEGIN;
                    end else begin
                        state <= ADP_IDLE;
                    end
                end

                default: state <= ADP_IDLE;
            endcase
        end
    end

endmodule
