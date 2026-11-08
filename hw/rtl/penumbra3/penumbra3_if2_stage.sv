// penumbra3_if2_stage -- Penumbra/3 instruction fetch, second half.
//
// Receives the IF1/IF2 register (this fetch's PC, next_PC, valid) and the L1's
// and MMU's verdicts on the lookup IF1 launched with it, composes the fetch
// fault, and *enqueues* the slot into the fetch FIFO. gen3 has no separate
// IF2/ID register: the FIFO storage is that register, so the decode runs
// combinationally on the enqueue path and the FIFO flops it. ID then reads a
// registered, pre-decoded bundle, so its issue cone starts from flops without
// paying an extra fetch-to-decode cycle.
//
// IF2 works per way, then selects. The word->bundle decode runs once for each
// way, on that way's word straight off the block RAM, beside the TLB match and
// the tag compare; the hit only selects a finished bundle. So the cone behind
// the hit is the select and the FIFO write, never the decode.
//
// A miss reaches IF1 only through a register. A slot with no word reports
// o_miss and leaves as the L1-I takes its access; penumbra3_fetch_replay then
// steers IF1 back to its PC, and the fetch comes round again and takes the
// access's word (doc/internals/penumbra3/l1-cache.md, "The fetch side"). The
// hold IF1 takes from here reads flops alone: a slot held for FIFO room stays
// in IF2 while the L1 reads its set again.
//
// Fault composition matches the IF-stage contract. A misaligned fetch address
// or a translation fault tags the slot inert (fault_pending) with a composed,
// self-qualifying status: alignment outranks the TLB verdict and is composed
// here; a TLB fault arrives composed from the MMU. A bus fault is late, known
// only once the read returns, so it arrives with the replayed slot's delivery.
// The faulting vaddr is the slot's own pc.
//
// A flush (a redirect, a fault entry, the replay's discard of the slot behind
// a miss) kills the slot: nothing enqueues and nothing is reported missed.

