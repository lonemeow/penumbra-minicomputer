// penumbra2_if2_stage — Penumbra/2.5 instruction fetch, second half.
//
// gen2.5 fork of the gen2 IF2 stage: identical to penumbra2/penumbra2_if2_stage
// except it carries the fetch-time BTB prediction tag (i_btb_predicted) on the
// slot, riding the IF2/ID register so the branch reaches EX knowing it was
// already redirected at fetch. The BTB itself lives in the core fork; this
// stage only forwards the bit in lockstep with the slot's PC (latched with
// o_pc on advance). gen2's IF2 has no such tag — that file is unchanged.
//
// Specified by the IF2 section of doc/internals/penumbra2/pipeline-stages.md.
// Receives the IF1/IF2 register (this fetch's PC, next_PC, valid) and the
// instruction word the I-cache BRAM presents this cycle — the word for the
// address IF1 drove last cycle — and latches the IF2/ID register for ID under
// the back-pressure handshake.
//
// The stage consumes the I-side MMU verdict (port A — the query is launched
// with IF1's fetch; the registered verdict is valid here and holds under a
// stall, so it stays paired with the held word). A misaligned fetch address
// or a translation fault tags the slot inert (fault_pending), with the
// composed, self-qualifying status carried alongside: alignment outranks the
// TLB verdict (per-access order in exception-flow.md) and is composed here —
// IF2 is its detector — while a TLB fault arrives composed from the MMU
// (Decision 16). The faulting vaddr needs no field of its own: it is the
// slot's own pc.
//
// IF2 owns the front side of the fetch request: a valid, un-faulted slot
// asserts o_fetch_re on its resolve cycle and holds it level until the word
// completes — the cycle i_mem_busy is low (drop-equals-valid: a hit completes
// on the resolve cycle itself; a miss or pass-through completes at the busy
// drop, possibly many cycles later). While waiting, IF2 back-pressures IF1
// (whose launch gate and PC hold track the same busy) and lets bubbles fall
// into ID. A faulted slot raises no request — its launch already happened
// (the VIPT overlap), but nothing may complete or fill on its behalf; it
// advances carrying only its fault tag. The VIPT I-cache behind the port
// indexes by this vaddr and tag-compares against the verdict's paddr, so
// non-identity translations deliver correctly; a flat stand-in (busy tied
// low, no tag compare) completes every fetch on its resolve cycle and is
// only correct under identity mapping — its integration's constraint, not
// this stage's.
//
// The completed word is consumed the cycle busy drops — a fill serves it for
// exactly that one cycle. If ID is stalled just then (scoreboard RAW, divmul,
// a D-side access in MEM), the word parks in a one-entry skid register and is
// delivered when ID accepts; the request deasserts once the skid is full, so
// the front port sees the completion consumed either way.
//
// A taken branch resolved in EX flushes the front end: IF2 is one of the three
// wrong-path slots (it holds the branch-shadow fetch). i_flush discards it by
// forcing the IF2/ID register to a bubble, and wins over back-pressure — the
// instruction is being thrown away, so holding it makes no sense.

