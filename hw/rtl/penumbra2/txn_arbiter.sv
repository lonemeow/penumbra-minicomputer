// txn_arbiter — Penumbra/2 transactional I/D arbiter (L1↔L2 layer)
//
// Grants the single downstream port (the fill sequencer in front of the
// shared L2) to one L1 back side — instruction or data — for the duration
// of a whole transaction, per
// doc/internals/penumbra2/memory-interface.md: transaction-granular grant,
// single-outstanding, D-priority on simultaneous pending, full-shape
// forwarding ({addr, wdata, byte_en, re, we, cacheable}).
//
// Completion is type-dependent: a line read (re && cacheable) releases on
// the fill sequencer's fill_done; every single-beat op releases on the
// downstream busy-drop. Those two events are all the arbiter follows — it
// never sees words and carries no line-size knowledge. The fill bundle is
// routed through opaquely: word/wdata broadcast to both sides, the we/done
// strobes gated to the owner.
//
// The non-owner port sees o_busy=1, so its level-held request keeps
// waiting; pure-stall bounds demand to at most one pending request per
// side, so no queue exists and the wait is bounded by one transaction.
// D-priority is a tiebreak, not a starvation risk: a completed D-access
// must drain through MEM/WB before the next D-transaction can launch, and
// the I-side's held request wins the grant in that gap.
//
// Masters must present a completed request deasserted during the cycle
// after its completion — the L1 satisfies this combinationally, its
// request dropping with the state change at the completion edge — so a
// released grant re-arbitrates over honestly-new requests only (a
// residual line request would otherwise engage a phantom second fill in
// the sequencer). The grant select is combinational: a fresh
// request reaches the downstream port on its first cycle, and an
// immediately-ready downstream (busy already low) completes a single beat
// in that same cycle without ever taking the lock.

