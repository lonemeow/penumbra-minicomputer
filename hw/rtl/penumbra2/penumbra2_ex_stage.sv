// penumbra2_ex_stage — Penumbra/2 execute stage.
//
// The integration point of the gen2 datapath spine, mirroring how
// penumbra2_id_stage assembles the front half. Specified by the EX
// sections of doc/internals/penumbra2/{pipeline-stages,control-decode,
// hazard-model}.md. Each cycle it:
//   - feeds the ID/EX operands to the combinational ALU (penumbra2_alu),
//   - forwards the youngest in-flight NZCV (penumbra2_flag_bypass) and
//     hands its carry bit to the ALU for ADC/SBC,
//   - resolves control transfers — cond_eval against the forwarded NZCV,
//     branch target from the ALU (PC + offset), JMP target from op_a —
//     and drives the front-end redirect,
//   - narrows the control bundle to the ctrl_mem + ctrl_wb subsets and
//     latches the EX/MEM register under the back-pressure handshake.
//
// Drain-commit instructions (ERET / WRSYS / WRSPR-SR / EI / DI) are
// sequenced here too: EX holds them, drains MEM/WB, and pulses o_dc_commit
// at the commit point (the architectural SR/PC/sysreg writes are routed by
// integration from the held ID/EX bundle — EX owns the *when*, not the
// *what*). The divmul start/busy handshake is the one piece still to come.
//
// Flag bypass boundary: the youngest producer ahead of the EX reader is
// the EX/MEM-stage instruction, which is exactly what this stage latched
// last cycle — so the bypass's MEM source feeds back from this stage's
// own registered o_flag_value/o_flag_we (the same self-feedback shape the
// ID scoreboard uses for its EX writer). The WB producer and the
// committed SR arrive as ports from downstream.

