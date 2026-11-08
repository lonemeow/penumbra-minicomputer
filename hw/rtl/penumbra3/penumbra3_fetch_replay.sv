// penumbra3_fetch_replay -- brings a fetch the L1-I had no word for back
// through IF1.
//
// IF2 cannot hold a slot on a miss without putting the I-cache hit on IF1's
// hold, so a slot with no word leaves (i_miss) as the L1-I takes its access,
// and IF1 goes on launching. The cycle after the miss this unit steers IF1
// back to the missed PC (o_replay), and the slot IF1 launched behind the
// miss is flushed. IF1 holds while the L1-I is busy, then launches the
// missed PC again, and the slot that comes back through IF2 takes the
// access's word and fault (o_deliver) in place of its lookup's verdict,
// whether or not the line went into the L1
// (doc/internals/penumbra3/l1-cache.md, "The fetch side").
//
// One miss, cycle by cycle. m is the miss, d the L1-I's done:
//
//   m     i_miss: the fetch's PC is latched; the L1-I takes the access
//   m+1   o_replay steers IF1 to the missed PC and flushes the slot behind
//         the miss. The L1-I is busy
//   d     i_access_done. The L1-I is not busy, and IF1 launches the missed
//         PC again
//   d+1   o_deliver; the slot is back in IF2 and takes the access's word
//
// o_deliver stays high until the slot leaves IF2. A redirect or a fault
// entry that makes the fetch unwanted while its access is in flight flushes
// it (i_flush): the slot takes no word, and the access runs to its done
// regardless.
module penumbra3_fetch_replay (
    input  logic        i_clk,
    input  logic        i_rst,

    // -- The miss: a slot IF2 has no word for ----------------------
    input  logic        i_miss,           // one cycle; never while i_busy
    input  logic [31:0] i_miss_pc,

    // -- The L1-I's access -----------------------------------------
    input  logic        i_busy,
    input  logic        i_access_done,    // one cycle

    // -- The front end ---------------------------------------------
    input  logic        i_flush,          // the fetch is unwanted
    input  logic        i_slot_left,      // IF2 enqueued the slot in it
    output logic        o_replay,         // one cycle: steer IF1 to o_replay_pc
    output logic [31:0] o_replay_pc,
    output logic        o_replayed,       // level: a replay is on its way
    output logic        o_deliver         // level: the slot takes the access's word
);

    // pc_q loads every cycle the L1-I is not busy, so its enable stays off
    // the miss decision, which waits on the TLB verdict and the L1 hit.
    logic [31:0] pc_q;
    always_ff @(posedge i_clk)
        if (!i_busy) pc_q <= i_miss_pc;

    // replay_q:   the cycle after the miss.
    // replayed_q: a replay is on its way through IF1 and IF2, from the cycle
    //             after the miss until the slot leaves IF2 or a flush makes
    //             it unwanted.
    // deliver_q:  the replayed slot takes the access's word, from the cycle
    //             after the done until the slot leaves IF2 or is flushed. A
    //             flush in the done cycle wins.
    logic replay_q, replayed_q, deliver_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            replay_q   <= 1'b0;
            replayed_q <= 1'b0;
            deliver_q  <= 1'b0;
        end else begin
            replay_q <= i_miss;

            if (i_flush || i_slot_left) replayed_q <= 1'b0;
            else if (i_miss)            replayed_q <= 1'b1;

            if (i_flush || i_slot_left)           deliver_q <= 1'b0;
            else if (i_access_done && replayed_q) deliver_q <= 1'b1;
        end
    end

    assign o_replay    = replay_q;
    assign o_replay_pc = pc_q;
    assign o_replayed  = replayed_q;
    assign o_deliver   = deliver_q;

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // IF1 holds while the L1-I is busy, so no slot reaches IF2 to miss.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_miss && i_busy))
        else $error("penumbra3_fetch_replay: a miss while the L1-I is busy");

endmodule
