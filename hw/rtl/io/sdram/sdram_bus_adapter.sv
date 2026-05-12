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
//   ADP_WAIT_RSP   → real READ request accepted, waiting for the
//                    controller's read response (rsp_valid)
//   ADP_PRESENT    → drop o_busy, present rdata for one cycle so the
//                    cache can sample it
//
// Writes never enter ADP_WAIT_RSP — once the CDC accepts a write into
// its slot, ordering is locked (CDC FIFO + serial controller), so any
// later read or write at the same address will land after this write.
// We can release the cache the moment `req_ready` pulses by going
// straight from BEGIN to PRESENT.  The eventual `i_done` still pops
// the tag FIFO via the always-on response-handling logic; the cache-
// facing FSM doesn't need to be involved.
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

    // Latched read response.  Writes don't need a latched payload
    // because they're released to the cache the moment the CDC accepts
    // them — no per-transaction state needed past BEGIN.
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
                // Concurrent pop + push — count stays the same, but
                // the update differs by current tag_count because of
                // the FIFO's "valid entries fill from slot[0] upward"
                // convention (see push-only branch below and the
                // tag_fifo invariant SVAs at end-of-module):
                //
                //   tag_count == 2: slot[1] holds a valid tag.  Pop
                //     consumes slot[0]; slot[1] shifts to slot[0] to
                //     become the new head; the new push lands at
                //     slot[1] as the new tail.
                //
                //   tag_count == 1: slot[1] is the cleared-to-0
                //     sentinel from a previous pop, NOT a valid tag.
                //     Shifting it into slot[0] would zero the new
                //     head and strand the real entry at slot[1] —
                //     response routing reads slot[0] and would
                //     misclassify the next response.  The new push
                //     must go directly to slot[0] as the new (and
                //     only) entry.
                if (tag_count == 2'd1) begin
                    tag_fifo[0] <= next_tag;
                    tag_fifo[1] <= 1'b0;
                end else begin
                    tag_fifo[0] <= tag_fifo[1];
                    tag_fifo[1] <= next_tag;
                end
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
                        if (i_re && !spec_pending_push &&
                            !spec_in_flight && !spec_buffered) begin
                            spec_pending_push <= 1'b1;
                            spec_push_addr    <= i_addr + 32'd4;
                        end
                        // Reads wait for response data; writes are
                        // fire-and-forget through the CDC FIFO since
                        // ordering is preserved downstream.
                        state <= i_we ? ADP_PRESENT : ADP_WAIT_RSP;
                    end
                end

                ADP_WAIT_RSP: begin
                    // Reads only.  Wait for the response routed to
                    // cache (head tag = 0); writes don't visit this
                    // state — they go BEGIN→PRESENT directly.
                    if (i_rsp_valid && tag_fifo[0] == 1'b0)
                        state <= ADP_PRESENT;
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

    // ══════════════════════════════════════════════════════════
    // Simulation assertions — tag FIFO data-correctness invariants
    //
    // The FIFO has three valid configurations (0, 1, or 2 entries).
    // FSM/handshake correctness — push and pop occur only when valid,
    // count never goes out of range — is straightforward to inspect,
    // but the *data* contract (which slot is the head, what value
    // unused slots hold, response routing always sees a valid tag at
    // the head) is more subtle and easy to violate during refactors
    // of the pop/push branches.  These assertions encode that contract
    // so a violation fires at the first failing cycle rather than
    // surfacing as silent data corruption downstream.  Stripped by
    // Yosys at synth.
    // ══════════════════════════════════════════════════════════

    // Count never exceeds the FIFO depth.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (tag_count <= 2'd2))
        else $error("sdram_bus_adapter: tag_count > 2 (overflow)");

    // Slot-fill convention: entries fill from slot[0] upward.  When
    // tag_count<2, slot[1] must be cleared (sentinel 0).  When
    // tag_count==0, slot[0] must also be cleared.  Catches "a valid
    // entry got stranded above the head" — any pop/push code path
    // that leaves the FIFO in a state where the head doesn't actually
    // hold the next-out tag.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (tag_count < 2'd2) |-> (tag_fifo[1] == 1'b0))
        else $error("sdram_bus_adapter: tag_fifo[1] non-zero with tag_count<2 — head/tail desync");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (tag_count == 2'd0) |-> (tag_fifo[0] == 1'b0))
        else $error("sdram_bus_adapter: tag_fifo[0] non-zero with tag_count==0");

    // Responses only arrive against outstanding requests.  A response
    // with tag_count==0 would route on a stale tag_fifo[0] and silently
    // corrupt either rdata_latched or spec_data.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_rsp_valid |-> (tag_count > 2'd0))
        else $error("sdram_bus_adapter: response arrived with no outstanding tag");

    // Push never overflows; pop never underflows.  Encoded
    // combinationally in pop_event/push_event already, but asserting
    // at the sequential level too prevents a future refactor of those
    // wires from silently breaking the contract.
    assert property (@(posedge i_clk) disable iff (i_rst)
        push_event |-> (tag_count < 2'd2 || pop_event))
        else $error("sdram_bus_adapter: push when tag_count==2 without concurrent pop");
    assert property (@(posedge i_clk) disable iff (i_rst)
        pop_event |-> (tag_count > 2'd0))
        else $error("sdram_bus_adapter: pop when tag_count==0");

endmodule