// keep_hierarchy: hold this stage boundary through synth_ecp5 so the backward
// stall path places compactly and reads with real names in timing reports.
(* keep_hierarchy = "yes" *)
module penumbra3_if2_stage
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
#(
    parameter int WAYS = 2                // the L1's ways
)(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── IF1/IF2 register in ──────────────────────────────────────
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,
    input  logic                  i_valid,        // 0 = bubble in

    // ── L1 verdict on the lookup IF1 launched with this slot ─────
    input  logic [WAYS-1:0]       i_way_hit,      // one-hot
    input  logic [WAYS-1:0][31:0] i_way_word,
    input  logic                  i_hit,

    // ── MMU I-side verdict ───────────────────────────────────────
    input  logic                  i_cacheable,        // the page's, for counting
    input  logic                  i_user_mode,        // fetch privilege (status info)
    input  logic                  i_mmu_fault,        // a translation fault
    input  logic [31:0]           i_mmu_fault_status, // composed by the MMU

    // ── The replay (penumbra3_fetch_replay) and the access's word ──
    input  logic                  i_replayed,     // its miss is already counted
    input  logic                  i_deliver,      // level: the slot takes the word below
    input  logic [31:0]           i_deliver_word, // the L1-I's access's word and fault
    input  logic                  i_deliver_fault,
    output logic                  o_miss,         // one cycle: the slot has no word

    // ── Front-end flush ──────────────────────────────────────────
    input  logic                  i_flush,        // discard this slot

    // ── Fetch-FIFO enqueue (the IF2/ID boundary register lives here) ──
    input  logic                  i_enq_ready,    // FIFO can accept
    output logic                  o_enq_valid,
    output logic                  o_enq_fire,     // the slot leaves IF2 this cycle
    output ctrl_bundle_t          o_enq_bundle,
    output logic [31:0]           o_enq_pc,
    output logic [31:0]           o_enq_next_pc,
    output logic                  o_enq_fault_pending,
    output logic [3:0]            o_enq_fault_vec,
    output logic [31:0]           o_enq_fault_status,

    // ── To the L1 and IF1 ────────────────────────────────────────
    output logic                  o_count_read,   // once per cacheable fetch
    output logic                  o_hold          // the slot waits for FIFO room
);

    // ── Early fault detect ───────────────────────────────────────
    // Only a real fetch consumes the verdict. Alignment (from the PC) outranks
    // the TLB verdict (a misaligned address cannot be meaningfully translated).
    logic align_fault, tlb_fault, early_fault;
    assign align_fault = i_valid & (i_pc[1:0] != 2'b00);
    assign tlb_fault   = i_valid & i_mmu_fault & ~align_fault;
    assign early_fault = align_fault | tlb_fault;

    // ── The slot's word ──────────────────────────────────────────
    // An early-faulted slot needs none. A replayed slot takes the delivered
    // word whether or not its own lookup hits: the access's line may have
    // gone into the L1, and its word is the same either way.
    logic word_avail;
    assign word_avail = early_fault | i_deliver | i_hit;

    assign o_enq_valid = i_valid & ~i_flush & word_avail;
    assign o_miss      = i_valid & ~i_flush & ~word_avail;
    assign o_enq_fire  = o_enq_valid & i_enq_ready;
    assign o_hold      = i_valid & ~i_enq_ready;

    assign o_count_read = (o_miss | (o_enq_fire & ~early_fault))
                        & i_cacheable & ~i_replayed;

    // ── Outgoing fault composition ───────────────────────────────
    // A fetch is always an execute access. Status by source: alignment and bus
    // compose here, the TLB status arrives composed from the MMU.
    logic bus_fault;
    assign bus_fault = i_deliver & i_deliver_fault;
    assign o_enq_fault_pending = early_fault | bus_fault;
    always_comb begin
        if      (align_fault) o_enq_fault_status = compose_fault_status(i_user_mode, ACC_EXEC, FAULT_ALIGN);
        else if (tlb_fault)   o_enq_fault_status = i_mmu_fault_status;
        else if (bus_fault)   o_enq_fault_status = compose_fault_status(i_user_mode, ACC_EXEC, FAULT_BUS);
        else                  o_enq_fault_status = 32'b0;   // FAULT_NONE
    end
    assign o_enq_fault_vec = o_enq_fault_pending ? fault_vec_of(o_enq_fault_status[3:0]) : 4'd0;

    // ── Decode per way, then select ──────────────────────────────
    // A delivered word takes way 0's decoder and masks the ways' hits, both
    // steered by a flop, so it adds no decoder and only that flop's AND to
    // the select the hit drives. A faulted slot's bundle is don't-care --
    // downstream gates on o_enq_fault_pending.
    localparam int BUNDLE_W = $bits(ctrl_bundle_t);

    logic [WAYS-1:0][31:0] way_word;
    logic [WAYS-1:0]       way_sel;
    always_comb begin
        way_word = i_way_word;
        if (i_deliver) way_word[0] = i_deliver_word;
        way_sel = i_way_hit & ~{WAYS{i_deliver}};
        way_sel[0] = way_sel[0] | i_deliver;
    end

    logic [WAYS-1:0][BUNDLE_W-1:0] way_bundle;
    for (genvar w = 0; w < WAYS; w++) begin : g_decode
        ctrl_bundle_t bundle;
        penumbra3_decode u_decode (
            .i_ir     (way_word[w]),
            .o_bundle (bundle)
        );
        assign way_bundle[w] = bundle;
    end

    always_comb begin
        logic [BUNDLE_W-1:0] selected;
        selected = '0;
        for (int w = 0; w < WAYS; w++)
            selected |= {BUNDLE_W{way_sel[w]}} & way_bundle[w];
        o_enq_bundle = ctrl_bundle_t'(selected);
    end

    assign o_enq_pc      = i_pc;
    assign o_enq_next_pc = i_next_pc;

    // ── Assertions (sim-only; stripped at synth) ─────────────────
    // A flush discards the slot: nothing enqueues and nothing misses.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_flush |-> !(o_enq_valid || o_miss))
        else $error("penumbra3_if2_stage: flush did not discard the slot");
    // A slot with a word never misses, and a faulted slot asks for nothing.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_miss && (i_deliver || i_hit || early_fault)))
        else $error("penumbra3_if2_stage: a slot with no need of a word missed");

endmodule