module txn_arbiter #(
    parameter int FILL_WORD_W = 2   // routed fill word-index width (opaque payload)
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Port I: instruction L1 back side ──────────────────────────
    input  logic [31:0] i_i_addr,
    input  logic [31:0] i_i_wdata,
    input  logic [3:0]  i_i_byte_en,
    input  logic        i_i_re,
    input  logic        i_i_we,
    input  logic        i_i_cacheable,
    output logic [31:0] o_i_rdata,
    output logic        o_i_busy,
    output logic                   o_i_fill_we,
    output logic [FILL_WORD_W-1:0] o_i_fill_word,
    output logic [31:0]            o_i_fill_wdata,
    output logic                   o_i_fill_done,

    // ── Port D: data L1 back side ──────────────────────────────────
    input  logic [31:0] i_d_addr,
    input  logic [31:0] i_d_wdata,
    input  logic [3:0]  i_d_byte_en,
    input  logic        i_d_re,
    input  logic        i_d_we,
    input  logic        i_d_cacheable,
    output logic [31:0] o_d_rdata,
    output logic        o_d_busy,
    output logic                   o_d_fill_we,
    output logic [FILL_WORD_W-1:0] o_d_fill_word,
    output logic [31:0]            o_d_fill_wdata,
    output logic                   o_d_fill_done,

    // ── Downstream: the fill sequencer's upstream port ─────────────
    output logic [31:0] o_m_addr,
    output logic [31:0] o_m_wdata,
    output logic [3:0]  o_m_byte_en,
    output logic        o_m_re,
    output logic        o_m_we,
    output logic        o_m_cacheable,
    input  logic [31:0] i_m_rdata,
    input  logic        i_m_busy,
    input  logic                   i_fill_we,
    input  logic [FILL_WORD_W-1:0] i_fill_word,
    input  logic [31:0]            i_fill_wdata,
    input  logic                   i_fill_done
);

    // ── Pending requests, this cycle ──────────────────────────────
    logic i_req, d_req;
    assign i_req = i_i_re | i_i_we;
    assign d_req = i_d_re | i_d_we;

    // ── Grant state ───────────────────────────────────────────────
    // Encoded owner + active lock: owner_q names the side, active_q says
    // a transaction is in flight and the select below must hold it.
    logic owner_q;                  // 0 = port I, 1 = port D
    logic active_q;

    // The side driven downstream this cycle. Locked to the owner while a
    // transaction is active; otherwise a new request claims it with
    // D-priority. Combinational, so a fresh request forwards on its
    // first cycle.
    logic grant_valid, grant_d;
    assign grant_valid = active_q | i_req | d_req;
    assign grant_d     = active_q ? owner_q : d_req;

    // ── Full-shape forward mux ────────────────────────────────────
    always_comb begin
        if (grant_valid && grant_d) begin
            o_m_addr      = i_d_addr;
            o_m_wdata     = i_d_wdata;
            o_m_byte_en   = i_d_byte_en;
            o_m_re        = i_d_re;
            o_m_we        = i_d_we;
            o_m_cacheable = i_d_cacheable;
        end else if (grant_valid) begin
            o_m_addr      = i_i_addr;
            o_m_wdata     = i_i_wdata;
            o_m_byte_en   = i_i_byte_en;
            o_m_re        = i_i_re;
            o_m_we        = i_i_we;
            o_m_cacheable = i_i_cacheable;
        end else begin
            o_m_addr      = 32'b0;
            o_m_wdata     = 32'b0;
            o_m_byte_en   = 4'b0;
            o_m_re        = 1'b0;
            o_m_we        = 1'b0;
            o_m_cacheable = 1'b0;
        end
    end

    // ── Transaction type + completion event ───────────────────────
    // A line read completes on fill_done; a single beat completes on the
    // downstream busy-drop (which can be the transaction's first cycle).
    logic is_line, complete;
    assign is_line  = o_m_re & o_m_cacheable;
    assign complete = grant_valid
                    & (is_line ? i_fill_done
                               : ((o_m_re | o_m_we) & ~i_m_busy));

    // ── Grant lock update ─────────────────────────────────────────
    // Owns owner_q/active_q. The contract the assertions below pin:
    // a granted transaction that does not complete this cycle is locked
    // to its owner from the next cycle; completion releases the lock at
    // the edge (the combinational select re-arbitrates the following
    // cycle); a same-cycle start-and-complete never takes the lock; the
    // owner never changes while the lock is held.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            active_q <= 1'b0;
        end else if (grant_valid && !complete) begin
            active_q <= 1'b1;
            owner_q  <= grant_d;
        end else if (complete) begin
            active_q <= 1'b0;
        end
    end

    // ── Responses: data broadcast, busy and fill strobes by owner ──
    assign o_i_rdata = i_m_rdata;
    assign o_d_rdata = i_m_rdata;
    assign o_i_busy  = (grant_valid && !grant_d) ? i_m_busy : 1'b1;
    assign o_d_busy  = (grant_valid &&  grant_d) ? i_m_busy : 1'b1;

    assign o_i_fill_we    = i_fill_we   & grant_valid & ~grant_d;
    assign o_i_fill_done  = i_fill_done & grant_valid & ~grant_d;
    assign o_d_fill_we    = i_fill_we   & grant_valid &  grant_d;
    assign o_d_fill_done  = i_fill_done & grant_valid &  grant_d;
    assign o_i_fill_word  = i_fill_word;
    assign o_d_fill_word  = i_fill_word;
    assign o_i_fill_wdata = i_fill_wdata;
    assign o_d_fill_wdata = i_fill_wdata;

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // The first three pin the grant-lock contract; the rest guard the
    // interface obligations.
    // ══════════════════════════════════════════════════════════

    // A transaction that does not complete this cycle is locked next cycle.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (grant_valid && !complete) |=> active_q)
        else $error("txn_arbiter: in-flight transaction not locked");

    // Completion releases the lock.
    assert property (@(posedge i_clk) disable iff (i_rst)
        complete |=> !active_q)
        else $error("txn_arbiter: completed transaction left the lock held");

    // The owner never changes while the lock is held.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (active_q && !complete) |=> $stable(owner_q))
        else $error("txn_arbiter: owner changed mid-transaction");

    // Fill activity only belongs to an active line transaction.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_fill_we || i_fill_done) |-> (grant_valid && is_line))
        else $error("txn_arbiter: fill activity outside a line transaction");

    // The granted request has a sane single shape.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_m_re && o_m_we))
        else $error("txn_arbiter: granted request asserts read and write");

endmodule
