// penumbra4_stage_ex — EX: computes the ops' results and the access's
// address, and resolves the transfer.
//
// Slot Y alone: ID fills no other. A result is the ALU's, the link a
// transfer writes, or the value RDSPR or WRSPR moves; an access's address
// is src_a's value plus its offset. A misprediction redirects fetch as the
// pair leaves, and the redirect's flush clears what issued behind it.
//
// EX carries neither MUL nor DIV. One that reaches it stops the simulation,
// and on a board takes the illegal-instruction trap, so the machine never
// runs on with a result EX did not compute.
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
                if (!y.op.fault.valid) begin
                    y_out.op.fault.valid    = 1'b1;
                    y_out.op.fault.vector   = VEC_ILLEGAL;
                    y_out.op.fault.mmu_type = FAULT_NONE;
                end
            end
            default: begin
            end
        endcase
    end

    assign leaving = y.valid && !i_hold && !i_flush;

    assign o_redirect.valid  = leaving && mispredict;
    assign o_redirect.target = next_pc;
    assign o_hold            = i_hold;

    pair_t pair_q, pair_d;

    always_comb begin
        pair_d = pair_q;
        if (i_flush) begin
            pair_d = '0;
        end else if (!i_hold) begin
            pair_d   = i_pair;
            pair_d.y = y_out;
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
            assert (y.op.op_class != OPC_FUNIT)
                else $fatal(1, "penumbra4_stage_ex: MUL or DIV at 0x%08x, which EX does not carry", y.op.pc);
            assert (!i_pair.x.valid)
                else $fatal(1, "penumbra4_stage_ex: an op in slot X, which EX does not carry");
        end
    end

endmodule
