// penumbra2_decode — Penumbra/2 instruction decoder (ID stage).
//
// Combinational. Turns a 32-bit instruction word into the ID control
// bundle: the architectural register references that feed
// penumbra2_regmap, the operand-mux selects, the ALU function, the
// extended immediate, the memory/sysreg/SPR controls, the writeback
// enables, and the decode-time exception flags. Specified by
// doc/internals/penumbra2/control-decode.md; the instruction encoding
// it decodes is fixed by doc/system/instruction-encoding.md.
//
// Scope: field extraction, classification, and control derivation only.
// It produces *architectural* register selectors (Rd/Rs/Rb numbers +
// is_spr + enables); the architectural->physical scoreboard mapping
// (R14 banking, SPR-USP cross-bank) is a separate step in
// penumbra2_regmap, which the ID stage drives from these outputs.
//
// Operand routing convention: a_from_pc / b_from_imm pick what the EX
// ALU sees on each input, independently of the source enables. A store
// reads both Rb (base) and Rd (data) from the regfile (src_a, src_b),
// yet the ALU's B input is the immediate offset (b_from_imm), so EA =
// base + offset while the Rd read flows to the store-data path.

module penumbra2_decode
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic [31:0]          i_ir,
    input  logic                 i_supervisor,   // SR.S — for the privilege check

    // ── Master classification ───────────────────────────────────
    output logic [OPC_W-1:0]     o_op_class,

    // ── Register references (feed penumbra2_regmap) ──────────────
    // Architectural selectors with is_spr / enable. A source the
    // instruction does not read keeps its enable low (so it can never
    // cause a scoreboard stall); an instruction with no register
    // destination keeps o_dst_en low.
    output logic [3:0]           o_src_a_sel,
    output logic                 o_src_a_is_spr,
    output logic                 o_src_a_en,
    output logic [3:0]           o_src_b_sel,
    output logic                 o_src_b_is_spr,
    output logic                 o_src_b_en,
    output logic [3:0]           o_dst_sel,
    output logic                 o_dst_is_spr,
    output logic                 o_dst_en,
    output logic [3:0]           o_dst_aux_sel,    // second (aux) write-only GPR destination
    output logic                 o_dst_aux_en,

    // ── EX datapath controls ─────────────────────────────────────
    output logic [ALU_OP_W-1:0]  o_alu_op,
    output logic [1:0]           o_divmul_op,     // divmul variant: = ISA op[1:0] (bit1 div/mul, bit0 unsigned)
    output logic                 o_a_from_pc,     // ALU operand A: 1=PC, 0=regfile src A
    output logic                 o_b_from_imm,    // ALU operand B: 1=immediate, 0=regfile src B
    output logic [31:0]          o_imm,
    output logic [3:0]           o_cond,          // branch condition (Format B)

    // ── Flag (NZCV) interaction ──────────────────────────────────
    output logic                 o_writes_flags,
    output logic                 o_reads_flags,
    output logic                 o_flag_only,     // F bit: keep flag write, drop GPR write

    // ── MEM controls ─────────────────────────────────────────────
    output logic [MEM_OP_W-1:0]  o_mem_op,
    output logic [1:0]           o_mem_size,      // MEM_SZ_BYTE / MEM_SZ_HALF / MEM_SZ_WORD
    output logic                 o_sign_ext,      // sub-word load sign-extend

    // ── Sysreg / SPR selects ─────────────────────────────────────
    output logic [3:0]           o_sys_dev,
    output logic [3:0]           o_sys_reg,
    output logic [3:0]           o_spr_sel,

    // ── Commit ordering ──────────────────────────────────────────
    output logic                 o_drain_commit,
    output logic                 o_post_commit_wait,

    // ── Writeback enables ────────────────────────────────────────
    output logic                 o_gpr_we,
    output logic                 o_spr_we,
    output logic                 o_flag_we,

    // ── Decode-time exceptions ───────────────────────────────────
    output logic                 o_is_trap,       // SYSCALL/BREAK
    output logic                 o_illegal,
    output logic                 o_priv_fault,
    output logic [3:0]           o_fault_vec
);

    // ── Field extraction ────────────────────────────────────────
    // Always-valid wire taps; the per-format arms below pick which
    // ones are meaningful. IR[15:12] is the aliased field — Rdh for
    // divmul, SPR# for RDSPR/WRSPR, sysreg device for RDSYS/WRSYS —
    // so it is read once here and interpreted only under the matching
    // op_class.
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

    // ── Immediate extension ──────────────────────────────────────
    // The immediate's source bits and extension rule are
    // format-and-opcode specific (control-decode.md). M and B are
    // uniform per format; Format L varies per opcode and holds the
    // subtle cases.
    always_comb begin
        unique case (fmt)
            // Format M: 16-bit signed byte offset, EA = Rb + offset.
            FMT_M:   o_imm = {{16{i_ir[17]}}, i_ir[17:2]};
            // Format B: 22-bit signed word offset, target = PC + (off<<2).
            FMT_B:   o_imm = {{8{i_ir[25]}}, i_ir[25:4], 2'b00};
            FMT_L: begin
                case (l_op)
                    OP_L_LLIS: o_imm = {{16{i_ir[15]}}, i_ir[15:0]};
                    OP_L_LUI:  o_imm = {i_ir[15:0], 16'b0};
                    OP_L_SHLI,
                    OP_L_SHRI,
                    OP_L_SARI: o_imm = {27'b0, i_ir[4:0]};
                    OP_L_JMP,
                    OP_L_JALR: o_imm = 32'b0;
                    default:   o_imm = {16'b0, i_ir[15:0]};
                endcase
            end
            // Format R has no immediate.
            default: o_imm = 32'd0;
        endcase
    end

    // ── Main decode ──────────────────────────────────────────────
    // Defaults describe an inert instruction; each arm overrides only
    // what it needs. o_illegal and o_flag_we are derived afterward, so
    // they are not driven here.
    always_comb begin
        o_op_class         = OPC_ILLEGAL;
        o_src_a_sel        = 4'd0; o_src_a_is_spr = 1'b0; o_src_a_en = 1'b0;
        o_src_b_sel        = 4'd0; o_src_b_is_spr = 1'b0; o_src_b_en = 1'b0;
        o_dst_sel          = 4'd0; o_dst_is_spr   = 1'b0; o_dst_en   = 1'b0;
        o_dst_aux_sel      = 4'd0; o_dst_aux_en   = 1'b0;
        o_alu_op           = ALU_PASS;
        o_divmul_op        = 2'b00;
        o_a_from_pc        = 1'b0;
        o_b_from_imm       = 1'b0;
        o_cond             = b_cond;
        o_writes_flags     = 1'b0;
        o_reads_flags      = 1'b0;
        o_flag_only        = 1'b0;
        o_mem_op           = MEM_NONE;
        o_mem_size         = MEM_SZ_WORD;
        o_sign_ext         = 1'b0;
        o_sys_dev          = field_1512;
        o_sys_reg          = field_118;
        o_spr_sel          = field_1512;
        o_drain_commit     = 1'b0;
        o_post_commit_wait = 1'b0;
        o_gpr_we           = 1'b0;
        o_spr_we           = 1'b0;
        o_is_trap          = 1'b0;
        o_priv_fault       = 1'b0;

        unique case (fmt)
        // ══ Format R ════════════════════════════════════════════
        FMT_R: begin
            if (r_op[4] == 1'b0) begin
                // Single-cycle ALU/move region (00000-01011). The gen2
                // ALU_* codes equal op[3:0] here, so alu_op is a slice,
                // not a remap. 01100-01111 fall through to illegal.
                if (r_op <= OP_R_SBC) begin
                    o_op_class  = OPC_ALU;
                    o_alu_op    = r_op[3:0];
                    o_dst_sel   = r_rd; o_dst_en   = 1'b1;
                    o_src_a_sel = r_rd; o_src_a_en = 1'b1;   // Rd = dest and src A
                    o_src_b_sel = r_rs; o_src_b_en = 1'b1;   // Rs = src B
                    o_gpr_we    = 1'b1;
                    o_writes_flags = 1'b1;
                    // MOV/NOT take only Rs (operand B); they do not read
                    // Rd. MOV additionally writes no flags.
                    if (r_op == OP_R_MOV) begin
                        o_src_a_en     = 1'b0;
                        o_writes_flags = 1'b0;
                    end else if (r_op == OP_R_NOT) begin
                        o_src_a_en = 1'b0;
                    end
                    // ADC/SBC additionally read the carry flag; it
                    // reaches EX via the flag bypass, not a scoreboard
                    // source.
                    if (r_op == OP_R_ADC || r_op == OP_R_SBC)
                        o_reads_flags = 1'b1;
                    // F bit: CMP (=SUB·F) / TEST (=AND·F) keep the flag
                    // write but drop the GPR write.
                    o_flag_only = r_f;
                    if (r_f) o_gpr_we = 1'b0;
                end
            end else begin
                // Multi-cycle / system region (op[4]=1).
                unique case (r_op)
                    OP_R_MUL, OP_R_MULU, OP_R_DIV, OP_R_DIVU: begin
                        o_op_class   = OPC_DIVMUL;
                        o_src_a_sel  = r_rd; o_src_a_en = 1'b1;   // dividend / multiplicand
                        o_src_b_sel  = r_rs; o_src_b_en = 1'b1;   // divisor / multiplier
                        o_dst_sel    = r_rd; o_dst_en   = 1'b1;   // low half / quotient
                        o_dst_aux_sel = field_1512; o_dst_aux_en = 1'b1;  // high half / remainder
                        o_gpr_we     = 1'b1;
                        o_writes_flags = 1'b1;    // sets N,Z; forces C=V=0 in EX
                        o_alu_op     = ALU_PASS;  // the divmul peer unit owns the result
                        o_divmul_op  = r_op[1:0]; // MUL/MULU/DIV/DIVU select
                    end
                    OP_R_WRSYS: begin
                        o_op_class         = OPC_WRSYS;
                        o_src_b_sel        = r_rs; o_src_b_en = 1'b1;   // value to write
                        o_drain_commit     = 1'b1;
                        o_post_commit_wait = 1'b1;   // wait one cycle for the device latch
                        o_priv_fault       = ~i_supervisor;
                    end
                    OP_R_RDSYS: begin
                        o_op_class   = OPC_RDSYS;
                        o_dst_sel    = r_rd; o_dst_en = 1'b1;
                        o_gpr_we     = 1'b1;
                        o_priv_fault = ~i_supervisor;
                    end
                    OP_R_SYSCALL: begin
                        o_op_class = OPC_SYSCALL;
                        o_is_trap  = 1'b1;           // not privileged — user may trap
                    end
                    OP_R_BREAK: begin
                        o_op_class = OPC_BREAK;
                        o_is_trap  = 1'b1;
                    end
                    OP_R_ERET: begin
                        o_op_class     = OPC_ERET;
                        o_drain_commit = 1'b1;
                        o_writes_flags = 1'b1;       // restores full SR incl. NZCV
                        o_priv_fault   = ~i_supervisor;
                    end
                    OP_R_EI: begin
                        o_op_class     = OPC_EI;
                        o_drain_commit = 1'b1;       // I-bit ordering via the drain
                        o_priv_fault   = ~i_supervisor;
                    end
                    OP_R_DI: begin
                        o_op_class     = OPC_DI;
                        o_drain_commit = 1'b1;
                        o_priv_fault   = ~i_supervisor;
                    end
                    OP_R_WRSPR: begin
                        o_op_class   = OPC_WRSPR;
                        o_src_b_sel  = r_rs; o_src_b_en = 1'b1;   // value to write
                        o_spr_sel    = field_1512;
                        o_priv_fault = ~i_supervisor;
                        if (field_1512 == SPR_SR) begin
                            // SR is not a scoreboard/SPR-file entry: its
                            // NZCV bits feed the flag bypass and its S/I
                            // bits commit via the drain. No scoreboard
                            // destination, no SPR-file write.
                            o_drain_commit = 1'b1;
                            o_writes_flags = 1'b1;
                        end else begin
                            o_dst_sel = field_1512; o_dst_is_spr = 1'b1; o_dst_en = 1'b1;
                            o_spr_we  = 1'b1;
                        end
                    end
                    OP_R_RDSPR: begin
                        o_op_class   = OPC_RDSPR;
                        o_dst_sel    = r_rd; o_dst_en = 1'b1;
                        o_gpr_we     = 1'b1;
                        o_spr_sel    = field_1512;
                        o_priv_fault = ~i_supervisor;
                        if (field_1512 == SPR_SR) begin
                            // SR's NZCV bits come from the flag bypass
                            // (reads_flags); its S/I bits from committed
                            // SR. Not a scoreboard source.
                            o_reads_flags = 1'b1;
                        end else begin
                            o_src_a_sel = field_1512; o_src_a_is_spr = 1'b1; o_src_a_en = 1'b1;
                        end
                    end
                    default: ;   // 10100-10110 reserved → OPC_ILLEGAL
                endcase
            end
        end
        // ══ Format L ════════════════════════════════════════════
        FMT_L: begin
            unique case (l_op)
                OP_L_LLI, OP_L_LLIS: begin
                    // Rd = ext(imm) — pass the immediate through the ALU.
                    o_op_class   = OPC_ALU;
                    o_alu_op     = ALU_PASS;
                    o_b_from_imm = 1'b1;
                    o_dst_sel    = l_rd; o_dst_en = 1'b1;
                    o_gpr_we     = 1'b1;
                end
                OP_L_LUI: begin
                    // Rd = Rd | (imm<<16) — reads Rd, no flag write.
                    o_op_class   = OPC_ALU;
                    o_alu_op     = ALU_OR;
                    o_b_from_imm = 1'b1;
                    o_src_a_sel  = l_rd; o_src_a_en = 1'b1;
                    o_dst_sel    = l_rd; o_dst_en   = 1'b1;
                    o_gpr_we     = 1'b1;
                end
                OP_L_ADDI, OP_L_SUBI, OP_L_ANDI,
                OP_L_SHLI, OP_L_SHRI, OP_L_SARI: begin
                    o_op_class     = OPC_ALU;
                    o_b_from_imm   = 1'b1;
                    o_src_a_sel    = l_rd; o_src_a_en = 1'b1;
                    o_dst_sel      = l_rd; o_dst_en   = 1'b1;
                    o_gpr_we       = 1'b1;
                    o_writes_flags = 1'b1;
                    unique case (l_op)
                        OP_L_ADDI: o_alu_op = ALU_ADD;
                        OP_L_SUBI: o_alu_op = ALU_SUB;
                        OP_L_ANDI: o_alu_op = ALU_AND;
                        OP_L_SHLI: o_alu_op = ALU_SHL;
                        OP_L_SHRI: o_alu_op = ALU_SHR;
                        OP_L_SARI: o_alu_op = ALU_SAR;
                        default:   o_alu_op = ALU_PASS;
                    endcase
                end
                OP_L_CMPI: begin
                    // flags = Rd - imm, no write.
                    o_op_class     = OPC_ALU;
                    o_alu_op       = ALU_SUB;
                    o_b_from_imm   = 1'b1;
                    o_src_a_sel    = l_rd; o_src_a_en = 1'b1;
                    o_writes_flags = 1'b1;
                    o_flag_only    = 1'b1;
                end
                OP_L_TESTI: begin
                    // flags = Rd & imm, no write.
                    o_op_class     = OPC_ALU;
                    o_alu_op       = ALU_AND;
                    o_b_from_imm   = 1'b1;
                    o_src_a_sel    = l_rd; o_src_a_en = 1'b1;
                    o_writes_flags = 1'b1;
                    o_flag_only    = 1'b1;
                end
                OP_L_JMP: begin
                    // PC = Rd. The target register is read; the redirect
                    // is resolved in EX.
                    o_op_class  = OPC_JMP;
                    o_src_a_sel = l_rd; o_src_a_en = 1'b1;
                end
                OP_L_JALR: begin
                    // R13 = PC+4; PC = Rd. The link value (next_pc) is
                    // sourced in EX, not from the ALU operands.
                    o_op_class  = OPC_JMP;
                    o_src_a_sel = l_rd; o_src_a_en = 1'b1;
                    o_dst_sel   = REG_LR; o_dst_en = 1'b1;
                    o_gpr_we    = 1'b1;
                end
                default: ;   // 1101-1111 reserved → OPC_ILLEGAL
            endcase
        end
        // ══ Format M ════════════════════════════════════════════
        FMT_M: begin
            o_src_a_sel  = m_rb; o_src_a_en = 1'b1;   // base → ALU operand A
            o_alu_op     = ALU_ADD;                    // EA = base + offset
            o_b_from_imm = 1'b1;                       // ALU operand B = signed offset
            o_mem_size   = m_size;
            o_sign_ext   = m_se;
            if (m_load) begin
                o_op_class = OPC_LOAD;
                o_mem_op   = MEM_LOAD;
                o_dst_sel  = m_rd; o_dst_en = 1'b1;
                o_gpr_we   = 1'b1;
            end else begin
                o_op_class  = OPC_STORE;
                o_mem_op    = MEM_STORE;
                o_src_b_sel = m_rd; o_src_b_en = 1'b1;   // store data (regfile port B)
            end
        end
        // ══ Format B ════════════════════════════════════════════
        FMT_B: begin
            o_op_class   = OPC_BRANCH;
            o_cond       = b_cond;
            o_a_from_pc  = 1'b1;          // target = PC + offset (relative to this PC)
            o_b_from_imm = 1'b1;
            o_alu_op     = ALU_ADD;
            // Conditional branches read NZCV; unconditional B and BL
            // do not.
            if (b_cond != COND_AL && b_cond != COND_BL)
                o_reads_flags = 1'b1;
            // BL links PC+4 → R13.
            if (b_cond == COND_BL) begin
                o_dst_sel = REG_LR; o_dst_en = 1'b1;
                o_gpr_we  = 1'b1;
            end
        end
        endcase
    end

    // illegal = no legal opcode/operand form decoded. flag_we tracks
    // writes_flags exactly (control-decode.md). Fault vector priority
    // is illegal > priv > trap (illegal outranks an op that would also
    // be privileged).
    assign o_illegal  = (o_op_class == OPC_ILLEGAL);
    assign o_flag_we  = o_writes_flags;
    assign o_fault_vec =
          o_illegal                            ? VEC_ILLEGAL
        : o_priv_fault                         ? VEC_PRIV
        : (o_is_trap && o_op_class == OPC_SYSCALL) ? VEC_SYSCALL
        : o_is_trap                            ? VEC_BREAK
        :                                        4'd0;

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // Contract invariants a correct decode cannot violate.
    // ══════════════════════════════════════════════════════════
    always_comb begin
        // A GPR write needs a destination to write.
        assert (!(o_gpr_we && !o_dst_en))
            else $error("penumbra2_decode: gpr_we without a destination");
        // An SPR-file write likewise needs a destination.
        assert (!(o_spr_we && !o_dst_en))
            else $error("penumbra2_decode: spr_we without a destination");
        // GPR and SPR writes are mutually exclusive: MEM collapses the two
        // writeback data into one o_wb_value and WB routes it by exactly one
        // of these strobes (the other is quiet). An instruction asserting both
        // would write a GPR and strobe an SPR with the same datum.
        assert (!(o_gpr_we && o_spr_we))
            else $error("penumbra2_decode: gpr_we and spr_we both set");
        // illegal and priv_fault are constructed mutually exclusive.
        assert (!(o_illegal && o_priv_fault))
            else $error("penumbra2_decode: illegal and priv_fault both set");
        // Only a dual-destination opcode carries a second (aux) destination.
        assert (!(o_dst_aux_en && o_op_class != OPC_DIVMUL))
            else $error("penumbra2_decode: aux dst enabled outside a dual-destination opcode");
        // A dual write's aux (high half) is a GPR, and WB gates its second
        // write cycle on gpr_we — so an aux destination implies gpr_we, else
        // the high half is silently dropped while WB still holds MEM a cycle.
        assert (!(o_dst_aux_en && !o_gpr_we))
            else $error("penumbra2_decode: aux dst without gpr_we");
        // is_trap belongs only to SYSCALL/BREAK.
        assert (!(o_is_trap && o_op_class != OPC_SYSCALL && o_op_class != OPC_BREAK))
            else $error("penumbra2_decode: is_trap outside SYSCALL/BREAK");
        // A live GPR reference is never R15/PC — those resolve to the
        // PC value, not a regfile/scoreboard slot (regmap would map it
        // to entry 0). This is the decoder's own invariant; regmap
        // re-checks it as a consumer guard.
        assert (!(o_src_a_en && !o_src_a_is_spr && o_src_a_sel == REG_PC))
            else $error("penumbra2_decode: R15/PC as live GPR source A");
        assert (!(o_src_b_en && !o_src_b_is_spr && o_src_b_sel == REG_PC))
            else $error("penumbra2_decode: R15/PC as live GPR source B");
        assert (!(o_dst_en && !o_dst_is_spr && o_dst_sel == REG_PC))
            else $error("penumbra2_decode: R15/PC as live GPR destination");
    end

endmodule
