// penumbra3_if2_stage -- Penumbra/3 instruction fetch, second half.
//
// Receives the IF1/IF2 register (this fetch's PC, next_PC, valid) and the
// instruction word the I-cache BRAM presents this cycle, composes the fetch
// fault, runs the word->bundle decode, and *enqueues* the result into the
// fetch FIFO. gen3 has no separate IF2/ID register: the FIFO storage is that
// register, so the decode runs combinationally on the enqueue path and the
// FIFO flops it. This is the "pre-decode folds into the enqueue, no extra
// stage" arrangement -- ID then reads a registered, pre-decoded bundle, so its
// issue cone starts from flops without paying an extra fetch-to-decode cycle.
//
// Fault composition matches the IF-stage contract. A misaligned fetch address
// or a translation fault tags the slot inert (fault_pending) with a composed,
// self-qualifying status: alignment outranks the TLB verdict and is composed
// here; a TLB fault arrives composed from the MMU. A bus fault is late -- it is
// known only when the requested word returns faulted (i_mem_fault at the busy
// drop) -- so a bus-faulting slot does issue, waits out the port, and learns
// its fault on the completion cycle (carried by the skid if the FIFO is full
// just then). The faulting vaddr is the slot's own pc.
//
// Skid (one entry). The I-cache completion is drop-equals-valid: a fill serves
// its word for exactly one cycle. The FIFO absorbs back-end rate mismatch, but
// because fetch is pipelined (IF1 launches word N while IF2 completes word
// N-1) up to two words head for the FIFO, so a single completion can still
// land the cycle the FIFO is momentarily full. The skid catches that one
// completion; IF1 is held until it drains. The FIFO replaces the IF2/ID
// register, not the skid.
//
// Flush (taken-branch redirect / fault entry) discards the in-flight slot:
// o_enq_valid drops and the skid is cleared, winning over back-pressure. The
// fetch request does NOT gate on i_flush -- a flush coincident with a miss
// lets the wrong-path fill complete into the void (the withdrawn slot consumes
// nothing), which keeps the flush off the request/fetch-busy edge and out of
// the redirect cone. The cost is a possible wasted fill, never a hazard.

