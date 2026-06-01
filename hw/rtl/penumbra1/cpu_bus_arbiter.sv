// Penumbra CPU bus arbiter — gives icache and dcache private bus ports
//
// The CPU has two memory clients (split I/D L1) but only one external
// bus.  The naive solution is a combinational mux of addr/we/re plus
// an OR of `o_busy` aggregation, but that creates a structural cross-
// dependency where dcache's bus traffic fans into icache's `i_mem_busy`
// (and vice versa) through device-decode and busy-aggregation logic.
// Synthesis closes for the worst-case path through *both* caches even
// when only one is active.  That cross-coupling pinned the CPU's
// critical path between the two caches.
//
// This arbiter eliminates the cross-coupling structurally.  Each cache
// gets a private (i_re, i_we, addr, wdata) → (busy, rdata) port.  The
// arbiter holds a small FSM (IDLE/BUSY) and *registers* the outgoing
// request (addr/wdata/we) at the latch event.  Per-port busy is driven
// from arbiter state plus the relevant cache's pending bit, so each
// cache sees a busy net that does not depend on the other cache's
// traffic.
//
// Arbitration policy: dcache priority.  Fetch (S_FETCH) and data
// access (S_EXEC) are mutually exclusive at the CPU level, so the
// priority is a tiebreaker — only relevant during a corner-case
// overlap (e.g. a fetch refill straddling a data access).
//
// FSM: two states, S_IDLE and S_BUSY.  A "latch event" — where the
// arbiter captures a new request into the bus-side registers — happens
// on either:
//   - S_IDLE → S_BUSY: a new owner is being picked (from idle).
//   - S_BUSY → S_BUSY: the current transaction completed
//     (i_mem_busy=0) and a request is still pending — could be the
//     same owner continuing a burst, or the other owner taking over.
// Latch events drive the `o_*_req_accepted` handshake pulses, which
// the downstream cache uses to know its address has been captured and
// can advance to the next address without waiting for busy to drop.
// This eliminates the dead cycle between back-to-back transactions
// that earlier versions of this arbiter required (S_DONE cooldown),
// matching the four-phase handshake's "keep req high across addr
// changes" pattern in doc/hardware/bus-protocol.md.
//
// Costs:
//   - Zero idle bus cycles between back-to-back transactions (same
//     owner burst or alternating owner handoff).  Per-word cost
//     equals slave latency.
//   - Single-cycle pick from IDLE for a new transaction.
//   - Zero cycles on cached hits — they never reach the arbiter.
//
// Response-routing contract:
//   The arbiter latches `owner` on every latch event.  Owner is
//   stable mid-transaction (S_BUSY with i_mem_busy=1).  Owner may
//   change at a S_BUSY→S_BUSY edge (handoff to other cache, or burst
//   continuation by same cache).
//
//   Read data is a combinational pass-through: o_d_rdata = o_i_rdata
//   = i_mem_rdata.  Both caches see the same i_mem_rdata, but each is
//   gated by its own busy signal — only the current owner sees its
//   busy drop on the response cycle (the cycle i_mem_busy=0 in
//   S_BUSY).  The non-owner with a pending request sees busy=1
//   throughout (driven by its own pending bit), so its cache cannot
//   mis-capture.  The owner/non-owner SVAs at the bottom of the file
//   pin this down.
//
//   The contract trusts the external bus to honour "`i_mem_rdata` on
//   the cycle `i_mem_busy=0` after `o_mem_re`/`o_mem_we` was driven
//   in S_BUSY is the response to that request."  Violating that on
//   the device side (spurious data on `i_mem_rdata` while
//   `i_mem_busy=0` outside the paired window) will be captured by
//   whichever cache is the current owner — the arbiter has no
//   separate response tag.

// verilator lint_off UNUSEDSIGNAL

