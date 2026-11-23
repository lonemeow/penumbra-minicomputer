// penumbra4_resolve — where an op goes, and whether fetch went there.
//
// Combinational, for slot Y. A transfer that is taken goes to its target,
// from the instruction or, for JMP and JALR, from src_a's value; every
// other op goes to the word after it. Fetch went where the op's prediction
// says, so a difference is a misprediction, whatever the op is.

module penumbra4_resolve
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    // A slot carries more than resolve reads of it.
    /* verilator lint_off UNUSEDSIGNAL */
    input  slot_t  i_slot,
    /* verilator lint_on UNUSEDSIGNAL */
    output vaddr_t o_next_pc,
    output logic   o_mispredict
);

    /* verilator lint_off UNUSEDSIGNAL */
    op_t    op;
    /* verilator lint_on UNUSEDSIGNAL */
    logic   condition;
    logic   taken;
    vaddr_t fall_through;
    vaddr_t predicted;

    assign op           = i_slot.op;
    assign fall_through = op.pc + vaddr_t'(4);

    cond_eval u_cond_eval (
        .i_flag_z (i_slot.flags.z),
        .i_flag_n (i_slot.flags.n),
        .i_flag_c (i_slot.flags.c),
        .i_flag_v (i_slot.flags.v),
        .i_cond   (op.cond),
        .o_taken  (condition)
    );

    assign taken = (op.op_class == OPC_BRANCH) && (op.indirect || condition);

    always_comb begin
        if (!taken) begin
            o_next_pc = fall_through;
        end else if (op.indirect) begin
            o_next_pc = vaddr_t'(i_slot.a);
        end else begin
            o_next_pc = op.target;
        end

        if (op.prediction.taken) begin
            predicted = op.prediction.target;
        end else begin
            predicted = fall_through;
        end
    end

    assign o_mispredict = i_slot.valid && (o_next_pc != predicted);

endmodule