// keep_hierarchy: hold this stage boundary through synth_ecp5 so the backward
// stall path places compactly and reads with real names in timing reports.
(* keep_hierarchy = "yes" *)
module penumbra3_if2_stage
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic         i_clk,
    input  logic         i_rst,

    // ── IF1/IF2 register in ──────────────────────────────────────
    input  logic [31:0]  i_pc,
    input  logic [31:0]  i_next_pc,
    input  logic         i_valid,        // 0 = bubble in

    // ── I-side front port (the L1's front side) ──────────────────
    // i_ir is the word for the launch IF1 drove last cycle, valid on the
    // completion cycle (i_mem_busy low); o_fetch_re is this stage's request,
    // held level from the resolve cycle until the completion is consumed.
    input  logic [31:0]  i_ir,
    input  logic         i_mem_busy,
    input  logic         i_mem_fault,    // bus fault on the fetch, at the i_mem_busy drop
    output logic         o_fetch_re,

    // ── MMU I-side verdict (port A; query launched with IF1's fetch) ──
    // Only the fault leg is consumed here; the paddr leg goes to the I-cache
    // tag compare.
    input  logic         i_user_mode,        // fetch privilege (alignment status info)
    input  logic         i_mmu_fault,        // any translation fault (miss / protection)
    input  logic [31:0]  i_mmu_fault_status, // composed by the MMU

    // ── Front-end flush ──────────────────────────────────────────
    input  logic         i_flush,        // taken-branch / fault flush: discard this slot

    // ── Fetch-FIFO enqueue (the IF2/ID boundary register lives here) ──
    input  logic         i_enq_ready,    // FIFO can accept (penumbra3_fetch_buffer o_enq_ready)
    output logic         o_enq_valid,
    output ctrl_bundle_t o_enq_bundle,
    output logic [31:0]  o_enq_pc,
    output logic [31:0]  o_enq_next_pc,
    output logic         o_enq_fault_pending,
    output logic [3:0]   o_enq_fault_vec,
    output logic [31:0]  o_enq_fault_status,

    // ── Back-pressure to IF1 ─────────────────────────────────────
    output logic         o_stall
);

    // ── Skid (one entry) ─────────────────────────────────────────
    logic        skid_full, skid_fault;
    logic [31:0] skid_word;

    // ── Early fault detect ───────────────────────────────────────
    // Only a real fetch consumes the verdict. Alignment (from the PC) outranks
    // the TLB verdict (a misaligned address cannot be meaningfully translated).
    logic align_fault, tlb_fault, early_fault;
    assign align_fault = i_valid & (i_pc[1:0] != 2'b00);
    assign tlb_fault   = i_valid & i_mmu_fault & ~align_fault;
    assign early_fault = align_fault | tlb_fault;

    // ── Fetch request ────────────────────────────────────────────
    // A valid, un-faulted slot whose word is not already parked requests it,
    // level, until the completion is consumed. A faulted slot raises no request
    // (its launch happened via the VIPT overlap, but nothing may complete on
    // its behalf). Does not gate on i_flush -- see the header.
    assign o_fetch_re = i_valid & ~early_fault & ~skid_full;

    // ── Word availability + selection ────────────────────────────
    // A live completion is a requested word arriving this cycle (busy low). The
    // slot's word is available when it is an early-fault (none needed), parked
    // in the skid, or completing live.
    logic        live_complete;
    logic        word_avail;
    logic [31:0] word_sel;
    assign live_complete = o_fetch_re & ~i_mem_busy;
    assign word_avail    = early_fault | skid_full | live_complete;
    assign word_sel      = skid_full ? skid_word : i_ir;

    // ── Outgoing fault composition ───────────────────────────────
    // A fetch is always an execute access. The late bus fault arrives with the
    // completion (or parked in the skid). Status by source: alignment and bus
    // compose here, the TLB status arrives composed from the MMU.
    logic bus_fault;
    assign bus_fault = skid_full ? skid_fault
                                 : (live_complete & i_mem_fault);
    assign o_enq_fault_pending = early_fault | bus_fault;
    always_comb begin
        if      (align_fault) o_enq_fault_status = compose_fault_status(i_user_mode, ACC_EXEC, FAULT_ALIGN);
        else if (tlb_fault)   o_enq_fault_status = i_mmu_fault_status;
        else if (bus_fault)   o_enq_fault_status = compose_fault_status(i_user_mode, ACC_EXEC, FAULT_BUS);
        else                  o_enq_fault_status = 32'b0;   // FAULT_NONE
    end
    assign o_enq_fault_vec = o_enq_fault_pending ? fault_vec_of(o_enq_fault_status[3:0]) : 4'd0;

    // ── Decode folds into the enqueue ────────────────────────────
    // Combinational word->bundle decode on the selected word; the FIFO flops
    // the result. A faulted slot's bundle is don't-care -- downstream gates on
    // o_enq_fault_pending.
    penumbra3_decode u_decode (
        .i_ir     (word_sel),
        .o_bundle (o_enq_bundle)
    );

    // ── Enqueue + back-pressure ──────────────────────────────────
    assign o_enq_pc      = i_pc;
    assign o_enq_next_pc = i_next_pc;
    assign o_enq_valid   = i_valid & word_avail & ~i_flush;

    logic enq_fire;
    assign enq_fire = o_enq_valid & i_enq_ready;

    // Hold IF1 unless the slot leaves (enqueues), is a bubble, or is flushed.
    // A slot waiting on the port (word not yet available) or on FIFO room (word
    // available but ~i_enq_ready) both fall under ~enq_fire.
    assign o_stall = i_valid & ~i_flush & ~enq_fire;

    // ── Skid capture / release ───────────────────────────────────
    // A live completion that cannot enqueue this cycle (FIFO not ready) must be
    // parked -- the port serves it for one cycle only. The parked word leaves
    // when it enqueues, when a flush drops it, or when its slot vanishes (IF1
    // bubbled it: a stale skid would otherwise poison the next slot).
    logic skid_capture, skid_release;
    assign skid_capture = live_complete & ~i_flush & ~i_enq_ready;
    assign skid_release = skid_full & (enq_fire | i_flush | ~i_valid);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            skid_full <= 1'b0;
        end else if (i_flush) begin
            skid_full <= 1'b0;          // wrong-path word: drop it
        end else if (skid_capture) begin
            skid_word  <= i_ir;
            skid_fault <= i_mem_fault;   // carry a faulted completion across the park
            skid_full  <= 1'b1;
        end else if (skid_release) begin
            skid_full <= 1'b0;
        end
    end

    // ── Assertions (sim-only; stripped at synth) ─────────────────
    // A flush discards the slot: nothing enqueues the cycle it is asserted.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_flush |-> !o_enq_valid)
        else $error("penumbra3_if2_stage: flush did not discard the enqueue");
    // The skid never coexists with an in-flight transaction: it is captured
    // only on a completion (busy low), and while it is full o_fetch_re is gated
    // off, so no new transaction can be outstanding.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(skid_full && i_mem_busy))
        else $error("penumbra3_if2_stage: skid held across a new transaction");
    // A faulted slot raises no fetch request.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_fetch_re && early_fault))
        else $error("penumbra3_if2_stage: request raised for an early-faulted slot");

endmodule
