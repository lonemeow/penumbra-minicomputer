// SDRAM bus adapter — speculative-prefetch version (Stage B).
//
// Translates the project's standard synchronous device-bus interface
// (i_re/i_we/o_busy/o_rdata, used by simple_mem, fpga_ram, boot_rom)
// into the controller's req_valid/req_ready/rsp_valid handshake.
// Drop-in replacement for `simple_mem` at the SDRAM region.
//
// On every accepted read request, the adapter speculatively pushes a
// second request for `i_addr + 4` to the CDC.  The depth-2 CDC carries
// both the real and the speculation in flight simultaneously.  When
// the cache later asks for that next address (the typical case during
// a 4-word fill), the speculation buffer satisfies it without a
// fresh CDC round-trip — hiding most of the bus-traversal latency that
// dominates miss cost on the current architecture.
//
// Speculation policy (simple version):
//   • Always prefetch addr+4 when a real read is accepted, IF no spec
//     is already pending/in-flight/buffered.
//   • Whenever the cache consumes a spec result (buffered hit, or
//     in-flight hit that ultimately reads from buffer), queue a new
//     spec for the *next* word — keeps the chain rolling so a single
//     fill never has to wait for an empty buffer after the first word.
//   • Reads only.  Writes don't speculate.  Mispredicts (cache asks
//     for an address that doesn't match the spec) abandon the in-flight
//     spec — its response is discarded when it arrives — and push the
//     new real request normally.
//
// Response routing — 1-bit tag FIFO (depth 2):
//   • Each push to the CDC also pushes a tag: 0 = real (for the cache),
//     1 = spec (for the buffer/discard).
//   • Each rsp_valid pulse pops the head tag; rsp_data routes accordingly.
//   • Order is preserved end-to-end because the CDC's SD side processes
//     requests serially in push order.
//
// In-flight hit handling (cache asks for spec_addr while spec is still
// in flight): adapter just stalls in BEGIN.  When the spec response
// arrives, the response handler routes it to the buffer.  Next cycle,
// BEGIN sees `spec_hit_buffered` and presents.  Costs one extra sys
// cycle vs. forwarding the response combinationally to the cache, but
// avoids a class of same-cycle race conditions between the BEGIN
// "claim" and the response handler "buffer" updates.
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
//   ADP_BEGIN      → cache request live; deciding whether to consume
//                    spec buffer, wait for in-flight spec, or push real
//   ADP_WAIT_RSP   → real request accepted (payload latched), waiting
//                    for the controller's response (rsp_valid/done)
//   ADP_PRESENT    → drop o_busy, present rdata for one cycle so the
//                    cache can sample it
//
// Speculation push happens combinationally whenever `spec_pending_push`
// is set and we're not currently driving a real request — the same
// `o_req_valid` wires carry either real or spec, gated by state.

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
        ADP_WAIT_RSP,
        ADP_PRESENT
    } state_t;

    state_t state;

    // Latched real-request bookkeeping.  cache_we_r distinguishes
    // read/write completion paths in WAIT_RSP.
    logic        cache_we_r;
    logic [31:0] rdata_latched;

    // Speculation state
    logic        spec_in_flight;     // pushed to CDC, response not yet back
    logic        spec_buffered;      // response back, in buffer
    logic [31:0] spec_addr;          // address tracked by the spec (in flight or buffered)
    logic [31:0] spec_data;          // buffered spec response data
    logic        spec_abandoned;     // discard the next spec response when it arrives

    logic        spec_pending_push;  // we want to push spec_push_addr to CDC next chance
    logic [31:0] spec_push_addr;

    // Tag FIFO (depth 2): bit 0 = head (next response routes here),
    // bit 1 = next in line.  Tag 0 = real, 1 = spec.
    logic [1:0]  tag_fifo;
    logic [1:0]  tag_count;

    // ── Hit detection ──
    wire spec_hit_buffered  = i_re && spec_buffered  && (i_addr == spec_addr);
    wire spec_hit_in_flight = i_re && spec_in_flight && (i_addr == spec_addr) && !spec_abandoned;

    // ── Combinational request drive ──
    // Real push: only when we're in BEGIN, the cache is asking, and
    // we're not waiting on a spec hit (which would mean don't push,
    // the spec covers it).
    wire pushing_real =
        (state == ADP_BEGIN) && (i_re || i_we) &&
        !spec_hit_buffered && !spec_hit_in_flight;

    // Spec push: combinationally driven whenever a push is queued AND
    // we're not currently driving a real on the same wires.
    wire pushing_spec = !pushing_real && spec_pending_push;

    assign o_req_valid   = pushing_real || pushing_spec;
    assign o_req_we      = pushing_real ? i_we      : 1'b0;
    assign o_req_addr    = pushing_real ? i_addr    : spec_push_addr;
    assign o_req_wdata   = pushing_real ? i_wdata   : 32'b0;
    assign o_req_byte_en = pushing_real ? i_byte_en : 4'hF;
    assign o_rsp_ready   = 1'b1;
    assign o_rdata       = rdata_latched;

    // o_busy: high when a transaction is mid-flight or queued for push.
    assign o_busy =
        (state == ADP_BEGIN) || (state == ADP_WAIT_RSP) ||
        ((state == ADP_IDLE) && (i_re || i_we));

    // ── Tag FIFO push/pop signals ──
    wire pop_event       = (i_rsp_valid || (i_done && !i_rsp_valid)) && (tag_count > 0);
    wire push_real_event = pushing_real && i_req_ready;
    wire push_spec_event = pushing_spec && i_req_ready;
    wire push_event      = push_real_event || push_spec_event;
    wire next_tag        = push_spec_event;   // 0 = real, 1 = spec

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state              <= ADP_IDLE;
            cache_we_r         <= 1'b0;
            rdata_latched      <= '0;
            spec_in_flight     <= 1'b0;
            spec_buffered      <= 1'b0;
            spec_addr          <= '0;
            spec_data          <= '0;
            spec_abandoned     <= 1'b0;
            spec_pending_push  <= 1'b0;
            spec_push_addr     <= '0;
            tag_fifo           <= '0;
            tag_count          <= '0;
        end else begin
            // ── Tag FIFO: combined push/pop in one update ──
            if (pop_event && push_event) begin
                // Pop and push same cycle — count stays, head shifts in
                // tag_fifo[1], new tail is next_tag.
                tag_fifo[0] <= tag_fifo[1];
                tag_fifo[1] <= next_tag;
            end else if (pop_event) begin
                tag_fifo[0] <= tag_fifo[1];
                tag_fifo[1] <= 1'b0;
                tag_count   <= tag_count - 2'd1;
            end else if (push_event) begin
                if (tag_count == 2'd0) tag_fifo[0] <= next_tag;
                else                   tag_fifo[1] <= next_tag;
                tag_count <= tag_count + 2'd1;
            end

            // ── Response routing ──
            if (i_rsp_valid && tag_count > 0) begin
                if (tag_fifo[0] == 1'b0) begin
                    // Real (cache-pending) read response
                    rdata_latched <= i_rsp_data;
                end else begin
                    // Spec response — buffer or discard
                    if (spec_abandoned) begin
                        spec_abandoned <= 1'b0;
                    end else begin
                        spec_data     <= i_rsp_data;
                        spec_buffered <= 1'b1;
                    end
                    spec_in_flight <= 1'b0;
                end
            end

            // ── Spec push completion ──
            if (push_spec_event) begin
                spec_pending_push <= 1'b0;
                spec_in_flight    <= 1'b1;
                spec_addr         <= spec_push_addr;
            end

            // ── Cache-facing FSM ──
            case (state)
                ADP_IDLE: begin
                    if (i_re || i_we) state <= ADP_BEGIN;
                end

                ADP_BEGIN: begin
                    if (!(i_re || i_we)) begin
                        // Cache backed off (rare).  Drop back to idle.
                        state <= ADP_IDLE;
                    end else if (spec_hit_buffered) begin
                        // Buffered hit — present immediately and queue
                        // the next spec to keep the chain rolling.
                        rdata_latched     <= spec_data;
                        spec_buffered     <= 1'b0;
                        spec_pending_push <= 1'b1;
                        spec_push_addr    <= i_addr + 32'd4;
                        state             <= ADP_PRESENT;
                    end else if (spec_hit_in_flight) begin
                        // Spec is in flight for this exact address.  Stay
                        // in BEGIN; o_busy stays high.  When the response
                        // arrives, the response handler buffers it; next
                        // cycle BEGIN takes the spec_hit_buffered branch.
                    end else if (push_real_event) begin
                        // Real push accepted.  Any pending spec is
                        // unusable for the new request: this branch
                        // is only reached on (a) the initial miss
                        // when no spec exists, (b) a mispredicted
                        // address, or (c) a write that may alias the
                        // spec read.  Cache fills go through the
                        // spec_hit branches, never here, so this
                        // unconditional abandonment costs no chain.
                        if (spec_in_flight) spec_abandoned <= 1'b1;
                        if (spec_buffered)  spec_buffered  <= 1'b0;
                        cache_we_r <= i_we;
                        if (i_re && !spec_pending_push &&
                            !spec_in_flight && !spec_buffered) begin
                            spec_pending_push <= 1'b1;
                            spec_push_addr    <= i_addr + 32'd4;
                        end
                        state <= ADP_WAIT_RSP;
                    end
                end

                ADP_WAIT_RSP: begin
                    // Wait for the response routed to cache (head tag = 0).
                    if (!cache_we_r && i_rsp_valid && tag_fifo[0] == 1'b0) begin
                        state <= ADP_PRESENT;
                    end else if (cache_we_r && i_done && tag_fifo[0] == 1'b0) begin
                        state <= ADP_PRESENT;
                    end
                end

                ADP_PRESENT: begin
                    // PRESENT→BEGIN fast path retained from Stage A.
                    if (i_re || i_we) state <= ADP_BEGIN;
                    else              state <= ADP_IDLE;
                end

                default: state <= ADP_IDLE;
            endcase
        end
    end

endmodule
