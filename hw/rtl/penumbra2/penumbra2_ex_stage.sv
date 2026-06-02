// penumbra2_ex_stage — Penumbra/2 execute stage (straight-line core).
//
// The integration point of the gen2 datapath spine, mirroring how
// penumbra2_id_stage assembles the front half. Specified by the EX
// sections of doc/internals/penumbra2/{pipeline-stages,control-decode,
// hazard-model}.md. Each cycle it:
//   - feeds the ID/EX operands to the combinational ALU (penumbra2_alu),
//   - forwards the youngest in-flight NZCV (penumbra2_flag_bypass) and
//     hands its carry bit to the ALU for ADC/SBC,
//   - narrows the control bundle to the ctrl_mem + ctrl_wb subsets and
//     latches the EX/MEM register under the back-pressure handshake.
//
// This is the straight-line slice: it computes, forwards flags, and
// advances. Branch resolution (target + squash/redirect), the
// drain-commit FSM, and the divmul start/busy handshake are layered on
// in later steps; the ID/EX inputs and EX-internal state those need are
// added with them, so the port list here is the straight-line subset.
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
    input  logic [MEM_OP_W-1:0]   i_mem_op,
    input  logic [1:0]            i_mem_size,
    input  logic                  i_sign_ext,
    input  logic [3:0]            i_sys_dev,
    input  logic [3:0]            i_sys_reg,
    input  logic [3:0]            i_spr_sel,
    input  logic                  i_gpr_we,
    input  logic                  i_spr_we,
    input  logic                  i_flag_we,
    input  logic [SB_IDX_W-1:0]   i_phys_dst,
    input  logic [SB_IDX_W-1:0]   i_phys_dst_hi,
    input  logic                  i_phys_dst_hi_en,
    input  logic [31:0]           i_pc,
    input  logic                  i_valid,          // 0 = bubble in
    input  logic                  i_fault_pending,
    input  logic [3:0]            i_fault_vec,

    // ── Flag bypass external sources (MEM producer is internal) ──
    input  logic [3:0]            i_sr_flags,        // committed SR NZCV
    input  logic [3:0]            i_wb_flags,        // MEM/WB in-flight producer
    input  logic                  i_wb_writes_flags,

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic                  i_stall_in,        // MEM cannot accept this cycle
    input  logic                  i_bubble,          // squash this insn (fault flush)
    output logic                  o_stall,           // back-pressure to ID

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
    // Only the carry bit feeds the ALU here; the N/Z/V bits feed the
    // branch-condition evaluation added with branch resolution.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [3:0] fwd_flags;
    /* verilator lint_on UNUSEDSIGNAL */
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

    // ── Issue / back-pressure control ────────────────────────────
    // EX accepts a new ID/EX instruction every cycle except when MEM
    // back-pressures it (i_stall_in) — then it holds its EX/MEM output
    // and propagates the stall upstream. i_bubble (a fault flush from
    // WB) squashes the in-flight slot and wins over a hold. The drain-
    // commit and divmul internal stalls layer onto o_stall later.
    logic advance, next_valid;

    always_comb begin
        o_stall = i_stall_in;
        if (i_bubble) begin
            next_valid = 1'b0;
            advance    = 1'b0;
        end else if (i_stall_in) begin
            next_valid = o_valid;       // hold EX/MEM unchanged
            advance    = 1'b0;
        end else begin
            next_valid = i_valid;       // advance: bubble in if i_valid=0
            advance    = i_valid;
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
                o_result         <= alu_result;
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
        // squashed, not back-pressured. A mis-gated latch (advancing
        // on a stall/bubble, or latching a bubble) violates this.
        assert (!advance || (i_valid && !i_bubble && !i_stall_in))
            else $error("penumbra2_ex_stage: advance without a clean precondition");
    end

    // A squash (fault flush from WB) always lands as a bubble in EX/MEM:
    // a wrong-path or faulting instruction must never slip through to
    // MEM and commit.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_bubble |=> !o_valid)
        else $error("penumbra2_ex_stage: i_bubble did not squash the EX/MEM slot");

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

endmodule
