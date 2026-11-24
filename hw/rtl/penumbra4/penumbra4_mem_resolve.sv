// penumbra4_mem_resolve — MEM2: takes the access's verdicts and finishes
// the access or hands it to the L1-D.
//
// A translation fault is the access's, unless MEM1 found one first. A load
// the L1-D hit takes its word. Any other access, a load the L1-D had no
// word for or an uncacheable store's write, is handed to the L1-D as the
// pair leaves, and the slot says so: commit holds the pair, so the pair
// waits in this stage's register, which is the one commit decides from.
// The L1-D's word, or its bus fault, enters the slot there at its done
// (doc/internals/penumbra4/structure.md, "A missed load holds its pair at
// commit").
//
// A loaded sub-word is extracted and extended here, from the hit's word or
// the access's.
//
// The output register is the pair commit takes.

module penumbra4_mem_resolve
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    input  pair_t       i_pair,
    input  logic        i_kill,
    input  logic        i_hold,
    output logic        o_hold,

    // ── The verdicts on MEM1's launch ────────────────────────────
    input  paddr_t      i_paddr,
    input  logic        i_cacheable,
    input  fault_type_t i_fault,
    input  logic        i_hit,
    input  word_t       i_word,

    // ── The access the L1-D carries ──────────────────────────────
    output logic        o_access_req,
    output uncore_req_t o_access,
    input  logic        i_access_done,
    input  word_t       i_access_word,
    input  logic        i_access_fault,

    output pair_t       o_pair
);

    pair_t pair_q, pair_d;

    function automatic access_t access_of(logic store);
        if (store) begin
            return ACC_WRITE;
        end
        return ACC_READ;
    endfunction

    // ── The verdicts ─────────────────────────────────────────────
    slot_t y;
    slot_t y_out;
    logic  is_access;
    logic  needs_access;
    logic  leaving;
    word_t hit_value;

    assign y         = i_pair.y;
    assign is_access = y.valid && (y.op.op_class == OPC_MEM) && !y.op.fault.valid;
    assign leaving   = y.valid && !i_hold && !i_kill;

    byte_ext u_extract_hit (
        .i_data     (i_word),
        .i_addr_lo  (y.addr[1:0]),
        .i_size     (y.op.size),
        .i_sign_ext (y.op.sign_extend),
        .o_data     (hit_value)
    );

    always_comb begin
        y_out        = y;
        needs_access = 1'b0;
        if (is_access) begin
            if (i_fault != FAULT_NONE) begin
                y_out.op.fault.valid    = 1'b1;
                y_out.op.fault.vector   = fault_vec_of(i_fault);
                y_out.op.fault.mmu_type = i_fault;
                y_out.op.fault.access   = access_of(y.op.store);
            end else if (!y.op.store && i_hit) begin
                y_out.result = hit_value;
            end else begin
                needs_access         = 1'b1;
                y_out.access_pending = 1'b1;
            end
        end
    end

    assign o_access_req       = leaving && needs_access;
    assign o_access.cacheable = i_cacheable;
    assign o_access.addr      = i_paddr;
    assign o_access.we        = y.op.store;
    assign o_access.byte_en   = y.byte_en;
    assign o_access.wdata     = y.wdata;

    // ── The access's result, into the waiting slot ───────────────
    slot_t waiting;
    word_t access_value;

    byte_ext u_extract_access (
        .i_data     (i_access_word),
        .i_addr_lo  (pair_q.y.addr[1:0]),
        .i_size     (pair_q.y.op.size),
        .i_sign_ext (pair_q.y.op.sign_extend),
        .o_data     (access_value)
    );

    always_comb begin
        waiting                = pair_q.y;
        waiting.access_pending = 1'b0;
        if (i_access_fault) begin
            waiting.op.fault.valid    = 1'b1;
            waiting.op.fault.vector   = VEC_BUS_FAULT;
            waiting.op.fault.mmu_type = FAULT_BUS;
            waiting.op.fault.access   = access_of(pair_q.y.op.store);
        end else if (!pair_q.y.op.store) begin
            waiting.result = access_value;
        end
    end

    always_comb begin
        pair_d = pair_q;
        if (i_kill) begin
            pair_d = '0;
        end else if (!i_hold) begin
            pair_d   = i_pair;
            pair_d.y = y_out;
        end else if (pair_q.y.access_pending && i_access_done) begin
            pair_d.y = waiting;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            pair_q <= '0;
        end else begin
            pair_q <= pair_d;
        end
    end

    assign o_hold = i_hold;
    assign o_pair = pair_q;

    // ── Contract ─────────────────────────────────────────────────
    // The store side, which carries a cacheable store's write, is not built;
    // with translation off no store is cacheable.
    always_ff @(posedge i_clk) begin
        if (!i_rst && o_access_req) begin
            assert (!(y.op.store && i_cacheable))
                else $fatal(1, "penumbra4_mem_resolve: a cacheable store at 0x%08x, which the store side carries",
                            y.op.pc);
        end
    end

endmodule