// keep_hierarchy: hold this stage boundary through synth_ecp5 so the backward
// stall path (mem_stall -> ex_stall -> fetch_en) places compactly instead of
// smearing across the die, and reads with real names in timing reports.
// Paired across the pipeline stages (if1 / if2 / spine / mem_stage).
(* keep_hierarchy = "yes" *)
module penumbra2_if2_stage
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── IF1/IF2 register in ──────────────────────────────────────
    input  logic [31:0] i_pc,
    input  logic [31:0] i_next_pc,
    input  logic        i_valid,        // 0 = bubble in
    input  logic        i_btb_predicted, // gen2.5: this slot was BTB-predicted taken at fetch

    // ── I-side front port (the L1's front side, or the flat stand-in) ──
    // i_ir is the word for the launch IF1 drove last cycle, valid on the
    // completion cycle (i_mem_busy low); o_fetch_re is this stage's request,
    // held level from the resolve cycle until the completion is consumed.
    input  logic [31:0] i_ir,
    input  logic        i_mem_busy,
    input  logic        i_mem_fault,    // bus fault on the fetch, at the i_mem_busy drop
    output logic        o_fetch_re,

    // ── MMU I-side verdict (port A; query launched with IF1's fetch) ──
    // The paddr leg of the verdict goes to the I-cache's tag compare, not
    // here — this stage consumes only the fault verdict.
    input  logic        i_user_mode,        // fetch privilege (alignment status info)
    input  logic        i_mmu_fault,        // any translation fault (miss / protection)
    input  logic [31:0] i_mmu_fault_status, // composed by the MMU (Decision 16)

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic        i_stall_in,     // ID cannot accept this cycle
    input  logic        i_flush,        // taken-branch flush: bubble the IF2/ID slot
    output logic        o_stall,         // back-pressure to IF1

    // ── IF2/ID register out (to ID) ──────────────────────────────
    output logic [31:0] o_ir,
    output logic [31:0] o_pc,
    output logic [31:0] o_next_pc,
    output logic        o_valid,
    output logic        o_btb_predicted,    // gen2.5: the carried BTB-predicted tag
    output logic        o_fault_pending,
    output logic [3:0]  o_fault_vec,
    output logic [31:0] o_fault_status      // composed payload; FAULT_NONE when no fault
);

    // ── IF-stage fault detect + payload composition ──────────────
    // Only a real fetch consumes the verdict (a bubble's query is never
    // read). Faults split by when they are known:
    //   * Early faults — alignment (from the PC) and TLB (from the held
    //     verdict) — are known at resolve, before the fetch issues. The slot
    //     raises no request and advances carrying only its tag. Alignment
    //     outranks the TLB verdict: a misaligned address cannot be
    //     meaningfully translated.
    //   * A bus fault is late: it is only known when the requested word
    //     comes back faulted (i_mem_fault at the completion). So a bus-
    //     faulting slot *does* issue, waits out the port, and learns its
    //     fault on the busy-drop cycle (carried by the skid if ID is stalled
    //     just then). Composed below at the register where the word lands.
    // Every IF-side fault's FAULT_ADDR is the slot's own pc (set by MEM), and
    // its vector derives from the composed status type — the single
    // classification.
    logic        align_fault, tlb_fault, early_fault;
    assign align_fault = i_valid & (i_pc[1:0] != 2'b00);
    assign tlb_fault   = i_valid & i_mmu_fault & ~align_fault;
    assign early_fault = align_fault | tlb_fault;

    // ── Fetch request ────────────────────────────────────────────
    // A valid, un-faulted slot whose word is not already parked in the skid
    // requests it, level, from its resolve cycle until the completion is
    // consumed. A faulted slot raises no request: its launch happened (the
    // VIPT overlap) but nothing may complete — or engage a fill — on its
    // behalf; alignment in particular is invisible to the cache's own
    // i_fault gate, so the suppression has to live here. The ~i_flush term
    // skips the fill a wrong-path fetch would engage on a coincident
    // flush + miss resolve; a flush landing mid-fill drops the request and
    // the engaged fill completes into the void on the cache's own state —
    // the one consumer obligation that always survives the kill is "no new
    // launch while busy", and IF1's gate holds that.
    logic        skid_full, skid_fault;
    logic [31:0] skid_ir;
    assign o_fetch_re = i_valid & ~early_fault & ~skid_full & ~i_flush;

    // The slot can leave this cycle: its word is available (skid, or the
    // port completing — for a hit, the resolve cycle itself), or it carries
    // only an early-fault tag and never waits on the port.
    logic word_avail;
    assign word_avail = early_fault | skid_full | ~i_mem_busy;

    // ── Late bus fault + outgoing fault composition ──────────────
    // The requested word completes faulted (i_mem_fault on the busy-drop), or
    // a faulted completion was parked in the skid. Either way the slot leaves
    // carrying FAULT_BUS instead of its (garbage) word; a coincident flush
    // drops the request, so a wrong-path fetch never takes the fault.
    logic        bus_fault_deliver, out_fault_pending;
    logic [31:0] out_fault_status;
    assign bus_fault_deliver = skid_full ? skid_fault
                                         : (o_fetch_re & ~i_mem_busy & i_mem_fault);
    assign out_fault_pending = early_fault | bus_fault_deliver;
    // Status by source — a fetch is always an execute access (ACC_EXEC):
    // alignment and bus compose here, the TLB status arrives composed.
    always_comb begin
        if      (align_fault)       out_fault_status = compose_fault_status(i_user_mode, ACC_EXEC, FAULT_ALIGN);
        else if (tlb_fault)         out_fault_status = i_mmu_fault_status;
        else if (bus_fault_deliver) out_fault_status = compose_fault_status(i_user_mode, ACC_EXEC, FAULT_BUS);
        else                        out_fault_status = 32'b0;   // FAULT_NONE
    end

    // ── Issue / back-pressure control ────────────────────────────
    // A taken-branch flush wins over back-pressure: the in-flight fetch is
    // wrong-path and gets discarded, so there is nothing to hold (mirrors
    // MEM's i_bubble). An un-completed word (i_mem_busy with no skid) holds
    // IF1 and lets bubbles fall into ID — the I-side miss stall.
    logic advance, next_valid;

    always_comb begin
        if (i_flush) begin
            next_valid = 1'b0;          // flush wins: discard the wrong-path fetch
            advance    = 1'b0;
            o_stall    = i_stall_in | i_mem_busy;
        end else if (i_stall_in) begin
            next_valid = o_valid;       // hold IF2/ID unchanged
            advance    = 1'b0;
            o_stall    = 1'b1;
        end else begin
            next_valid = i_valid & word_avail;
            advance    = i_valid & word_avail;
            o_stall    = i_mem_busy;    // waiting word: hold IF1, bubble into ID
        end
    end

    // ── Word skid ────────────────────────────────────────────────
    // One entry, the completion-under-back-pressure parking spot: a fill
    // serves its word for exactly one cycle (drop-equals-valid), so a word
    // completing while ID back-pressures must be captured or lost. Owns
    // skid_full / skid_ir.

    // The requested word completes this cycle (busy low while the request
    // is up) but ID cannot accept it — park it.
    logic skid_capture;
    assign skid_capture = o_fetch_re & ~i_mem_busy & i_stall_in;

    // The parked word is done with: delivered to ID (advance muxes it into
    // the IF2/ID register), or its slot vanished without delivery (the
    // interrupt unit's fetch-stop bubbled IF1/IF2 — a stale skid would
    // otherwise poison the next slot).
    logic skid_release;
    assign skid_release = advance | ~i_valid;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            skid_full <= 1'b0;
        end else begin
            if (i_flush) begin              // wrong-path word: drop it
                skid_full <= 1'b0;
            end else if (skid_capture) begin
                skid_ir    <= i_ir;
                skid_fault <= i_mem_fault;   // carry a faulted completion across the park
                skid_full  <= 1'b1;
            end else if (skid_release) begin
                skid_full <= 1'b0;
            end
        end
    end

    // ── IF2/ID register ──────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid <= 1'b0;
        end else begin
            o_valid <= next_valid;
            if (advance) begin
                o_ir            <= skid_full ? skid_ir : i_ir;
                o_pc            <= i_pc;
                o_next_pc       <= i_next_pc;
                o_btb_predicted <= i_btb_predicted;   // rides the slot to ID (gen2.5)
                o_fault_pending <= out_fault_pending;
                o_fault_vec     <= out_fault_pending ? fault_vec_of(out_fault_status[3:0]) : 4'd0;
                o_fault_status  <= out_fault_status;
            end
        end
    end

    // ── Assertions (sim-only; stripped at synth) ─────────────────
    // A taken-branch flush must bubble the IF2/ID slot the next cycle — the
    // branch-shadow instruction must never reach ID and decode.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_flush |=> !o_valid)
        else $error("penumbra2_if2_stage: flush did not bubble the IF2/ID slot");

    // The skid never coexists with an in-flight transaction: it is captured
    // only under ID back-pressure (IF1 held, no launch), and delivering it
    // is what releases IF1 — busy can rise again only after the skid drains.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(skid_full && i_mem_busy))
        else $error("penumbra2_if2_stage: skid held across a new transaction");

endmodule
