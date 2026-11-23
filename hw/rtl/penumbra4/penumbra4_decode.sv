// penumbra4_decode — an instruction's fields, as an op of one.
//
// Combinational. A destination of R0 is no write, and a source of R0 is no
// read, so neither reaches the hazard compares; a source of R15 keeps its
// reference, and the stage that captures it takes the op's PC. A form no
// legal instruction takes, R15 as a destination among them, is an op of
// class OPC_TRAP with VEC_ILLEGAL. An instruction whose fetch faulted
// decodes to its fault alone.

module penumbra4_decode
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  insn_t       i_insn,
    input  vaddr_t      i_pc,
    input  prediction_t i_prediction,
    input  fault_t      i_fetch_fault,
    output op_t         o_op
);

    function automatic reg_ref_t reg_ref(reg_index_t index);
        reg_ref_t ref_;
        ref_.en    = (index != REG_ZERO);
        ref_.user  = 1'b0;
        ref_.index = index;
        return ref_;
    endfunction

    function automatic fault_t trap_of(vector_t vector);
        fault_t fault;
        fault          = '0;
        fault.valid    = 1'b1;
        fault.vector   = vector;
        fault.mmu_type = FAULT_NONE;
        return fault;
    endfunction

    // An op that does nothing but take its fault.
    function automatic op_t trap_op(fault_t fault, vaddr_t pc, prediction_t prediction);
        op_t trap;
        trap            = '0;
        trap.op_class   = OPC_TRAP;
        trap.alu_fn     = ALU_PASS;
        trap.fault      = fault;
        trap.pc         = pc;
        trap.prediction = prediction;
        return trap;
    endfunction

    localparam reg_ref_t USP_REF = '{en: 1'b1, user: 1'b1, index: REG_SP};

    op_t op;

    always_comb begin
        op = trap_op(trap_of(VEC_ILLEGAL), i_pc, i_prediction);

        unique case (i_insn.head.fmt)
            // ── Format R ────────────────────────────────────────
            FMT_R: begin
                if (i_insn.r.op <= OP_R_SBC) begin
                    op.op_class     = OPC_ALU;
                    op.fault        = '0;
                    op.alu_fn       = alu_fn_e'(i_insn.r.op[3:0]);
                    op.src_b        = reg_ref(i_insn.r.rs);
                    op.writes_flags = 1'b1;
                    if (!i_insn.r.flags_only) begin
                        op.dst = reg_ref(i_insn.r.rd);
                    end
                    // MOV and NOT take Rs alone; MOV keeps the flags.
                    if (i_insn.r.op == OP_R_MOV) begin
                        op.writes_flags = 1'b0;
                    end else if (i_insn.r.op != OP_R_NOT) begin
                        op.src_a = reg_ref(i_insn.r.rd);
                    end
                    if (i_insn.r.op == OP_R_ADC || i_insn.r.op == OP_R_SBC) begin
                        op.reads_flags = 1'b1;
                    end
                end else begin
                    case (i_insn.r.op)
                        OP_R_MUL, OP_R_MULU, OP_R_DIV, OP_R_DIVU: begin
                            op.op_class     = OPC_FUNIT;
                            op.fault        = '0;
                            op.funit_op     = i_insn.r.op;
                            op.src_a        = reg_ref(i_insn.divmul.rd);
                            op.src_b        = reg_ref(i_insn.divmul.rs);
                            op.dst          = reg_ref(i_insn.divmul.rd);
                            op.dst2         = reg_ref(i_insn.divmul.rdh);
                            op.writes_flags = 1'b1;
                        end
                        OP_R_WRSYS: begin
                            op.op_class   = OPC_WRSYS;
                            op.fault      = '0;
                            op.src_b      = reg_ref(i_insn.sys.rd);
                            op.sysdev     = i_insn.sys.dev;
                            op.sysreg     = i_insn.sys.sysreg;
                            op.privileged = 1'b1;
                        end
                        OP_R_RDSYS: begin
                            op.op_class   = OPC_RDSYS;
                            op.fault      = '0;
                            op.dst        = reg_ref(i_insn.sys.rd);
                            op.sysdev     = i_insn.sys.dev;
                            op.sysreg     = i_insn.sys.sysreg;
                            op.privileged = 1'b1;
                        end
                        OP_R_SYSCALL: begin
                            op.fault = trap_of(VEC_SYSCALL);
                        end
                        OP_R_BREAK: begin
                            op.fault = trap_of(VEC_BREAK);
                        end
                        OP_R_ERET: begin
                            op.op_class     = OPC_ERET;
                            op.fault        = '0;
                            op.writes_flags = 1'b1;
                            op.privileged   = 1'b1;
                        end
                        OP_R_EI, OP_R_DI: begin
                            op.op_class   = OPC_IRQ_MASK;
                            op.fault      = '0;
                            op.irq_enable = (i_insn.r.op == OP_R_EI);
                            op.privileged = 1'b1;
                        end
                        OP_R_WRSPR: begin
                            // SR has no direct write: the encoding is reserved.
                            if (i_insn.spr.spr != SPR_SR && i_insn.spr.spr <= SPR_SCR3) begin
                                op.op_class   = OPC_WRSPR;
                                op.fault      = '0;
                                op.spr        = i_insn.spr.spr;
                                op.src_b      = reg_ref(i_insn.spr.rd);
                                op.privileged = 1'b1;
                                if (i_insn.spr.spr == SPR_USP) begin
                                    op.dst = USP_REF;
                                end
                            end
                        end
                        OP_R_RDSPR: begin
                            if (i_insn.spr.spr <= SPR_SCR3) begin
                                op.op_class   = OPC_RDSPR;
                                op.fault      = '0;
                                op.spr        = i_insn.spr.spr;
                                op.dst        = reg_ref(i_insn.spr.rd);
                                op.privileged = 1'b1;
                                if (i_insn.spr.spr == SPR_USP) begin
                                    op.src_a = USP_REF;
                                end else if (i_insn.spr.spr == SPR_SR) begin
                                    op.reads_flags = 1'b1;
                                end
                            end
                        end
                        default: begin
                        end
                    endcase
                end
            end

            // ── Format L ────────────────────────────────────────
            FMT_L: begin
                op.op_class = OPC_ALU;
                op.fault    = '0;
                op.b_is_imm = 1'b1;
                op.imm      = {16'b0, i_insn.l.imm16};
                case (i_insn.l.op)
                    OP_L_LLI: begin
                        op.dst = reg_ref(i_insn.l.rd);
                    end
                    OP_L_LLIS: begin
                        op.imm = {{16{i_insn.l.imm16[15]}}, i_insn.l.imm16};
                        op.dst = reg_ref(i_insn.l.rd);
                    end
                    OP_L_LUI: begin
                        op.alu_fn = ALU_OR;
                        op.imm    = {i_insn.l.imm16, 16'b0};
                        op.src_a  = reg_ref(i_insn.l.rd);
                        op.dst    = reg_ref(i_insn.l.rd);
                    end
                    OP_L_ADDI, OP_L_SUBI, OP_L_ANDI,
                    OP_L_SHLI, OP_L_SHRI, OP_L_SARI: begin
                        op.src_a        = reg_ref(i_insn.l.rd);
                        op.dst          = reg_ref(i_insn.l.rd);
                        op.writes_flags = 1'b1;
                        case (i_insn.l.op)
                            OP_L_ADDI: op.alu_fn = ALU_ADD;
                            OP_L_SUBI: op.alu_fn = ALU_SUB;
                            OP_L_ANDI: op.alu_fn = ALU_AND;
                            OP_L_SHLI: op.alu_fn = ALU_SHL;
                            OP_L_SHRI: op.alu_fn = ALU_SHR;
                            default:   op.alu_fn = ALU_SAR;
                        endcase
                        if (i_insn.l.op inside {OP_L_SHLI, OP_L_SHRI, OP_L_SARI}) begin
                            op.imm = {27'b0, i_insn.l.imm16[4:0]};
                        end
                    end
                    OP_L_CMPI: begin
                        op.alu_fn       = ALU_SUB;
                        op.src_a        = reg_ref(i_insn.l.rd);
                        op.writes_flags = 1'b1;
                    end
                    OP_L_TESTI: begin
                        op.alu_fn       = ALU_AND;
                        op.src_a        = reg_ref(i_insn.l.rd);
                        op.writes_flags = 1'b1;
                    end
                    OP_L_JMP, OP_L_JALR: begin
                        op.op_class = OPC_BRANCH;
                        op.b_is_imm = 1'b0;
                        op.imm      = '0;
                        op.cond     = COND_AL;
                        op.indirect = 1'b1;
                        op.src_a    = reg_ref(i_insn.l.rd);
                        if (i_insn.l.op == OP_L_JALR) begin
                            op.dst = reg_ref(REG_LR);
                        end
                    end
                    default: begin
                        op = trap_op(trap_of(VEC_ILLEGAL), i_pc, i_prediction);
                    end
                endcase
            end

            // ── Format M ────────────────────────────────────────
            FMT_M: begin
                op.op_class    = OPC_MEM;
                op.fault       = '0;
                op.alu_fn      = ALU_ADD;
                op.b_is_imm    = 1'b1;
                op.imm         = {{16{i_insn.m.offset16[15]}}, i_insn.m.offset16};
                op.src_a       = reg_ref(i_insn.m.rb);
                op.size        = i_insn.m.size;
                op.sign_extend = i_insn.m.sign_extend;
                op.store       = !i_insn.m.load;
                if (i_insn.m.load) begin
                    op.dst   = reg_ref(i_insn.m.rd);
                end else begin
                    op.src_b = reg_ref(i_insn.m.rd);
                end
            end

            // ── Format B ────────────────────────────────────────
            FMT_B: begin
                op.op_class = OPC_BRANCH;
                op.fault    = '0;
                op.cond     = i_insn.b.cond;
                op.target   = i_pc + vaddr_t'({{8{i_insn.b.offset22[21]}}, i_insn.b.offset22, 2'b00});
                if (i_insn.b.cond != COND_AL && i_insn.b.cond != COND_BL) begin
                    op.reads_flags = 1'b1;
                end
                if (i_insn.b.cond == COND_BL) begin
                    op.dst = reg_ref(REG_LR);
                end
            end
        endcase

        // The PC is written by control flow alone.
        if ((op.dst.en && !op.dst.user && op.dst.index == REG_PC)
         || (op.dst2.en && op.dst2.index == REG_PC)) begin
            op = trap_op(trap_of(VEC_ILLEGAL), i_pc, i_prediction);
        end

        if (i_fetch_fault.valid) begin
            op = trap_op(i_fetch_fault, i_pc, i_prediction);
        end
    end

    assign o_op = op;

endmodule
