// penumbra3_fetch_buffer -- elastic decoupling FIFO for the IF2 -> ID seam.
//
// Breaks the combinational stall path from the back end to the fetch enable.
// Without it, ID's this-cycle stall (itself the tail of a load/divmul/hazard
// back-pressure) would propagate combinationally all the way back to the
// fetch enable -- the core's longest path. Interposing this FIFO lets the
// fetch front end see o_enq_ready (a registered occupancy bit) instead of
// id_stall, so a back-end stall no longer reaches the fetch enable in the
// same cycle: the front end runs ahead into the FIFO and back-pressures only
// when it fills.
//
// Generic valid/ready elastic buffer. It carries an opaque PAYLOAD_W-bit word
// -- in gen3 the core packs the *pre-decoded* control bundle plus its PC and
// fault state in, and unpacks them out -- so it knows nothing about the
// pipeline it sits in and can be unit-tested alone.
//
// Latency: a slot enqueued into an empty FIFO presents at the head the *next*
// cycle (registered storage, no same-cycle bypass). That one cycle is the
// pre-decode pipeline stage itself: gen3 runs the heavy word->bundle decode
// on the enqueue path, and this registered output is what keeps ID's issue
// cone starting from flops. A bypass/skid buffer would be cycle-neutral only
// by leaving that decode combinational into ID -- re-forming the very cone
// the registered boundary exists to cut -- so the FIFO does not bypass.
//
// DEPTH=2 is the minimum that breaks the ready path without throttling
// throughput (a 1-deep register cannot accept on the cycle it drains), and is
// skid-buffer-equivalent in capacity. Deeper only adds run-ahead that absorbs
// back-end-stall bursts; it does not change mispredict latency (the refill
// path is +1 regardless of depth), so depth is a measure-driven knob, not a
// structural choice.
//
// Flush (taken-branch redirect / fault entry / interrupt drain) empties the
// FIFO in one cycle: every buffered slot is younger than the cut point and
// must be discarded and re-fetched, exactly as IF1/IF2 bubble their own slots.

module penumbra3_fetch_buffer #(
    parameter int PAYLOAD_W = 133,   // set by the core to $bits(carried payload)
    parameter int DEPTH     = 2
)(
    input  logic                 i_clk,
    input  logic                 i_rst,
    input  logic                 i_flush,        // discard all entries this cycle

    // ── Enqueue (from IF2) ───────────────────────────────────────
    input  logic                 i_enq_valid,    // IF2 presents a real slot
    input  logic [PAYLOAD_W-1:0] i_enq_data,
    output logic                 o_enq_ready,    // FIFO can accept: ~full, a function of the registered count only (never i_deq_ready)

    // ── Dequeue (to ID / the spine) ──────────────────────────────
    output logic                 o_deq_valid,    // a slot is available (~empty)
    output logic [PAYLOAD_W-1:0] o_deq_data,
    input  logic                 i_deq_ready     // ID can accept this cycle (~id_stall)
);
    localparam int PTR_W = (DEPTH <= 1) ? 1 : $clog2(DEPTH);
    localparam int CNT_W = $clog2(DEPTH + 1);

    logic [PAYLOAD_W-1:0] mem [0:DEPTH-1];
    logic [PTR_W-1:0]     wptr, rptr;
    logic [CNT_W-1:0]     count;

    // Handshake fires (combinational), driven by the control block below.
    logic enq_fire, deq_fire;

    // ── Datapath ─────────────────────────────────────────────────
    // Write the enqueued word at the tail; present the head combinationally.
    // The head is only meaningful when o_deq_valid (count != 0).
    always_ff @(posedge i_clk)
        if (enq_fire) mem[wptr] <= i_enq_data;

    assign o_deq_data = mem[rptr];

    // ══════════════════════════════════════════════════════════
    // Occupancy + handshake control
    // ══════════════════════════════════════════════════════════
    // The path-breaking property lives in o_enq_ready: it is combinational, but
    // its sole input is `count` (a register) -- i_deq_ready never reaches it. So
    // the back-end stall (which drives i_deq_ready) has no combinational path to
    // the fetch enable; the front end sees occupancy sampled a cycle in arrears.
    // Widening it to "not full OR draining" would drain a cycle sooner but would
    // route i_deq_ready straight into o_enq_ready and re-form the very path we
    // are cutting -- so it stays count-only. The one-cycle bubble on the way out
    // of full is the price; the FIFO settles at DEPTH-1 and runs full-throughput.

    assign o_enq_ready = count < DEPTH[CNT_W-1:0];
    assign o_deq_valid = count != 0;

    assign enq_fire = i_enq_valid & o_enq_ready;
    assign deq_fire = o_deq_valid & i_deq_ready;

    always_ff @(posedge i_clk) begin
        if (i_rst | i_flush) begin
            count <= 'b0;
            wptr  <= 'b0;
            rptr  <= 'b0;
        end else begin
            if (enq_fire)
                wptr <= wptr + 'd1;
            if (deq_fire)
                rptr <= rptr + 'd1;
            count <= count + (enq_fire - deq_fire);
        end
    end

    // ── Assertions (sim-only; stripped at synth) ─────────────────
    // Occupancy never exceeds DEPTH, and the handshakes respect it: no
    // enqueue into a full FIFO, no dequeue from an empty one.
    // synthesis translate_off
    always_ff @(posedge i_clk) if (!i_rst) begin
        assert (count <= DEPTH[CNT_W-1:0])
            else $error("penumbra3_fetch_buffer: count exceeded DEPTH");
        assert (!(enq_fire && count == DEPTH[CNT_W-1:0] && !deq_fire))
            else $error("penumbra3_fetch_buffer: enqueue into a full FIFO");
        assert (!(deq_fire && count == '0))
            else $error("penumbra3_fetch_buffer: dequeue from an empty FIFO");
    end
    // synthesis translate_on

endmodule
