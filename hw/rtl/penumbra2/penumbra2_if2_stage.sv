// penumbra2_if2_stage — Penumbra/2 instruction fetch, second half.
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
// slot's own pc. The cache tag compare and the miss stall arrive with the
// BRAM L1; fetch is hit-always against the flat vaddr-addressed stand-in, so
// a completing translated fetch must be identity-mapped (asserted below)
// until the VIPT I-cache's tag compare delivers remapped words. IF2's only
// stall source is back-pressure from ID (i_stall_in); when held, it freezes
// its output register and back-pressures IF1, which holds the PC and the
// address so the same word stays presented.
//
// A taken branch resolved in EX flushes the front end: IF2 is one of the three
// wrong-path slots (it holds the branch-shadow fetch). i_flush discards it by
// forcing the IF2/ID register to a bubble, and wins over back-pressure — the
// instruction is being thrown away, so holding it makes no sense.

module penumbra2_if2_stage
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── IF1/IF2 register in ──────────────────────────────────────
    input  logic [31:0] i_pc,
    input  logic [31:0] i_next_pc,
    input  logic        i_valid,        // 0 = bubble in

    // ── Instruction word from the I-cache BRAM ───────────────────
    input  logic [31:0] i_ir,

    // ── MMU I-side verdict (port A; query launched with IF1's fetch) ──
    input  logic        i_user_mode,        // fetch privilege (alignment status info)
    input  logic [31:0] i_mmu_paddr,
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
    output logic        o_fault_pending,
    output logic [3:0]  o_fault_vec,
    output logic [31:0] o_fault_status      // composed payload; FAULT_NONE when no fault
);

    // ── Issue / back-pressure control ────────────────────────────
    // IF2 has no stall source of its own yet (hit-always, no miss path),
    // so it advances every cycle unless ID back-pressures it. A taken-branch
    // flush wins over back-pressure: the in-flight fetch is wrong-path and
    // gets discarded, so there is nothing to hold (mirrors MEM's i_bubble).
    logic advance, next_valid;

    always_comb begin
        if (i_flush) begin
            next_valid = 1'b0;          // flush wins: discard the wrong-path fetch
            advance    = 1'b0;
            o_stall    = i_stall_in;    // still pass upstream back-pressure through
        end else if (i_stall_in) begin
            next_valid = o_valid;       // hold IF2/ID unchanged
            advance    = 1'b0;
            o_stall    = 1'b1;
        end else begin
            next_valid = i_valid;       // advance: bubble in if i_valid=0
            advance    = i_valid;
            o_stall    = 1'b0;
        end
    end

    // ── IF-stage fault detect + payload composition ──────────────
    // Only a real fetch consumes the verdict (a bubble's query is never
    // read). Alignment outranks the TLB verdict — a misaligned address
    // cannot be meaningfully translated; the vector derives from the
    // composed status type, the single classification.
    logic        align_fault, tlb_fault, fault_pending;
    logic [31:0] fault_status;
    assign align_fault   = i_valid & (i_pc[1:0] != 2'b00);
    assign tlb_fault     = i_valid & i_mmu_fault & ~align_fault;
    assign fault_pending = align_fault | tlb_fault;
    assign fault_status  = align_fault ? {20'b0, i_user_mode, ACC_EXEC, 4'b0, FAULT_ALIGN}
                         : tlb_fault   ? i_mmu_fault_status
                         :               32'b0;   // FAULT_NONE

    // ── IF2/ID register ──────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid <= 1'b0;
        end else begin
            o_valid <= next_valid;
            if (advance) begin
                o_ir            <= i_ir;
                o_pc            <= i_pc;
                o_next_pc       <= i_next_pc;
                o_fault_pending <= fault_pending;
                o_fault_vec     <= fault_pending ? fault_vec_of(fault_status[3:0]) : 4'd0;
                o_fault_status  <= fault_status;
            end
        end
    end

    // ── Assertions (sim-only; stripped at synth) ─────────────────
    // The flat fetch stand-in is vaddr-indexed with no paddr tag compare: a
    // completing translated fetch must be identity-mapped until the VIPT
    // I-cache's tag compare delivers remapped words (mirror of the MEM-side
    // data assertion).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (advance && !fault_pending) |-> (i_mmu_paddr == i_pc))
        else $error("penumbra2_if2_stage: non-identity I-mapping over the flat fetch stand-in");

    // A taken-branch flush must bubble the IF2/ID slot the next cycle — the
    // branch-shadow instruction must never reach ID and decode.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_flush |=> !o_valid)
        else $error("penumbra2_if2_stage: flush did not bubble the IF2/ID slot");

endmodule
