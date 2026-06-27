// penumbra3_decode -- Penumbra/3 word->bundle decoder (fetch-FIFO enqueue).
//
// Combinational. Turns a 32-bit instruction word into a ctrl_bundle_t: the
// architectural register references, the EX operand-mux / ALU controls, the
// extended immediate, the memory / sysreg / SPR controls, the writeback
// enable, and the decode-time exception predicates. The instruction
// encoding it decodes is fixed by doc/system/instruction-encoding.md.
//
// gen3 places this on the fetch-FIFO *enqueue* path, not in ID: the heavy
// decode runs once in the fetch domain (which has slack) and ID then reads a
// pre-decoded bundle, so its hazard cone starts from registered fields. Two
// consequences follow:
//
//  - It is a pure function of i_ir -- no i_supervisor. Everything mode-
//    dependent (the arch->phys regmap, the privilege fault, the fault
//    vector's VEC_PRIV arm) stays in ID, where the live mode is unambiguous.
//    This decoder only marks priv_op; ID turns that into a fault.
//
//  - It is written parallel-then-select: all four formats decode off the raw
//    word concurrently and the only serial step is the final mux on the
//    format bits. (Select-then-decode made ID a critical cone in gen2.5.)
//
// The front end speculatively decodes wrong-path bytes, so the decoder must
// yield a clean bundle for *any* 32-bit input: an operand form no legal
// instruction can name is raised as OPC_ILLEGAL (faults to software if it
// commits, flushed before it does if it does not).
module penumbra3_decode
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic [31:0]  i_ir,
    output ctrl_bundle_t o_bundle
);

    // ── Field extraction ─────────────────────────────────────────
    // Always-valid wire taps; the per-format decoders pick which are
    // meaningful. IR[15:12] is the aliased field (Rdh for divmul, SPR# for
    // RDSPR/WRSPR, sysreg device for RDSYS/WRSYS) -- tapped once, interpreted
    // only under the matching op.
    logic [1:0]  fmt;
    logic [4:0]  r_op;
    logic [3:0]  r_rd, r_rs, field_1512, field_118;
    logic        r_f;
    logic [3:0]  l_op, l_rd;
    logic        m_load;
    logic [1:0]  m_size;
    logic        m_se;
    logic [3:0]  m_rd, m_rb;
    logic [3:0]  b_cond;

    assign fmt        = i_ir[31:30];
    assign r_op       = i_ir[29:25];
    assign r_rd       = i_ir[24:21];
    assign r_rs       = i_ir[20:17];
    assign r_f        = i_ir[16];
    assign field_1512 = i_ir[15:12];
    assign field_118  = i_ir[11:8];
    assign l_op       = i_ir[29:26];
    assign l_rd       = i_ir[25:22];
    assign m_load     = i_ir[29];
    assign m_size     = i_ir[28:27];
    assign m_se       = i_ir[26];
    assign m_rd       = i_ir[25:22];
    assign m_rb       = i_ir[21:18];
    assign b_cond     = i_ir[29:26];

    // ── Immediate extension (decoded per format, selected with the bundle) ─
    // The immediate's source bits and extension rule are format-and-opcode
    // specific. M and B are uniform per format; Format L varies per opcode and
    // holds the subtle cases.
    logic [31:0] imm;
    always_comb begin
        unique case (fmt)
            // Format R has no immediate.
            FMT_R: imm = 32'd0;
            // Format M: 16-bit signed byte offset, EA = Rb + offset.
            FMT_M: imm = {{16{i_ir[17]}}, i_ir[17:2]};
            // Format B: 22-bit signed word offset, target = PC + (off<<2).
            FMT_B: imm = {{8{i_ir[25]}}, i_ir[25:4], 2'b00};
            // Format L: per-opcode immediate extension.
            FMT_L: begin
                case (l_op)
                    OP_L_LLI:  imm = {16'b0, i_ir[15:0]};
                    OP_L_LLIS: imm = {{16{i_ir[15]}}, i_ir[15:0]};
                    OP_L_LUI:  imm = {i_ir[15:0], 16'b0};
                    OP_L_SHRI,
                    OP_L_SHLI,
                    OP_L_SARI: imm = {27'b0, i_ir[4:0]};
                    OP_L_JMP,
                    OP_L_JALR: imm = 32'b0;
                    default:   imm = {16'b0, i_ir[15:0]};
                endcase
            end
        endcase
    end

    // ── Inert default bundle ─────────────────────────────────────
    // Describes an illegal/no-op instruction; each format decoder starts here
    // and overrides only what it needs. Computed once and shared by all four.
    ctrl_bundle_t inert;
    always_comb begin
        inert          = '0;
        inert.op_class = OPC_ILLEGAL;
        inert.alu_op   = ALU_PASS;
        inert.mem_op   = MEM_NONE;
        inert.mem_size = MEM_SZ_WORD;
        inert.cond     = b_cond;
        inert.sys_dev  = field_1512;
        inert.sys_reg  = field_118;
        inert.spr_sel  = field_1512;
    end

    // ── Per-format decoders (all four evaluate in parallel) ──────
    ctrl_bundle_t bun_r, bun_l, bun_m, bun_b;

    // ══ Format R ════════════════════════════════════════════════
    always_comb begin
        bun_r = inert;
        if (r_op[4] == 1'b0) begin
            // Single-cycle ALU/move region (00000-01011). The gen3 ALU codes
            // equal op[3:0] here, so alu_op is a slice, not a remap; 01100-
            // 01111 fall through to illegal.
            if (r_op <= OP_R_SBC) begin
                bun_r.op_class      = OPC_ALU;
                bun_r.alu_op        = alu_op_e'(r_op[3:0]);
                bun_r.dst_sel       = r_rd; bun_r.dst_we   = 1'b1;
                bun_r.src_a_sel     = r_rd; bun_r.src_a_en = 1'b1;   // Rd = dest and src A
                bun_r.src_b_sel     = r_rs; bun_r.src_b_en = 1'b1;   // Rs = src B
                bun_r.flags_updater = 1'b1;
                // MOV/NOT take only Rs (operand B). MOV writes no flags.
                if (r_op == OP_R_MOV) begin
                    bun_r.src_a_en      = 1'b0;
                    bun_r.flags_updater = 1'b0;
                end else if (r_op == OP_R_NOT) begin
                    bun_r.src_a_en = 1'b0;
                end
                // ADC/SBC read the carry flag via the flag forward, not a
                // scoreboard source.
                if (r_op == OP_R_ADC || r_op == OP_R_SBC)
                    bun_r.flags_reader = 1'b1;
                // F bit: CMP (=SUB.F) / TEST (=AND.F) keep the flag write but
                // drop the GPR write -- and so advertise no destination to the
                // forward network.
                if (r_f) bun_r.dst_we = 1'b0;
            end
        end else begin
            // Multi-cycle / system region (op[4]=1).
            unique case (r_op)
                OP_R_MUL, OP_R_MULU, OP_R_DIV, OP_R_DIVU: begin
                    bun_r.op_class      = OPC_DIVMUL;
                    bun_r.src_a_sel     = r_rd; bun_r.src_a_en   = 1'b1;   // dividend / multiplicand
                    bun_r.src_b_sel     = r_rs; bun_r.src_b_en   = 1'b1;   // divisor / multiplier
                    bun_r.dst_sel       = r_rd; bun_r.dst_we     = 1'b1;   // low half / quotient
                    bun_r.dst_aux_sel   = field_1512; bun_r.dst_aux_we = 1'b1;  // high half / remainder
                    bun_r.flags_updater = 1'b1;   // sets N,Z; forces C=V=0 in EX
                    bun_r.alu_op        = ALU_PASS;
                    bun_r.divmul_op     = r_op[1:0];
                end
                OP_R_WRSYS: begin
                    bun_r.op_class     = OPC_WRSYS;
                    bun_r.src_b_sel    = r_rd; bun_r.src_b_en = 1'b1;   // value to write
                    bun_r.drain_commit = 1'b1;
                    bun_r.commit_wait  = 1'b1;   // wait one cycle for the device latch
                    bun_r.priv_op      = 1'b1;
                end
                OP_R_RDSYS: begin
                    bun_r.op_class = OPC_RDSYS;
                    bun_r.dst_sel  = r_rd; bun_r.dst_we = 1'b1;
                    bun_r.priv_op  = 1'b1;
                end
                OP_R_SYSCALL: begin
                    bun_r.op_class = OPC_SYSCALL;
                    bun_r.is_trap  = 1'b1;   // not privileged -- user may trap
                end
                OP_R_BREAK: begin
                    bun_r.op_class = OPC_BREAK;
                    bun_r.is_trap  = 1'b1;
                end
                OP_R_ERET: begin
                    bun_r.op_class      = OPC_ERET;
                    bun_r.drain_commit  = 1'b1;
                    bun_r.flags_updater = 1'b1;   // restores full SR incl. NZCV
                    bun_r.priv_op       = 1'b1;
                end
                OP_R_EI: begin
                    bun_r.op_class     = OPC_EI;
                    bun_r.drain_commit = 1'b1;   // I-bit ordering via the drain
                    bun_r.priv_op      = 1'b1;
                end
                OP_R_DI: begin
                    bun_r.op_class     = OPC_DI;
                    bun_r.drain_commit = 1'b1;
                    bun_r.priv_op      = 1'b1;
                end
                OP_R_WRSPR: begin
                    // SR is reserved for WRSPR (stays OPC_ILLEGAL): the S/I bits
                    // change only via exception entry and ERET, NZCV only via
                    // flag-writing ALU ops. The value SPRs (EPC/ESR/USP/SCRn)
                    // are plain stores -- ALU_PASS of the Rd value into the SPR
                    // destination, committed at WB.
                    if (field_1512 != SPR_SR) begin
                        bun_r.op_class   = OPC_WRSPR;
                        bun_r.src_b_sel  = r_rd; bun_r.src_b_en = 1'b1;   // value to write
                        bun_r.spr_sel    = field_1512;
                        bun_r.priv_op    = 1'b1;
                        bun_r.dst_sel    = field_1512; bun_r.dst_is_spr = 1'b1; bun_r.dst_we = 1'b1;
                    end
                end
                OP_R_RDSPR: begin
                    // RDSPR is readable for every SPR including SR (only WRSPR
                    // SR is reserved). SR has no scoreboard entry, so its value
                    // is composed in EX from committed S/I + flag-bypassed NZCV
                    // (flags_reader marks it). The value SPRs read their backend
                    // as operand B so ALU_PASS carries the value to the result.
                    bun_r.op_class = OPC_RDSPR;
                    bun_r.dst_sel  = r_rd; bun_r.dst_we = 1'b1;   // GPR destination
                    bun_r.spr_sel  = field_1512;
                    bun_r.priv_op  = 1'b1;
                    if (field_1512 == SPR_SR) begin
                        bun_r.flags_reader = 1'b1;
                    end else begin
                        bun_r.src_b_sel = field_1512; bun_r.src_b_is_spr = 1'b1; bun_r.src_b_en = 1'b1;
                    end
                end
                default: ;   // reserved -> OPC_ILLEGAL (inert)
            endcase
        end
    end

    // ══ Format L ════════════════════════════════════════════════
    always_comb begin
        bun_l = inert;
        unique case (l_op)
            OP_L_LLI, OP_L_LLIS: begin
                // Rd = ext(imm) -- pass the immediate through the ALU.
                bun_l.op_class   = OPC_ALU;
                bun_l.alu_op     = ALU_PASS;
                bun_l.b_from_imm = 1'b1;
                bun_l.dst_sel    = l_rd; bun_l.dst_we = 1'b1;
            end
            OP_L_LUI: begin
                // Rd = Rd | (imm<<16) -- reads Rd, no flag write.
                bun_l.op_class   = OPC_ALU;
                bun_l.alu_op     = ALU_OR;
                bun_l.b_from_imm = 1'b1;
                bun_l.src_a_sel  = l_rd; bun_l.src_a_en = 1'b1;
                bun_l.dst_sel    = l_rd; bun_l.dst_we   = 1'b1;
            end
            OP_L_ADDI, OP_L_SUBI, OP_L_ANDI,
            OP_L_SHLI, OP_L_SHRI, OP_L_SARI: begin
                bun_l.op_class      = OPC_ALU;
                bun_l.b_from_imm    = 1'b1;
                bun_l.src_a_sel     = l_rd; bun_l.src_a_en = 1'b1;
                bun_l.dst_sel       = l_rd; bun_l.dst_we   = 1'b1;
                bun_l.flags_updater = 1'b1;
                unique case (l_op)
                    OP_L_ADDI: bun_l.alu_op = ALU_ADD;
                    OP_L_SUBI: bun_l.alu_op = ALU_SUB;
                    OP_L_ANDI: bun_l.alu_op = ALU_AND;
                    OP_L_SHLI: bun_l.alu_op = ALU_SHL;
                    OP_L_SHRI: bun_l.alu_op = ALU_SHR;
                    OP_L_SARI: bun_l.alu_op = ALU_SAR;
                    default:   bun_l.alu_op = ALU_PASS;
                endcase
            end
            OP_L_CMPI: begin
                // flags = Rd - imm, no write (flags only).
                bun_l.op_class      = OPC_ALU;
                bun_l.alu_op        = ALU_SUB;
                bun_l.b_from_imm    = 1'b1;
                bun_l.src_a_sel     = l_rd; bun_l.src_a_en = 1'b1;
                bun_l.flags_updater = 1'b1;
            end
            OP_L_TESTI: begin
                // flags = Rd & imm, no write (flags only).
                bun_l.op_class      = OPC_ALU;
                bun_l.alu_op        = ALU_AND;
                bun_l.b_from_imm    = 1'b1;
                bun_l.src_a_sel     = l_rd; bun_l.src_a_en = 1'b1;
                bun_l.flags_updater = 1'b1;
            end
            OP_L_JMP: begin
                // PC = Rd. The redirect resolves in EX.
                bun_l.op_class  = OPC_JMP;
                bun_l.src_a_sel = l_rd; bun_l.src_a_en = 1'b1;
            end
            OP_L_JALR: begin
                // R13 = PC+4; PC = Rd. The link value is sourced in EX.
                bun_l.op_class  = OPC_JMP;
                bun_l.src_a_sel = l_rd;   bun_l.src_a_en = 1'b1;
                bun_l.dst_sel   = REG_LR; bun_l.dst_we   = 1'b1;
            end
            default: ;   // reserved -> OPC_ILLEGAL (inert)
        endcase
    end

    // ══ Format M ════════════════════════════════════════════════
    always_comb begin
        bun_m            = inert;
        bun_m.src_a_sel  = m_rb; bun_m.src_a_en = 1'b1;   // base -> ALU operand A
        bun_m.alu_op     = ALU_ADD;                        // EA = base + offset
        bun_m.b_from_imm = 1'b1;                            // ALU operand B = signed offset
        bun_m.mem_size   = m_size;
        bun_m.sign_ext   = m_se;
        if (m_load) begin
            bun_m.op_class = OPC_LOAD;
            bun_m.mem_op   = MEM_LOAD;
            bun_m.dst_sel  = m_rd; bun_m.dst_we = 1'b1;
        end else begin
            bun_m.op_class  = OPC_STORE;
            bun_m.mem_op    = MEM_STORE;
            bun_m.src_b_sel = m_rd; bun_m.src_b_en = 1'b1;   // store data (regfile port B)
        end
    end

    // ══ Format B ════════════════════════════════════════════════
    always_comb begin
        bun_b            = inert;
        bun_b.op_class   = OPC_BRANCH;
        bun_b.cond       = b_cond;
        bun_b.a_from_pc  = 1'b1;       // target = PC + offset
        bun_b.b_from_imm = 1'b1;
        bun_b.alu_op     = ALU_ADD;
        // Conditional branches read NZCV; unconditional B and BL do not.
        if (b_cond != COND_AL && b_cond != COND_BL)
            bun_b.flags_reader = 1'b1;
        // BL links PC+4 -> R13.
        if (b_cond == COND_BL) begin
            bun_b.dst_sel = REG_LR; bun_b.dst_we = 1'b1;
        end
    end

    // ── Tail select: the only serial step ───────────────────────
    ctrl_bundle_t sel;
    always_comb begin
        unique case (fmt)
            FMT_R: sel = bun_r;
            FMT_L: sel = bun_l;
            FMT_M: sel = bun_m;
            FMT_B: sel = bun_b;
        endcase
        sel.imm = imm;   // the format-selected immediate
    end

    // ── Mode-independent post-processing ────────────────────────
    always_comb begin
        o_bundle = sel;

        // R15/PC as a *source* resolves to the live PC (ID substitutes it),
        // not a regfile/scoreboard slot: flag it and drop the GPR enable so it
        // creates no dependency.
        o_bundle.src_a_is_pc = sel.src_a_en & ~sel.src_a_is_spr & (sel.src_a_sel == REG_PC);
        o_bundle.src_b_is_pc = sel.src_b_en & ~sel.src_b_is_spr & (sel.src_b_sel == REG_PC);
        if (o_bundle.src_a_is_pc) o_bundle.src_a_en = 1'b0;
        if (o_bundle.src_b_is_pc) o_bundle.src_b_en = 1'b0;

        // Operand-validity: a form no legal instruction can name is raised as
        // illegal, not silently sanitized. Cases: R15/PC as a written
        // destination (primary or aux) -- PC is not register-file writable;
        // an out-of-range SPR number on a scoreboard reference (defined SPRs
        // are ESR/EPC/USP/SCR0-3; SR is handled by the opcode decode).
        if ((o_bundle.dst_we     & ~o_bundle.dst_is_spr & (o_bundle.dst_sel     == REG_PC))
         || (o_bundle.dst_aux_we &                        (o_bundle.dst_aux_sel == REG_PC))
         || (o_bundle.src_a_en   & o_bundle.src_a_is_spr & (o_bundle.src_a_sel   > SPR_SCR3))
         || (o_bundle.src_b_en   & o_bundle.src_b_is_spr & (o_bundle.src_b_sel   > SPR_SCR3))
         || (o_bundle.dst_we     & o_bundle.dst_is_spr   & (o_bundle.dst_sel     > SPR_SCR3))) begin
            o_bundle.op_class      = OPC_ILLEGAL;
            o_bundle.src_a_en      = 1'b0;  o_bundle.src_a_is_pc = 1'b0;
            o_bundle.src_b_en      = 1'b0;  o_bundle.src_b_is_pc = 1'b0;
            o_bundle.dst_we        = 1'b0;
            o_bundle.dst_aux_we    = 1'b0;
            o_bundle.flags_updater = 1'b0;
            o_bundle.is_trap       = 1'b0;
            o_bundle.drain_commit  = 1'b0;
            o_bundle.priv_op       = 1'b0;
        end

        o_bundle.illegal = (o_bundle.op_class == OPC_ILLEGAL);
    end

    // ══════════════════════════════════════════════════════════
    // Assertions -- sim-only (Verilator --assert); stripped at synth.
    // Contract invariants a correct decode cannot violate.
    // ══════════════════════════════════════════════════════════
    always_comb begin
        // An illegal op carries no privilege check (sanitize clears priv_op).
        assert (!(o_bundle.illegal && o_bundle.priv_op))
            else $error("penumbra3_decode: illegal and priv_op both set");
        // Only a dual-destination opcode carries an aux destination, and the
        // aux (a GPR hi-half) implies the primary destination write.
        assert (!(o_bundle.dst_aux_we && o_bundle.op_class != OPC_DIVMUL))
            else $error("penumbra3_decode: aux dst outside a dual-destination opcode");
        assert (!(o_bundle.dst_aux_we && !o_bundle.dst_we))
            else $error("penumbra3_decode: aux dst without a primary dst write");
        // is_trap belongs only to SYSCALL/BREAK.
        assert (!(o_bundle.is_trap && o_bundle.op_class != OPC_SYSCALL && o_bundle.op_class != OPC_BREAK))
            else $error("penumbra3_decode: is_trap outside SYSCALL/BREAK");
        // A live GPR reference is never R15/PC -- those resolve to the PC.
        assert (!(o_bundle.src_a_en && !o_bundle.src_a_is_spr && o_bundle.src_a_sel == REG_PC))
            else $error("penumbra3_decode: R15/PC as a live GPR source A");
        assert (!(o_bundle.src_b_en && !o_bundle.src_b_is_spr && o_bundle.src_b_sel == REG_PC))
            else $error("penumbra3_decode: R15/PC as a live GPR source B");
        assert (!(o_bundle.dst_we && !o_bundle.dst_is_spr && o_bundle.dst_sel == REG_PC))
            else $error("penumbra3_decode: R15/PC as a GPR destination");
    end

endmodule
