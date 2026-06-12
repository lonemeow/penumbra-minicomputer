// penumbra2_id_stage — Penumbra/2 instruction decode / issue stage.
//
// The integration point of the gen2 front half. Specified by the ID
// sections of doc/internals/penumbra2/{pipeline-stages,hazard-model,
// control-decode}.md. Each cycle it:
//   - decodes the IF2/ID instruction (penumbra2_decode),
//   - maps its architectural register references to physical scoreboard
//     entries (penumbra2_regmap),
//   - checks the scoreboard for a RAW hazard (penumbra2_scoreboard) and
//     stalls issue if a source is pending,
//   - drives the regfile read-index ports and selects the final ALU
//     operands (op_a/op_b) and the store-data value,
//   - latches the control bundle + operands + PC into the ID/EX
//     register under the back-pressure handshake.
//
// Boundary: the regfile is external (this stage drives its read indices
// and consumes the async read data; WB owns the write port). The
// scoreboard lives here. Its in-flight-writer inputs come from the
// stages downstream of EX — MEM and WB destinations and the divmul
// auxiliary (Rdh) arrive as ports — while the EX-stage writer is this
// stage's own registered phys_dst, fed back internally: the instruction
// "in EX" is exactly what ID latched last cycle.
//
// NZCV is not a scoreboard entry (it is forwarded in EX), so flag reads
// never appear as scoreboard sources and never stall here.

