// txn_arbiter — Penumbra/2 transactional I/D arbiter (L1↔L2 layer)
//
// Grants the single downstream port (the fill sequencer in front of the
// shared L2) to one L1 back side — instruction or data — for the duration
// of a whole transaction, per
// doc/internals/penumbra2/memory-interface.md: transaction-granular grant,
// single-outstanding, D-priority on simultaneous pending, full-shape
// forwarding ({addr, wdata, byte_en, re, we, cacheable}).
//
// **Registered downstream request.** The granted request is presented to
// the downstream from a register (mq_*), not combinationally. A master's
// request → downstream chain would otherwise concatenate the L1's resolve
// cone (TLB translate + tag compare + miss/uncached classify) with the
// whole L2 read pipeline in one cycle — the cone that capped the machine's
// fmax. The launch register is the cut: the request reaches the downstream
// one cycle after grant, so the front cone (master → mq_*) and the back
// cone (mq_* → L2 → completion) become two flop-bounded paths instead of
// one. The cost is one cycle per *transaction* — a line fill engages one
// cycle later, an uncacheable beat completes one cycle later — and zero on
// the fill's per-word streaming (the sequencer walks words downstream of
// this register) and zero on L1 hits (which never reach the arbiter). This
// re-introduces the registered-request structure the gen1 arbiter had and
// an earlier gen2 draft dropped; the draft's premise (registered L1 hits
// keep the master→downstream path short) holds for hits but not for the
// miss/uncached request, which carries the resolve cone.
//
// Completion is type-dependent: a line read (re && cacheable) releases on
// the fill sequencer's fill_done; every single-beat op releases on the
// downstream busy-drop. Those two events are all the arbiter follows — it
// never sees words and carries no line-size knowledge. The fill bundle is
// routed through opaquely: word/wdata broadcast to both sides, the we/done
// strobes gated to the in-flight owner.
//
// The non-owner port sees o_busy=1, so its level-held request keeps
// waiting; pure-stall bounds demand to at most one pending request per
// side, so no queue exists and the wait is bounded by one transaction.
// D-priority is a tiebreak, not a starvation risk: a completed D-access
// must drain through MEM/WB before the next D-transaction can launch, and
// the I-side's held request wins the grant in that gap.
//
// Back-to-back launch without an idle gap is preserved through the launch
// eligibility: on a completion cycle the completing owner is still holding
// its (now stale) request — it drops the next cycle — so a fresh launch may
// pick only the *other* side, never re-launching the just-completed owner
// into a phantom second transaction.

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

    // ── Registered downstream request (the launch register) ───────
    // mq_valid says a transaction is presented downstream this cycle;
    // mq_owner names the side; mq_{addr,…} hold its captured shape. The
    // downstream port is driven purely from these flops — the cut.
    logic        mq_valid, mq_owner;     // owner: 0 = port I, 1 = port D
    logic [31:0] mq_addr, mq_wdata;
    logic [3:0]  mq_byte_en;
    logic        mq_re, mq_we, mq_cacheable;

    // ── In-flight completion ──────────────────────────────────────
    // A line read completes on the sequencer's fill_done; a single beat
    // completes on the downstream busy-drop. Both qualify on mq_valid, so
    // an idle arbiter never spuriously completes.
    logic is_line, complete;
    assign is_line  = mq_re & mq_cacheable;
    assign complete = mq_valid & (is_line ? i_fill_done
                                          : ((mq_re | mq_we) & ~i_m_busy));

    // ── Launch select ─────────────────────────────────────────────
    // A fresh transaction launches when the register is free (idle, or the
    // in-flight one completes this cycle for a back-to-back hand-off),
    // D-priority. On a completion cycle the completing owner is excluded:
    // it is still asserting its now-stale request (it drops next cycle), so
    // re-launching it would present a phantom second transaction.
    logic i_elig, d_elig;
    always_comb begin
        i_elig = i_req;
        d_elig = d_req;
        if (mq_valid && complete) begin
            if (mq_owner == 1'b0) i_elig = 1'b0;
            else                  d_elig = 1'b0;
        end
    end

    logic launch, launch_d;
    assign launch   = (!mq_valid | complete) & (i_elig | d_elig);
    assign launch_d = d_elig;            // D-priority among the eligible

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            mq_valid <= 1'b0;
        end else if (launch) begin
            mq_valid     <= 1'b1;
            mq_owner     <= launch_d;
            mq_addr      <= launch_d ? i_d_addr      : i_i_addr;
            mq_wdata     <= launch_d ? i_d_wdata     : i_i_wdata;
            mq_byte_en   <= launch_d ? i_d_byte_en   : i_i_byte_en;
            mq_re        <= launch_d ? i_d_re        : i_i_re;
            mq_we        <= launch_d ? i_d_we        : i_i_we;
            mq_cacheable <= launch_d ? i_d_cacheable : i_i_cacheable;
        end else if (complete) begin
            mq_valid <= 1'b0;
        end
    end

    // ── Downstream request — purely registered (the cut) ──────────
    assign o_m_addr      = mq_addr;
    assign o_m_wdata     = mq_wdata;
    assign o_m_byte_en   = mq_byte_en;
    assign o_m_re        = mq_valid & mq_re;
    assign o_m_we        = mq_valid & mq_we;
    assign o_m_cacheable = mq_cacheable;

    // ── Responses: data broadcast, busy + fill strobes by owner ───
    // A requesting master is busy until its own transaction completes; the
    // completing owner drops to 0 on the completion cycle so its L1 sees the
    // busy-drop. A non-requesting master reads 0 (its L1 won't sample it).
    logic i_owns, d_owns;
    assign i_owns = mq_valid & (mq_owner == 1'b0);
    assign d_owns = mq_valid & (mq_owner == 1'b1);

    assign o_i_rdata = i_m_rdata;
    assign o_d_rdata = i_m_rdata;
    assign o_i_busy  = (i_owns & complete) ? 1'b0 : i_req;
    assign o_d_busy  = (d_owns & complete) ? 1'b0 : d_req;

    assign o_i_fill_we    = i_fill_we   & i_owns;
    assign o_i_fill_done  = i_fill_done & i_owns;
    assign o_d_fill_we    = i_fill_we   & d_owns;
    assign o_d_fill_done  = i_fill_done & d_owns;
    assign o_i_fill_word  = i_fill_word;
    assign o_d_fill_word  = i_fill_word;
    assign o_i_fill_wdata = i_fill_wdata;
    assign o_d_fill_wdata = i_fill_wdata;

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // A launch presents the transaction downstream next cycle.
    assert property (@(posedge i_clk) disable iff (i_rst)
        launch |=> mq_valid)
        else $error("txn_arbiter: launched transaction not registered");

    // A completion with no back-to-back launch frees the register.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (complete && !launch) |=> !mq_valid)
        else $error("txn_arbiter: completed transaction left the register valid");

    // The owner never changes while a transaction is in flight.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (mq_valid && !complete) |=> $stable(mq_owner))
        else $error("txn_arbiter: owner changed mid-transaction");

    // Fill activity only belongs to an in-flight line transaction.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_fill_we || i_fill_done) |-> (mq_valid && is_line))
        else $error("txn_arbiter: fill activity outside a line transaction");

    // The presented request has a sane single shape.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_m_re && o_m_we))
        else $error("txn_arbiter: presented request asserts read and write");

endmodule
