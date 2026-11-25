// penumbra4_stage_if2 — IF2: turns the line the L1-I returns into ops for
// the fetch queue.
//
// A fetch gives the words from its address to the end of its line: those
// its lookup hit, or, for a fetch the L1-I's last access answers, that
// access's line, or the one word of a word read. A fetch with no word asks
// the L1-I for the access and IF1 to relaunch it once the access is done;
// a word read's fetch, and one the queue has no room for, relaunch at the
// first word they leave. A faulted fetch gives one op, carrying its fault.
//
// Combinational: the queue's storage is this stage's output register.

module penumbra4_stage_if2
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  fetch_slot_t                i_slot,
    input  logic                       i_flush,

    // ── The L1-I's lookup verdict and last access ────────────────
    input  logic                       i_hit,
    input  line_t                      i_line,
    input  line_t                      i_access_line,
    input  logic                       i_access_fault,

    // ── The MMU's fetch verdict ──────────────────────────────────
    input  paddr_t                     i_paddr,
    input  logic                       i_cacheable,
    input  fault_type_t                i_fault,

    // ── The fetch queue ──────────────────────────────────────────
    input  logic                       i_queue_room,
    output logic                       o_enq,
    output op_t [LINE_WORDS-1:0]       o_ops,
    output line_count_t                o_count,

    // ── The L1-I's access ────────────────────────────────────────
    output logic                       o_miss_req,
    output paddr_t                     o_miss_addr,
    output logic                       o_miss_cacheable,

    output relaunch_t                  o_relaunch
);

    line_word_t first;
    line_count_t to_line_end;
    line_t      words;          // the line's words from the fetch's address on
    fault_t     fetch_fault;

    assign first       = line_word_of(i_slot.pc);
    assign to_line_end = line_count_t'(LINE_WORDS) - line_count_t'(first);

    always_comb begin
        fetch_fault = '0;
        if (i_slot.pc[1:0] != 2'b00) begin
            fetch_fault.valid    = 1'b1;
            fetch_fault.vector   = VEC_ALIGN;
            fetch_fault.mmu_type = FAULT_ALIGN;
            fetch_fault.access   = ACC_EXEC;
        end else if (i_fault != FAULT_NONE) begin
            fetch_fault.valid    = 1'b1;
            fetch_fault.vector   = fault_vec_of(i_fault);
            fetch_fault.mmu_type = i_fault;
            fetch_fault.access   = ACC_EXEC;
        end else if (i_slot.from_access && i_access_fault) begin
            fetch_fault.valid    = 1'b1;
            fetch_fault.vector   = VEC_BUS_FAULT;
            fetch_fault.mmu_type = FAULT_BUS;
            fetch_fault.access   = ACC_EXEC;
        end
    end

    always_comb begin
        for (int k = 0; k < LINE_WORDS; k++) begin
            if (i_slot.from_access) begin
                words[k] = i_access_line[line_word_t'(first + line_word_t'(k))];
            end else begin
                words[k] = i_line[line_word_t'(first + line_word_t'(k))];
            end
        end
    end

    for (genvar k = 0; k < LINE_WORDS; k++) begin : g_decode
        penumbra4_decode u_decode (
            .i_insn        (insn_t'(words[k])),
            .i_pc          (i_slot.pc + vaddr_t'(4 * k)),
            .i_prediction  ('0),
            .i_fetch_fault (fetch_fault),
            .o_op          (o_ops[k])
        );
    end

    always_comb begin
        o_enq            = 1'b0;
        o_count          = '0;
        o_miss_req       = 1'b0;
        o_miss_addr      = i_paddr;
        o_miss_cacheable = i_cacheable;
        o_relaunch       = '0;

        if (i_slot.valid) begin
            if (!i_queue_room) begin
                o_relaunch.valid       = 1'b1;
                o_relaunch.pc          = i_slot.pc;
                o_relaunch.from_access = i_slot.from_access;
            end else if (fetch_fault.valid) begin
                o_enq   = 1'b1;
                o_count = line_count_t'(1);
            end else if (i_slot.from_access) begin
                o_enq = 1'b1;
                if (i_cacheable) begin
                    o_count = to_line_end;
                end else begin
                    o_count          = line_count_t'(1);
                    o_relaunch.valid = (to_line_end != line_count_t'(1));
                    o_relaunch.pc    = i_slot.pc + vaddr_t'(4);
                end
            end else if (i_hit) begin
                o_enq   = 1'b1;
                o_count = to_line_end;
            end else begin
                o_miss_req             = 1'b1;
                o_relaunch.valid       = 1'b1;
                o_relaunch.pc          = i_slot.pc;
                o_relaunch.from_access = 1'b1;
            end
        end
        // A flush stops what acts outside the queue, the L1-I's access and
        // IF1's relaunch; the queue drops an enqueue itself.
        if (i_flush) begin
            o_miss_req = 1'b0;
            o_relaunch = '0;
        end
    end

endmodule