module penumbra2_id_stage
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── IF2/ID input: the instruction available to decode ────────
    input  logic [31:0]           i_ir,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,
    input  logic                  i_valid,          // 0 = bubble in
    input  logic                  i_fault_pending,  // IF-stage fault
    input  logic [3:0]            i_fault_vec,
    input  logic [31:0]           i_fault_status,   // its composed payload; FAULT_NONE when none

    input  logic                  i_supervisor,     // SR.S

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic                  i_stall_in,       // EX cannot accept this cycle
    input  logic                  i_bubble,         // force a bubble this edge (taken-branch redirect / fault flush)
    output logic                  o_stall,          // back-pressure to IF

    // ── Regfile read interface (regfile is external) ─────────────
    output logic [SB_IDX_W-1:0]   o_rd_idx_a,
    output logic [SB_IDX_W-1:0]   o_rd_idx_b,
    input  logic [31:0]           i_rd_data_a,
    input  logic [31:0]           i_rd_data_b,

    // ── Scoreboard: in-flight writers downstream of EX ───────────
    input  logic [SB_IDX_W-1:0]   i_mem_dst,
    input  logic                  i_mem_dst_en,
    input  logic [SB_IDX_W-1:0]   i_wb_dst,
    input  logic                  i_wb_dst_en,
    input  logic [SB_IDX_W-1:0]   i_aux_dst,        // divmul Rdh, wherever it is in flight
    input  logic                  i_aux_dst_en,

    // ── ID/EX register (to EX) ───────────────────────────────────
    output logic [OPC_W-1:0]      o_op_class,
    output logic [ALU_OP_W-1:0]   o_alu_op,
    output logic [1:0]            o_divmul_op,
    output logic [31:0]           o_op_a,
    output logic [31:0]           o_op_b,
    output logic [31:0]           o_store_data,
    output logic [3:0]            o_cond,
    output logic                  o_writes_flags,
    output logic                  o_reads_flags,
    output logic                  o_flag_only,
    output logic [MEM_OP_W-1:0]   o_mem_op,
    output logic [1:0]            o_mem_size,
    output logic                  o_sign_ext,
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    output logic [3:0]            o_spr_sel,
    output logic                  o_drain_commit,
    output logic                  o_post_commit_wait,
    output logic                  o_gpr_we,
    output logic                  o_spr_we,
    output logic                  o_flag_we,
    output logic                  o_is_trap,
    output logic [SB_IDX_W-1:0]   o_phys_dst,
    output logic [SB_IDX_W-1:0]   o_phys_dst_aux,
    output logic                  o_phys_dst_aux_en,  // aux dst present — drives the scoreboard aux
    output logic [31:0]           o_pc,
    output logic [31:0]           o_next_pc,
    output logic                  o_valid,
    output logic                  o_fault_pending,
    output logic [3:0]            o_fault_vec,
    output logic [31:0]           o_fault_status
);

    // ── Decode (combinational) ───────────────────────────────────
    logic [OPC_W-1:0]    d_op_class;
    logic [3:0]          d_src_a_sel, d_src_b_sel, d_dst_sel, d_dst_aux_sel;
    logic                d_src_a_is_spr, d_src_a_en;
    logic                d_src_b_is_spr, d_src_b_en;
    logic                d_dst_is_spr, d_dst_en, d_dst_aux_en;
    logic [ALU_OP_W-1:0] d_alu_op;
    logic [1:0]          d_divmul_op;
    logic                d_a_from_pc, d_b_from_imm;
    logic [31:0]         d_imm;
    logic [3:0]          d_cond;
    logic                d_writes_flags, d_reads_flags, d_flag_only;
    logic [MEM_OP_W-1:0] d_mem_op;
    logic [1:0]          d_mem_size;
    logic                d_sign_ext;
    logic [3:0]          d_sys_dev, d_sys_reg, d_spr_sel;
    logic                d_drain_commit, d_post_commit_wait;
    logic                d_gpr_we, d_spr_we, d_flag_we;
    logic                d_is_trap, d_illegal, d_priv_fault;
    logic [3:0]          d_fault_vec;

    penumbra2_decode u_decode (
        .i_ir(i_ir), .i_supervisor(i_supervisor),
        .o_op_class(d_op_class),
        .o_src_a_sel(d_src_a_sel), .o_src_a_is_spr(d_src_a_is_spr), .o_src_a_en(d_src_a_en),
        .o_src_b_sel(d_src_b_sel), .o_src_b_is_spr(d_src_b_is_spr), .o_src_b_en(d_src_b_en),
        .o_dst_sel(d_dst_sel),     .o_dst_is_spr(d_dst_is_spr),     .o_dst_en(d_dst_en),
        .o_dst_aux_sel(d_dst_aux_sel), .o_dst_aux_en(d_dst_aux_en),
        .o_alu_op(d_alu_op), .o_divmul_op(d_divmul_op),
        .o_a_from_pc(d_a_from_pc), .o_b_from_imm(d_b_from_imm),
        .o_imm(d_imm), .o_cond(d_cond),
        .o_writes_flags(d_writes_flags), .o_reads_flags(d_reads_flags), .o_flag_only(d_flag_only),
        .o_mem_op(d_mem_op), .o_mem_size(d_mem_size), .o_sign_ext(d_sign_ext),
        .o_sys_dev(d_sys_dev), .o_sys_reg(d_sys_reg), .o_spr_sel(d_spr_sel),
        .o_drain_commit(d_drain_commit), .o_post_commit_wait(d_post_commit_wait),
        .o_gpr_we(d_gpr_we), .o_spr_we(d_spr_we), .o_flag_we(d_flag_we),
        .o_is_trap(d_is_trap), .o_illegal(d_illegal), .o_priv_fault(d_priv_fault),
        .o_fault_vec(d_fault_vec)
    );

    // ── ISA→physical register mapping (combinational) ────────────
    logic [SB_IDX_W-1:0] phys_src_a, phys_src_b, phys_dst, phys_dst_aux;
    logic                phys_src_a_en, phys_src_b_en, phys_dst_en, phys_dst_aux_en;
    // cross_bank is informational: physical addressing already maps an
    // SPR-USP access to SB_USP, so no consumer here needs it.
    /* verilator lint_off UNUSEDSIGNAL */
    logic                cross_bank;
    /* verilator lint_on UNUSEDSIGNAL */

    penumbra2_regmap u_regmap (
        .i_supervisor(i_supervisor),
        .i_src_a_sel(d_src_a_sel), .i_src_a_is_spr(d_src_a_is_spr), .i_src_a_en(d_src_a_en),
        .i_src_b_sel(d_src_b_sel), .i_src_b_is_spr(d_src_b_is_spr), .i_src_b_en(d_src_b_en),
        .i_dst_sel(d_dst_sel),     .i_dst_is_spr(d_dst_is_spr),     .i_dst_en(d_dst_en),
        .i_dst_aux_sel(d_dst_aux_sel), .i_dst_aux_en(d_dst_aux_en),
        .o_src_a(phys_src_a), .o_src_a_en(phys_src_a_en),
        .o_src_b(phys_src_b), .o_src_b_en(phys_src_b_en),
        .o_dst(phys_dst),     .o_dst_en(phys_dst_en),
        .o_dst_aux(phys_dst_aux), .o_dst_aux_en(phys_dst_aux_en),
        .o_cross_bank(cross_bank)
    );

    // ── Regfile read drive (async read; data returns this cycle) ──
    assign o_rd_idx_a = phys_src_a;
    assign o_rd_idx_b = phys_src_b;

    // ── Operand select: ID produces the final ALU operands ───────
    // op_a/op_b are what EX feeds the ALU directly; store_data is the
    // raw port-B read (the value a store writes).
    logic [31:0] op_a_sel, op_b_sel, store_data_sel;
    assign op_a_sel       = d_a_from_pc  ? i_pc  : i_rd_data_a;
    assign op_b_sel       = d_b_from_imm ? d_imm : i_rd_data_b;
    assign store_data_sel = i_rd_data_b;

    // ── Fault tag for the decoded instruction ────────────────────
    // An IF-stage fault outranks decode faults; SYSCALL/BREAK are
    // traps, not faults (they ride is_trap). A faulting or bubble slot
    // must be inert to the scoreboard.
    logic       insn_fault_pending;
    logic [3:0] insn_fault_vec;
    assign insn_fault_pending = i_fault_pending | d_illegal | d_priv_fault;
    assign insn_fault_vec     = i_fault_pending ? i_fault_vec : d_fault_vec;

    logic sb_eligible;   // this ID slot actually participates in the scoreboard
    assign sb_eligible = i_valid & ~insn_fault_pending;

    // ── Scoreboard ───────────────────────────────────────────────
    // EX writer = this stage's registered phys_dst (the insn now in
    // EX). ex_dst_en_r is its scoreboard-destination enable, registered
    // alongside the bundle and gated by validity / non-fault below.
    logic ex_dst_en_r;
    logic scoreboard_stall;
    /* verilator lint_off UNUSEDSIGNAL */
    logic [SB_NUM_ENTRIES-1:0] sb_valid;
    /* verilator lint_on UNUSEDSIGNAL */

    penumbra2_scoreboard u_scoreboard (
        .i_src_a(phys_src_a), .i_src_a_en(phys_src_a_en & sb_eligible),
        .i_src_b(phys_src_b), .i_src_b_en(phys_src_b_en & sb_eligible),
        .i_ex_dst(o_phys_dst),  .i_ex_dst_en(ex_dst_en_r & o_valid & ~o_fault_pending),
        .i_mem_dst(i_mem_dst),  .i_mem_dst_en(i_mem_dst_en),
        .i_wb_dst(i_wb_dst),    .i_wb_dst_en(i_wb_dst_en),
        .i_aux_dst(i_aux_dst),  .i_aux_dst_en(i_aux_dst_en),
        .o_valid(sb_valid),
        .o_stall(scoreboard_stall)
    );

    // ── Issue / back-pressure control ────────────────────────────
    // can_issue : the ID instruction is eligible to advance into EX —
    //             a real, non-RAW-stalled slot. A fault-tagged insn is
    //             eligible too: it never scoreboard-stalls (sb_eligible
    //             gates its sources off) and faults at WB.
    // issue     : it actually advances into EX this edge — latch the
    //             decoded bundle + operands into ID/EX.
    // next_valid: the ID/EX valid bit after this edge.
    // o_stall   : back-pressure to IF (hold the IF2/ID input).
    logic can_issue, issue, next_valid;
    assign can_issue = i_valid & ~scoreboard_stall;

    always_comb begin
        o_stall = i_stall_in || (i_valid && scoreboard_stall);
        if (i_bubble) begin
            next_valid = 1'b0;          // flush the in-flight insn to a bubble
            issue      = 1'b0;
        end else if (i_stall_in) begin
            next_valid = o_valid;       // hold ID/EX unchanged
            issue      = 1'b0;
        end else begin
            next_valid = can_issue;     // advance: issue if eligible, else bubble
            issue      = can_issue;
        end
    end

    // ── ID/EX register ───────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid <= 1'b0;
        end else begin
            o_valid <= next_valid;
            if (issue) begin
                o_op_class         <= d_op_class;
                o_alu_op           <= d_alu_op;
                o_divmul_op        <= d_divmul_op;
                o_op_a             <= op_a_sel;
                o_op_b             <= op_b_sel;
                o_store_data       <= store_data_sel;
                o_cond             <= d_cond;
                o_writes_flags     <= d_writes_flags;
                o_reads_flags      <= d_reads_flags;
                o_flag_only        <= d_flag_only;
                o_mem_op           <= d_mem_op;
                o_mem_size         <= d_mem_size;
                o_sign_ext         <= d_sign_ext;
                o_sys_dev          <= d_sys_dev;
                o_sys_reg          <= d_sys_reg;
                o_spr_sel          <= d_spr_sel;
                o_drain_commit     <= d_drain_commit;
                o_post_commit_wait <= d_post_commit_wait;
                o_gpr_we           <= d_gpr_we;
                o_spr_we           <= d_spr_we;
                o_flag_we          <= d_flag_we;
                // Fault outranks trap in the exception priority, so a
                // faulting slot is never also a trap (an IF-faulted word
                // could otherwise decode as SYSCALL/BREAK).
                o_is_trap          <= d_is_trap & ~insn_fault_pending;
                o_phys_dst         <= phys_dst;
                o_phys_dst_aux     <= phys_dst_aux;
                o_phys_dst_aux_en  <= phys_dst_aux_en;
                o_pc               <= i_pc;
                o_next_pc          <= i_next_pc;
                o_fault_pending    <= insn_fault_pending;
                o_fault_vec        <= insn_fault_vec;
                // The carried payload is self-qualifying: an IF address fault
                // arrives with its composed status, and i_fault_status is
                // FAULT_NONE otherwise — so a decode fault raised here rides
                // FAULT_NONE with no mux.
                o_fault_status     <= i_fault_status;
                ex_dst_en_r        <= phys_dst_en;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // Non-structural preconditions this stage owns.
    // ══════════════════════════════════════════════════════════
    always_comb begin
        // Issue precondition: an instruction advances into EX only when
        // it is a real, non-RAW-stalled slot and we are neither
        // flushing it nor back-pressured. A mis-gated advance (issuing
        // on a stall/bubble, or ignoring the scoreboard) violates this.
        assert (!issue || (i_valid && !scoreboard_stall && !i_bubble && !i_stall_in))
            else $error("penumbra2_id_stage: issue without a clean issue precondition");
    end

    // A committed ID/EX slot is never both faulting and trapping —
    // fault outranks trap, and is_trap is suppressed when the slot
    // faults.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_valid |-> !(o_fault_pending && o_is_trap))
        else $error("penumbra2_id_stage: ID/EX slot is both faulting and trapping");

endmodule
