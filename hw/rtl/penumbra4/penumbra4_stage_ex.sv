// penumbra4_stage_ex — EX: computes the ops' results and the access's
// address, and resolves the transfer.
//
// Slot Y alone: ID fills no other. A result is the ALU's, the link a
// transfer writes, or the value RDSPR or WRSPR moves; an access's address
// is src_a's value plus its offset. A misprediction redirects fetch as the
// pair leaves, and the redirect's flush clears what issued behind it.
//
// MUL and DIV run on the shared divmul unit, which EX starts as the op
// arrives and waits on with the op held here: a hold of EX's own, from
// registered state (doc/internals/penumbra4/structure.md, "The stage
// contract"). MEM1 takes an empty slot meanwhile. Both halves leave in the
// slot, and a divide by zero takes VEC_ARITH (doc/internals/divmul.md,
// "Completion Signalling").
//
// The output register is the pair MEM1 launches.

module penumbra4_stage_ex
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic      i_clk,
    input  logic      i_rst,

    input  pair_t     i_pair,
    input  logic      i_flush,
    input  logic      i_hold,
    output logic      o_hold,

    output redirect_t o_redirect,
    output pair_t     o_pair
);

    slot_t  y;
    word_t  alu_b;
    word_t  alu_result;
    flags_t alu_flags;
    vaddr_t next_pc;
    logic   mispredict;
    logic   leaving;

    assign y = i_pair.y;

    // ── MUL and DIV ──────────────────────────────────────────────
    // divmul's own op encoding, which its sequence decodes.
    localparam logic [4:0] DIVMUL_OP_MUL  = 5'b01101;
    localparam logic [4:0] DIVMUL_OP_MULU = 5'b01110;
    localparam logic [4:0] DIVMUL_OP_DIV  = 5'b01111;
    localparam logic [4:0] DIVMUL_OP_DIVU = 5'b10000;

    logic       is_divmul;
    logic [4:0] divmul_op;
    logic       divmul_start;
    logic       divmul_idle_q;    // o_done: no operation in flight
    logic       divmul_fault_q;   // the unit's flops, both
    word_t      divmul_lo, divmul_hi;
    logic       divmul_z, divmul_n;
    logic       divmul_wait;

    // OPC_FUNIT is any peer unit's class; funit_op names the unit.
    assign is_divmul = y.valid && (y.op.op_class == OPC_FUNIT)
                    && (y.op.funit_op inside {OP_R_MUL, OP_R_MULU, OP_R_DIV, OP_R_DIVU});

    always_comb begin
        case (y.op.funit_op)
            OP_R_MUL:  divmul_op = DIVMUL_OP_MUL;
            OP_R_MULU: divmul_op = DIVMUL_OP_MULU;
            OP_R_DIV:  divmul_op = DIVMUL_OP_DIV;
            default:   divmul_op = DIVMUL_OP_DIVU;
        endcase
    end

    divmul #(
        .MUL_BITS_PER_STEP (4)
    ) u_divmul (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_a         (y.a),
        .i_b         (y.b),
        .i_op        (divmul_op),
        .i_start     (divmul_start),
        .o_done      (divmul_idle_q),
        .o_fault     (divmul_fault_q),
        .o_result_lo (divmul_lo),
        .o_result_hi (divmul_hi),
        .o_flag_z    (divmul_z),
        .o_flag_n    (divmul_n)
    );

    // The unit has the op in EX from the cycle EX starts it until that op
    // leaves EX or is flushed, whether or not it has finished: a finished
    // op held here by a later stage keeps it, so it is never started again.
    logic divmul_has_op_q, divmul_has_op_d;

    assign divmul_start = !i_flush && is_divmul && !divmul_has_op_q && divmul_idle_q;
    assign divmul_wait  = divmul_start || !divmul_idle_q;

    always_comb begin
        divmul_has_op_d = divmul_has_op_q;
        if      (i_flush)      divmul_has_op_d = 1'b0;
        else if (divmul_start) divmul_has_op_d = 1'b1;
        else if (leaving)      divmul_has_op_d = 1'b0;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            divmul_has_op_q <= 1'b0;
        end else begin
            divmul_has_op_q <= divmul_has_op_d;
        end
    end

    always_comb begin
        if (y.op.b_is_imm) begin
            alu_b = y.op.imm;
        end else begin
            alu_b = y.b;
        end
    end

    penumbra4_alu u_alu_y (
        .i_fn     (y.op.alu_fn),
        .i_a      (y.a),
        .i_b      (alu_b),
        .i_carry  (y.flags.c),
        .o_result (alu_result),
        .o_flags  (alu_flags)
    );

    penumbra4_resolve u_resolve (
        .i_slot       (y),
        .o_next_pc    (next_pc),
        .o_mispredict (mispredict)
    );

    slot_t y_out;

    always_comb begin
        y_out = y;
        case (y.op.op_class)
            OPC_ALU: begin
                y_out.result = alu_result;
                if (y.op.writes_flags) begin
                    y_out.flags = alu_flags;
                end
            end
            OPC_BRANCH: y_out.result = word_t'(y.op.pc + vaddr_t'(4));
            OPC_MEM:    y_out.addr   = vaddr_t'(alu_result);
            OPC_RDSPR:  y_out.result = y.a;
            OPC_WRSPR:  y_out.result = y.b;
            OPC_FUNIT: begin
                if (is_divmul) begin
                    y_out.result  = divmul_lo;
                    y_out.result2 = divmul_hi;
                    y_out.flags   = '{v: 1'b0, c: 1'b0, z: divmul_z, n: divmul_n};
                    if (divmul_fault_q && !y.op.fault.valid) begin
                        y_out.op.fault.valid    = 1'b1;
                        y_out.op.fault.vector   = VEC_ARITH;
                        y_out.op.fault.mmu_type = FAULT_NONE;
                    end
                end
            end
            default: begin
            end
        endcase
    end

    assign leaving = y.valid && !i_hold && !divmul_wait && !i_flush;

    assign o_redirect.valid  = leaving && mispredict;
    assign o_redirect.target = next_pc;
    assign o_hold            = i_hold || divmul_wait;

    pair_t pair_q, pair_d;

    always_comb begin
        pair_d = pair_q;
        if (i_flush) begin
            pair_d = '0;
        end else if (!i_hold) begin
            if (divmul_wait) begin
                pair_d = '0;
            end else begin
                pair_d   = i_pair;
                pair_d.y = y_out;
            end
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            pair_q <= '0;
        end else begin
            pair_q <= pair_d;
        end
    end

    assign o_pair = pair_q;

    // ── Contract ─────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst && y.valid) begin
            assert (!i_pair.x.valid)
                else $fatal(1, "penumbra4_stage_ex: an op in slot X, which EX does not carry");
            assert (y.op.op_class != OPC_FUNIT || is_divmul)
                else $fatal(1, "penumbra4_stage_ex: a peer-unit op at 0x%08x for a unit EX does not have",
                            y.op.pc);
        end
        if (!i_rst) begin
            assert (!(leaving && is_divmul) || (divmul_has_op_q && divmul_idle_q))
                else $fatal(1, "penumbra4_stage_ex: a MUL or DIV at 0x%08x left EX without the unit's result for it",
                            y.op.pc);
            assert (!divmul_start || (is_divmul && !divmul_has_op_q))
                else $fatal(1, "penumbra4_stage_ex: the unit started at 0x%08x for no MUL or DIV, or for one it has",
                            y.op.pc);
            assert (!divmul_has_op_q || is_divmul)
                else $fatal(1, "penumbra4_stage_ex: the unit has an op while EX holds no MUL or DIV");
            // The unit takes operands only when idle: a start while it still
            // works on an op a flush dropped would leave it that op's results.
            assert (!divmul_start || divmul_idle_q)
                else $fatal(1, "penumbra4_stage_ex: the unit started at 0x%08x while still busy with a dropped op",
                            y.op.pc);
        end
    end

    // From its start until the op leaves EX or is flushed, the unit has it.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (divmul_start && !i_flush) |=> divmul_has_op_q)
        else $fatal(1, "penumbra4_stage_ex: the unit was started but does not have the op");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (divmul_has_op_q && !leaving && !i_flush) |=> divmul_has_op_q)
        else $fatal(1, "penumbra4_stage_ex: the unit gave up the op in EX before the op left");

endmodule