module cpu_bus_arbiter
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Port D: dcache (priority, full read/write) ─────────
    input  logic [31:0] i_d_addr,
    input  logic [31:0] i_d_wdata,
    input  logic [3:0]  i_d_byte_en,
    input  logic        i_d_we,
    input  logic        i_d_re,
    input  logic        i_d_cacheable,
    output logic [31:0] o_d_rdata,
    output logic        o_d_busy,

    // ── Port I: icache (read-only fetch) ───────────────────
    input  logic [31:0] i_i_addr,
    input  logic        i_i_re,
    input  logic        i_i_cacheable,
    output logic [31:0] o_i_rdata,
    output logic        o_i_busy,

    // ── Request-accepted handshake (1-cycle combinational pulse) ──
    // Each pulse fires in the cycle the arbiter is about to latch
    // that owner's request on the next clock edge — either on entry
    // to S_BUSY from S_IDLE (new transaction sequence) or on the
    // busy-drop cycle while already in S_BUSY (back-to-back).
    // Downstream caches use these to advance to the next address
    // (in a burst fill) or drop o_mem_re (single-shot) without
    // waiting for busy to drop.  Mutually exclusive (D wins
    // simultaneous pending).
    output logic        o_d_req_accepted,
    output logic        o_i_req_accepted,

    // ── External bus (single, shared with rest of system) ──
    output logic [31:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,
    output logic [3:0]  o_mem_byte_en,
    output logic        o_mem_we,
    output logic        o_mem_re,
    // o_mem_cacheable: forwarded from whichever cache port is the
    // current owner.  Drives the future L2's cache/bypass
    // decision; current external devices ignore it.
    output logic        o_mem_cacheable,
    input  logic [31:0] i_mem_rdata,
    input  logic        i_mem_busy
);

    // ══════════════════════════════════════════════════════════
    // FSM — IDLE → BUSY (BUSY can loop on back-to-back) → IDLE
    // ══════════════════════════════════════════════════════════
    typedef enum logic {
        S_IDLE,
        S_BUSY
    } state_t;

    state_t state, state_n;

    // Latched request — driven onto the external bus during S_BUSY.
    logic [31:0] req_addr;
    logic [31:0] req_wdata;
    logic [3:0]  req_byte_en;
    logic        req_we;
    logic        req_re;
    logic        req_cacheable;
    logic        owner;        // 0 = D, 1 = I (current owner)

    // Pending request flags (combinational; held by cache while
    // STALLed or, during a burst fill, while !fill_done).
    logic pending_d, pending_i;
    assign pending_d = i_d_re | i_d_we;
    assign pending_i = i_i_re;

    // Pick policy: dcache wins ties.
    logic pick_d, pick_i;
    assign pick_d = pending_d;
    assign pick_i = pending_i & ~pending_d;

    // ── Latch event: a new request is being captured this edge ──
    //
    // Fires when either:
    //   (a) state is IDLE and a request is pending (entering S_BUSY)
    //   (b) state is BUSY and i_mem_busy=0 (current transaction
    //       completing) and a request is still pending (back-to-back
    //       continuation, same or different owner).
    //
    // Central new-request signal — drives both the o_*_req_accepted
    // pulses (combinationally) and the request-register load enable
    // (in the always_ff below).
    logic latch_event;
    assign latch_event = (state == S_IDLE && (pending_d | pending_i)) ||
                         (state == S_BUSY && !i_mem_busy &&
                          (pending_d | pending_i));

    // ── Sequential: state + latched request ─────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state         <= S_IDLE;
            req_addr      <= 32'b0;
            req_wdata     <= 32'b0;
            req_byte_en   <= 4'b0;
            req_we        <= 1'b0;
            req_re        <= 1'b0;
            req_cacheable <= 1'b0;
            owner         <= 1'b0;
        end else begin
            state <= state_n;

            if (latch_event) begin
                if (pick_d) begin
                    req_addr      <= i_d_addr;
                    req_wdata     <= i_d_wdata;
                    req_byte_en   <= i_d_byte_en;
                    req_we        <= i_d_we;
                    req_re        <= i_d_re;
                    req_cacheable <= i_d_cacheable;
                    owner         <= 1'b0;
                end else begin
                    req_addr      <= i_i_addr;
                    req_wdata     <= 32'b0;
                    req_byte_en   <= 4'b0;
                    req_we        <= 1'b0;
                    req_re        <= i_i_re;
                    req_cacheable <= i_i_cacheable;
                    owner         <= 1'b1;
                end
            end
        end
    end

    // ── Next-state logic ────────────────────────────────────────
    always_comb begin
        state_n = state;
        case (state)
            S_IDLE:
                if (pending_d | pending_i)
                    state_n = S_BUSY;
            S_BUSY:
                if (!i_mem_busy) begin
                    if (pending_d | pending_i)
                        state_n = S_BUSY;   // back-to-back
                    else
                        state_n = S_IDLE;
                end
            default:
                state_n = S_IDLE;
        endcase
    end

    // ── External bus drive — only in S_BUSY ────────────────────
    assign o_mem_addr      = req_addr;
    assign o_mem_wdata     = req_wdata;
    assign o_mem_byte_en   = req_byte_en;
    assign o_mem_we        = (state == S_BUSY) && req_we;
    assign o_mem_re        = (state == S_BUSY) && req_re;
    assign o_mem_cacheable = req_cacheable;

    // ── req_accepted pulses ─────────────────────────────────
    assign o_d_req_accepted = latch_event && pick_d;
    assign o_i_req_accepted = latch_event && pick_i;

    // ── Per-cache rdata — combinational pass-through ───────
    // Both caches see live i_mem_rdata.  Each is gated by its own
    // busy signal — only the current owner sees its busy drop on
    // the response cycle.  Non-owner with pending sees busy=1
    // throughout S_BUSY (driven by its pending bit), so it cannot
    // mis-capture.
    assign o_d_rdata = i_mem_rdata;
    assign o_i_rdata = i_mem_rdata;

    // ══════════════════════════════════════════════════════════
    // Per-cache busy mux
    //
    // In S_IDLE: pick signals "this owner is about to be served on
    //   the next edge" → busy=1 so the cache's STALL holds across
    //   the IDLE→BUSY edge.
    //
    // In S_BUSY: the owner's busy mirrors i_mem_busy — high while
    //   the transaction is in flight, low for the single cycle when
    //   the response arrives (that's when the cache captures rdata).
    //   The non-owner's busy is high iff it has a pending request
    //   (its STALL holds until it becomes the owner).
    //
    // The owner-busy-drops-on-response and non-owner-stays-busy
    // contracts are pinned by the SVAs at the bottom of this file.
    // ══════════════════════════════════════════════════════════
    always_comb begin
        case (state)
            S_IDLE: begin
                o_d_busy = pick_d;
                o_i_busy = pick_i;
            end
            S_BUSY: begin
                o_d_busy = owner == 1'b1 || i_mem_busy;
                o_i_busy = owner == 1'b0 || i_mem_busy;
            end
            default: begin
                o_d_busy = 1'b0;
                o_i_busy = 1'b0;
            end
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Simulation assertions — FSM and handshake invariants
    //
    // These pin down the arbiter's contract with both caches and
    // the external bus.  Stripped by Yosys at synth.
    // ══════════════════════════════════════════════════════════

    // External bus drive is only active in S_BUSY.  In S_IDLE the
    // bus must be quiet so device-side decoders don't double-trigger
    // (e.g. SDRAM adapter speculation latching off a phantom request).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state != S_BUSY) |-> (!o_mem_re && !o_mem_we))
        else $error("cpu_bus_arbiter: bus driven outside S_BUSY");

    // Owner is stable mid-transaction.  Mid-transaction = "in S_BUSY
    // with i_mem_busy held high last cycle".  Owner may change at a
    // BUSY→BUSY edge when i_mem_busy dropped (handoff or burst
    // continuation), so the stability check is gated by
    // $past(i_mem_busy).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_BUSY && $past(state == S_BUSY) && $past(i_mem_busy))
            |-> $stable(owner))
        else $error("cpu_bus_arbiter: owner changed mid-transaction");

    // ── req_accepted contract ──

    // (1) D pulse implies the arbiter latches D's address on the
    //     next edge.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_d_req_accepted |=> (state == S_BUSY && owner == 1'b0 &&
                              req_addr == $past(i_d_addr)))
        else $error("cpu_bus_arbiter: o_d_req_accepted high but no D latch on next edge");

    // (2) I pulse implies the arbiter latches I's address on the
    //     next edge.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_i_req_accepted |=> (state == S_BUSY && owner == 1'b1 &&
                              req_addr == $past(i_i_addr)))
        else $error("cpu_bus_arbiter: o_i_req_accepted high but no I latch on next edge");

    // (3) Reverse direction: any latch event landing in S_BUSY with
    //     owner=D must have been preceded by an o_d_req_accepted
    //     pulse.  A latch event is "state is S_BUSY this cycle and
    //     either $past(state) was IDLE OR $past(state) was S_BUSY
    //     with $past(!i_mem_busy)".
    assert property (@(posedge i_clk) disable iff (i_rst)
        ((($past(state) == S_IDLE) ||
          ($past(state) == S_BUSY && !$past(i_mem_busy)))
         && state == S_BUSY && owner == 1'b0)
            |-> $past(o_d_req_accepted))
        else $error("cpu_bus_arbiter: D latch occurred without prior o_d_req_accepted pulse");

    // (4) Same reverse-direction guarantee for I.
    assert property (@(posedge i_clk) disable iff (i_rst)
        ((($past(state) == S_IDLE) ||
          ($past(state) == S_BUSY && !$past(i_mem_busy)))
         && state == S_BUSY && owner == 1'b1)
            |-> $past(o_i_req_accepted))
        else $error("cpu_bus_arbiter: I latch occurred without prior o_i_req_accepted pulse");

    // (5) D and I pulses are mutually exclusive — at most one
    //     owner is latched per edge.  Structurally guaranteed by
    //     pick_i = pending_i & ~pending_d, but asserted explicitly
    //     so any future refactor that breaks the structural mutex
    //     trips the SVA.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_d_req_accepted && o_i_req_accepted))
        else $error("cpu_bus_arbiter: D and I req_accepted both high in same cycle");

    // ── Cacheable forwarding contract ──

    // Cacheable bit is latched alongside addr/we/re at every
    // latch_event.  Forward direction: a pulse implies the
    // upcoming req_cacheable matches the source port's input.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_d_req_accepted |=> (req_cacheable == $past(i_d_cacheable)))
        else $error("cpu_bus_arbiter: D req_accepted but req_cacheable not from i_d_cacheable");
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_i_req_accepted |=> (req_cacheable == $past(i_i_cacheable)))
        else $error("cpu_bus_arbiter: I req_accepted but req_cacheable not from i_i_cacheable");

    // ── Busy mux contract ──

    // (a) Owner sees busy drop on the response cycle: when
    //     i_mem_busy=0 in S_BUSY, the owner's busy must also be 0
    //     (that's the single cycle the cache captures rdata).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_BUSY && !i_mem_busy && owner == 1'b0)
            |-> !o_d_busy)
        else $error("cpu_bus_arbiter: D owner saw busy=1 on response cycle");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_BUSY && !i_mem_busy && owner == 1'b1)
            |-> !o_i_busy)
        else $error("cpu_bus_arbiter: I owner saw busy=1 on response cycle");

    // (b) Non-owner exclusion: the non-owner with a pending request
    //     must see busy=1 throughout S_BUSY.  Both o_d_rdata and
    //     o_i_rdata are physically wired to i_mem_rdata, so busy is
    //     the only gate that keeps the response from being consumed
    //     by the wrong client.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_BUSY && owner == 1'b0 && pending_i) |-> o_i_busy)
        else $error("cpu_bus_arbiter: non-owner I saw busy=0 in S_BUSY (would latch D's response)");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_BUSY && owner == 1'b1 && pending_d) |-> o_d_busy)
        else $error("cpu_bus_arbiter: non-owner D saw busy=0 in S_BUSY (would latch I's response)");

endmodule

// verilator lint_on UNUSEDSIGNAL