module penumbra2_ex_stage
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── ID/EX input: the issued instruction ──────────────────────
    input  logic [OPC_W-1:0]      i_op_class,
    input  logic [ALU_OP_W-1:0]   i_alu_op,
    input  logic [31:0]           i_op_a,
    input  logic [31:0]           i_op_b,
    input  logic [31:0]           i_store_data,
    input  logic [3:0]            i_cond,           // branch condition (Format B)
    input  logic [MEM_OP_W-1:0]   i_mem_op,
    input  logic [1:0]            i_mem_size,
    input  logic                  i_sign_ext,
    input  logic [3:0]            i_sys_dev,
    input  logic [3:0]            i_sys_reg,
    input  logic [3:0]            i_spr_sel,
    input  logic                  i_drain_commit,     // ERET/WRSYS/WRSPR-SR/EI/DI
    input  logic                  i_post_commit_wait, // WRSYS: hold 1 cyc for the device latch
    input  logic                  i_gpr_we,
    input  logic                  i_spr_we,
    input  logic                  i_flag_we,
    input  logic [SB_IDX_W-1:0]   i_phys_dst,
    input  logic [SB_IDX_W-1:0]   i_phys_dst_hi,
    input  logic                  i_phys_dst_hi_en,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,        // PC + 4: BL/JALR link value
    input  logic                  i_valid,          // 0 = bubble in
    input  logic                  i_fault_pending,
    input  logic [3:0]            i_fault_vec,

    // ── Flag bypass external sources (MEM producer is internal) ──
    input  logic [3:0]            i_sr_flags,        // committed SR NZCV
    input  logic [3:0]            i_wb_flags,        // MEM/WB in-flight producer
    input  logic                  i_wb_writes_flags,

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic                  i_stall_in,        // MEM cannot accept this cycle
    input  logic                  i_wb_active,       // WB holds a live (non-bubble) insn
    input  logic                  i_bubble,          // force this insn to a bubble (fault flush from WB)
    output logic                  o_stall,           // back-pressure to ID
    output logic                  o_dc_commit,       // drain-commit insn commits this cycle

    // ── Branch resolution (to IF: redirect + flush IF1/IF2/ID) ──
    output logic                  o_branch_taken,    // redirect the front-end this cycle
    output logic [31:0]           o_branch_target,   // PC to redirect to

    // ── EX/MEM register (to MEM) ─────────────────────────────────
    output logic [OPC_W-1:0]      o_op_class,
    output logic [MEM_OP_W-1:0]   o_mem_op,
    output logic [1:0]            o_mem_size,
    output logic                  o_sign_ext,
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    output logic [3:0]            o_spr_sel,
    output logic                  o_gpr_we,
    output logic                  o_spr_we,
    output logic                  o_flag_we,
    output logic [31:0]           o_result,
    output logic [31:0]           o_store_data,
    output logic [3:0]            o_flag_value,      // NZCV, packed as SR[3:0]
    output logic [SB_IDX_W-1:0]   o_phys_dst,
    output logic [SB_IDX_W-1:0]   o_phys_dst_hi,
    output logic                  o_phys_dst_hi_en,
    output logic [31:0]           o_pc,
    output logic                  o_valid,
    output logic                  o_fault_pending,
    output logic [3:0]            o_fault_vec
);

    // ── Flag bypass: youngest in-flight NZCV for the EX reader ───
    // The MEM producer is this stage's own registered output: the
    // EX/MEM slot is the instruction now in MEM. A bubble or faulting
    // slot is not a committed flag writer, so it is gated off here.
    // The carry bit feeds the ALU (ADC/SBC); all four bits feed the
    // branch-condition evaluation below.
    logic [3:0] fwd_flags;
    logic       mem_writes_flags;
    assign mem_writes_flags = o_flag_we & o_valid & ~o_fault_pending;

    penumbra2_flag_bypass u_flag_bypass (
        .i_sr_flags(i_sr_flags),
        .i_mem_flags(o_flag_value),
        .i_mem_writes_flags(mem_writes_flags),
        .i_wb_flags(i_wb_flags),
        .i_wb_writes_flags(i_wb_writes_flags),
        .o_flags(fwd_flags)
    );

    // ── ALU compute ──────────────────────────────────────────────
    // NZCV bundle packs as SR[3:0]: N=0 Z=1 C=2 V=3. ADC/SBC take the
    // forwarded carry (bundle bit 2); other ops ignore the carry-in.
    logic        carry_in;
    logic [31:0] alu_result;
    logic        alu_n, alu_z, alu_c, alu_v;
    logic [3:0]  alu_flags;

    assign carry_in = fwd_flags[2];

    penumbra2_alu u_alu (
        .i_a(i_op_a),
        .i_b(i_op_b),
        .i_op(i_alu_op),
        .i_carry_in(carry_in),
        .o_result(alu_result),
        .o_flag_z(alu_z),
        .o_flag_n(alu_n),
        .o_flag_c(alu_c),
        .o_flag_v(alu_v)
    );

    assign alu_flags = {alu_v, alu_c, alu_z, alu_n};

    // ── Branch resolution ────────────────────────────────────────
    // EX resolves every control transfer. The condition is evaluated
    // against the forwarded NZCV by gen1's cond_eval (reused unchanged —
    // the cond encoding is shared ISA). For Format B the ALU already
    // computed the target (PC + offset) as its result; JMP/JALR carry
    // the target register in op_a. BL and JALR additionally link PC+4
    // into R13 — that link value, not the ALU result, must reach WB.
    logic cond_taken;
    cond_eval u_cond_eval (
        .i_flag_n(fwd_flags[0]),
        .i_flag_z(fwd_flags[1]),
        .i_flag_c(fwd_flags[2]),
        .i_flag_v(fwd_flags[3]),
        .i_cond(i_cond),
        .o_taken(cond_taken)
    );

    // Link-value mux: a linking control transfer (BL / JALR — the only
    // branch/JMP forms with gpr_we) writes PC+4 to R13, so its EX/MEM
    // result is next_pc; everything else keeps the ALU result (which is
    // the load/store EA for memory ops and the value for ALU ops).
    logic        is_link;
    logic [31:0] result_value;
    assign is_link      = (i_op_class == OPC_BRANCH || i_op_class == OPC_JMP) & i_gpr_we;
    assign result_value = is_link ? i_next_pc : alu_result;

    // branch_redirect / branch_target are the resolution proper.
    logic        branch_redirect;
    logic [31:0] branch_target;

    always_comb begin
        case (i_op_class)
            OPC_JMP: begin
                branch_redirect = 1'b1;
                branch_target   = i_op_a;
            end
            OPC_BRANCH: begin
                branch_redirect = cond_taken;
                branch_target   = alu_result;
            end
            default: begin
                branch_redirect = 1'b0;
                branch_target   = 32'b0;
            end
        endcase
    end

    // A taken redirect counts only for a real, non-faulting, un-flushed
    // branch slot; a bubble or a slot the fault flush is killing must
    // not steer the front-end.
    assign o_branch_taken  = branch_redirect & i_valid & ~i_fault_pending & ~i_bubble;
    assign o_branch_target = branch_target;

    // ── Drain-commit FSM ─────────────────────────────────────────
    // ERET/WRSYS/WRSPR-SR/EI/DI must order their commit against older
    // in-flight instructions (which observe the pre-commit SR) and
    // younger ones (which observe the post-commit state). The insn holds
    // in EX — it never advances into MEM/WB — while MEM and WB drain;
    // once drained it commits (o_dc_commit pulse) and, for the WRSYS
    // variant, EX holds upstream one extra cycle (post_wait_q) so the
    // sysreg device latches before the next insn can observe it.
    //
    // MEM occupancy is EX's own EX/MEM register (o_valid) — MEM holds an
    // insn by back-pressuring, never by buffering past the register — so
    // only WB needs an occupancy feedback (i_wb_active).
    logic dc_here;          // a drain-commit insn occupies EX (held in ID/EX)
    logic drained;          // MEM and WB hold no live instruction
    logic dc_commit_now;    // commit fires this cycle
    logic post_wait_q;      // the WRSYS post-commit extra-hold cycle

    assign dc_here       = i_valid & i_drain_commit & ~i_fault_pending & ~i_bubble;
    assign drained       = ~o_valid & ~i_wb_active;
    assign dc_commit_now = dc_here & drained & ~post_wait_q;
    assign o_dc_commit   = dc_commit_now;

    always_ff @(posedge i_clk) begin
        if (i_rst) post_wait_q <= 1'b0;
        else       post_wait_q <= dc_commit_now & i_post_commit_wait;
    end

    // ── Issue / back-pressure control ────────────────────────────
    // EX accepts a new ID/EX instruction every cycle except when it is
    // back-pressured (MEM via i_stall_in) or busy sequencing a
    // drain-commit. i_bubble — the fault-flush from WB — forces the
    // in-flight slot and wins over everything. The divmul internal stall
    // layers onto o_stall later.
    logic advance, next_valid;

    always_comb begin
        if (i_bubble) begin
            next_valid = 1'b0;          // flush wins
            advance    = 1'b0;
            o_stall    = i_stall_in;
        end else if (post_wait_q) begin
            next_valid = 1'b0;          // WRSYS already committed; the insn leaves as a bubble
            advance    = 1'b0;
            o_stall    = 1'b0;          // release upstream after the one extra hold
        end else if (dc_here) begin
            // Drain-commit insn: never advances into MEM/WB. Inject a
            // bubble so MEM/WB drain — but only when MEM can accept;
            // while MEM is mid-access (i_stall_in) hold EX/MEM so its
            // live work is not clobbered.
            advance = 1'b0;
            if (i_stall_in) begin
                next_valid = o_valid;
                o_stall    = 1'b1;
            end else begin
                next_valid = 1'b0;
                // Hold upstream until drained; on the drained (commit)
                // cycle, hold one more only for the WRSYS variant.
                o_stall = drained ? i_post_commit_wait : 1'b1;
            end
        end else if (i_stall_in) begin
            next_valid = o_valid;       // hold EX/MEM unchanged
            advance    = 1'b0;
            o_stall    = 1'b1;
        end else begin
            next_valid = i_valid;       // advance: bubble in if i_valid=0
            advance    = i_valid;
            o_stall    = 1'b0;
        end
    end

    // ── EX/MEM register ──────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid <= 1'b0;
        end else begin
            o_valid <= next_valid;
            if (advance) begin
                o_op_class       <= i_op_class;
                o_mem_op         <= i_mem_op;
                o_mem_size       <= i_mem_size;
                o_sign_ext       <= i_sign_ext;
                o_sys_dev        <= i_sys_dev;
                o_sys_reg        <= i_sys_reg;
                o_spr_sel        <= i_spr_sel;
                o_gpr_we         <= i_gpr_we;
                o_spr_we         <= i_spr_we;
                o_flag_we        <= i_flag_we;
                o_result         <= result_value;
                o_store_data     <= i_store_data;
                o_flag_value     <= alu_flags;
                o_phys_dst       <= i_phys_dst;
                o_phys_dst_hi    <= i_phys_dst_hi;
                o_phys_dst_hi_en <= i_phys_dst_hi_en;
                o_pc             <= i_pc;
                o_fault_pending  <= i_fault_pending;
                o_fault_vec      <= i_fault_vec;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // EX-owned behavioral invariants the structure does not enforce.
    // Control-bundle consistency (gpr_we needs a dst, is_trap only on
    // SYSCALL/BREAK, dst_hi only on divmul, …) is owned and asserted in
    // penumbra2_decode, so it is not re-checked here.
    // ══════════════════════════════════════════════════════════
    always_comb begin
        // Advance precondition: the EX/MEM register latches a real
        // instruction only on a clean accept — a valid input, not
        // flushed, not back-pressured. A mis-gated latch (advancing
        // on a stall/bubble, or latching a bubble) violates this.
        assert (!advance || (i_valid && !i_bubble && !i_stall_in))
            else $error("penumbra2_ex_stage: advance without a clean precondition");
    end

    // A fault-commit flush from WB always lands as a bubble in EX/MEM:
    // a wrong-path or faulting instruction must never slip through to
    // MEM and commit.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_bubble |=> !o_valid)
        else $error("penumbra2_ex_stage: i_bubble did not flush the EX/MEM slot");

    // Back-pressure holds the EX/MEM slot intact: a stalled EX neither
    // drops nor fabricates its valid bit — the classic lost- or
    // duplicated-instruction bug at a stall boundary.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble) |=> $stable(o_valid))
        else $error("penumbra2_ex_stage: back-pressure changed o_valid");

    // ...and it does not swap the held instruction's identity under it
    // (the EX/MEM payload an upstream stall is waiting on must persist).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble && o_valid) |=> $stable(o_phys_dst))
        else $error("penumbra2_ex_stage: back-pressure swapped the held EX/MEM slot");

    // EX steers the front-end only for an actual control-transfer
    // instruction; a redirect resolved from any other op_class is a
    // branch-resolution bug.
    always_comb begin
        assert (!o_branch_taken ||
                i_op_class == OPC_BRANCH || i_op_class == OPC_JMP)
            else $error("penumbra2_ex_stage: branch redirect on a non-branch op_class");
    end

    // A drain-commit instruction never advances into MEM/WB — its effect
    // is the o_dc_commit pulse, not a downstream pipeline-register slot.
    // If one ever flowed past EX it would double-commit (once via
    // o_dc_commit, once at WB).
    always_comb begin
        assert (!(dc_here && advance))
            else $error("penumbra2_ex_stage: drain-commit insn advanced into EX/MEM");
    end

    // The post-commit hold never re-fires the commit (it is the cycle
    // *after* the WRSYS commit, holding for the device latch).
    always_comb begin
        assert (!(post_wait_q && o_dc_commit))
            else $error("penumbra2_ex_stage: drain-commit re-fired during post-commit hold");
    end

    // The post-commit hold lasts exactly one cycle — never two in a row.
    assert property (@(posedge i_clk) disable iff (i_rst)
        post_wait_q |=> !post_wait_q)
        else $error("penumbra2_ex_stage: post-commit hold exceeded one cycle");

endmodule
