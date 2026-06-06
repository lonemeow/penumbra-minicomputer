// penumbra2_spine — Penumbra/2 ID->EX->MEM->WB datapath integration.
//
// The first time the gen2 stages run as a *pipeline* rather than in
// isolation. It wires the four datapath stages (penumbra2_id_stage,
// _ex_stage, _mem_stage, _wb_stage) around the shared register file and
// closes the loops between them:
//   - the inter-stage registers (ID/EX, EX/MEM, MEM/WB) producer->consumer,
//   - the back-pressure chain WB->MEM->EX->ID->fetch,
//   - the scoreboard's view of the downstream in-flight writers (the loop
//     that makes RAW hazard detection span the whole pipe),
//   - the NZCV flag bypass: a minimal committed-SR flag register feeds EX,
//     alongside the MEM/WB in-flight producer.
//
// There is no instruction fetch yet: the IF2/ID inputs (i_ir/i_pc/...) are
// driven by a testbench acting as the fetch stream, back-pressured by
// o_fetch_stall. Branch redirect and the fault-commit flush are exposed
// (o_branch_*) / tied off — straight-line and divmul streams exercise the
// scoreboard, the dual-write commit, and the handshake without them.
// Loads/stores (MEM guard), RDSYS, and drain-commit (no SR/SPR/EPC yet)
// stay out of the first stream.

module penumbra2_spine
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── Fetch stream in (driven by the testbench; IF lands here later) ──
    input  logic [31:0]           i_ir,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,
    input  logic                  i_valid,
    input  logic                  i_supervisor,
    output logic                  o_fetch_stall,     // hold the fetch stream this cycle

    // ── Commit observability (WB regfile write port) ─────────────
    output logic [SB_IDX_W-1:0]   o_commit_idx,
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we,

    // ── Retire observability (the instruction leaving WB) ────────
    // Every retiring instruction, GPR-writing or not, with its op_class.
    // The consumer (core) reads this to act on retiring control insns the
    // commit port can't see — e.g. a BREAK, which writes no register.
    output logic                  o_retire_valid,
    output logic [OPC_W-1:0]      o_retire_op_class,

    // ── Branch resolution (for a future fetch-redirect model) ────
    output logic                  o_branch_taken,
    output logic [31:0]           o_branch_target,

    // ── Data memory (MEM's BRAM data-side port, exposed to the core) ──
    output logic [31:0]           o_dmem_addr,
    output logic [31:0]           o_dmem_wdata,
    output logic [3:0]            o_dmem_byte_en,
    output logic                  o_dmem_we,
    output logic                  o_dmem_en,
    input  logic [31:0]           i_dmem_rdata,

    // ── Exception entry (to the core: IF flush + vector-fetch FSM) ──
    output logic                  o_fault_commit,    // a fault is being taken this cycle
    output logic [3:0]            o_fault_vec,       // its vector number
    output logic [31:0]           o_epc,             // saved exception PC (ERET / vector-fetch redirect)

    // ── ERET return (to the core: flush IF1/IF2 + redirect PC ← EPC) ──
    output logic                  o_eret_commit      // an ERET is committing this cycle
);

    // This integration deliberately leaves several sub-module outputs
    // unconnected: ID/EX control fields EX does not consume, the deferred
    // MEM load/sysreg path, and the drain-commit / SPR-write strobes with
    // no consumer yet. The empty pin connections below are intentional.
    /* verilator lint_off PINCONNECTEMPTY */

    // ════════════════════════════════════════════════════════════
    // Inter-stage wires (named by the register they carry)
    // ════════════════════════════════════════════════════════════
    // ID/EX
    logic [OPC_W-1:0]    idex_op_class;
    logic [ALU_OP_W-1:0] idex_alu_op;
    logic [1:0]          idex_divmul_op;
    logic [31:0]         idex_op_a, idex_op_b, idex_store_data;
    logic [3:0]          idex_cond;
    logic [MEM_OP_W-1:0] idex_mem_op;
    logic [1:0]          idex_mem_size;
    logic                idex_sign_ext;
    logic [3:0]          idex_sys_dev, idex_sys_reg, idex_spr_sel;
    logic                idex_drain_commit, idex_post_commit_wait;
    logic                idex_gpr_we, idex_spr_we, idex_flag_we;
    logic [SB_IDX_W-1:0] idex_phys_dst, idex_phys_dst_aux;
    logic                idex_phys_dst_aux_en;
    logic [31:0]         idex_pc, idex_next_pc;
    logic                idex_valid, idex_fault_pending;
    logic [3:0]          idex_fault_vec;

    // EX/MEM
    logic [OPC_W-1:0]    exmem_op_class;
    logic [MEM_OP_W-1:0] exmem_mem_op;
    logic [1:0]          exmem_mem_size;
    logic                exmem_sign_ext;
    logic [31:0]         exmem_store_data;
    logic                exmem_gpr_we, exmem_spr_we, exmem_flag_we;
    logic [3:0]          exmem_spr_sel;
    logic [31:0]         exmem_result, exmem_result_aux;
    logic [3:0]          exmem_flag_value;
    logic [SB_IDX_W-1:0] exmem_phys_dst, exmem_phys_dst_aux;
    logic                exmem_phys_dst_aux_en;
    logic [31:0]         exmem_pc;
    logic                exmem_valid, exmem_fault_pending;
    logic [3:0]          exmem_fault_vec;

    // MEM/WB
    logic [OPC_W-1:0]    memwb_op_class;
    logic                memwb_gpr_we, memwb_spr_we, memwb_flag_we;
    logic [3:0]          memwb_spr_sel;
    logic [31:0]         memwb_wb_value, memwb_wb_value_aux;
    logic [3:0]          memwb_flag_value;
    logic [SB_IDX_W-1:0] memwb_phys_dst, memwb_phys_dst_aux;
    logic                memwb_phys_dst_aux_en;
    logic [31:0]         memwb_pc;
    logic                memwb_valid, memwb_fault_pending;
    logic [3:0]          memwb_fault_vec;

    // Fault commit (WB → exception unit / flush)
    logic                wb_fault_commit;
    logic [31:0]         wb_fault_pc;

    // Regfile read ports (ID-driven)
    logic [SB_IDX_W-1:0] rd_idx_a, rd_idx_b;
    logic [31:0]         rd_data_a, rd_data_b;

    // Handshake
    logic id_stall, ex_stall, mem_stall, wb_stall;
    logic ex_branch_taken;
    logic ex_dc_commit;     // EX drain-commit pulse (ERET/WRSYS/WRSPR-SR/EI/DI)

    // An ERET commits this cycle: the drain-commit pulse from EX, gated to the
    // ERET op held in EX. EX owns the *when* (it sequenced the drain); the
    // integration owns the *what* — restore SR from ESR and redirect PC to EPC.
    logic eret_commit;
    assign eret_commit = ex_dc_commit & (idex_op_class == OPC_ERET);

    // WB write port + committed flags
    logic [SB_IDX_W-1:0] wr_idx;
    logic [31:0]         wr_data;
    logic                wr_en;
    logic [3:0]          wb_flag_value;
    logic                wb_flag_we;

    // Scoreboard downstream-writer signals (fed to ID)
    logic [SB_IDX_W-1:0] sb_mem_dst, sb_wb_dst, sb_aux_dst;
    logic                sb_mem_dst_en, sb_wb_dst_en, sb_aux_dst_en;

    // ════════════════════════════════════════════════════════════
    // Privileged save-state registers (SR / ESR / EPC)
    // ════════════════════════════════════════════════════════════
    // WB commits NZCV into SR here; EX's flag bypass reads SR's NZCV as the
    // committed-SR fallback when no in-flight producer forwards. The fault
    // commit drives the save-state pulse (EPC ← faulting PC, ESR ← SR, S=1,
    // I=0). ERET / WRSPR-SPR are not wired into the pipeline yet (no RDSPR /
    // drain-commit-SR path), so those write ports are tied off for now.
    logic [3:0]  spr_sr_flags;
    penumbra2_spr_file u_spr (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_flag_we(wb_flag_we), .i_flag_value(wb_flag_value),
        .i_save_state(wb_fault_commit), .i_save_pc(wb_fault_pc),
        .i_eret(eret_commit),
        .i_spr_we(1'b0), .i_spr_sel(4'd0), .i_spr_value(32'b0),
        .i_rd_sel(4'd0), .o_rd_value(),
        .o_sr_flags(spr_sr_flags),
        .o_sr_s(), .o_sr_i(), .o_sr_read(),
        .o_epc(o_epc), .o_esr()
    );

    // ════════════════════════════════════════════════════════════
    // Register file (shared: ID reads, WB writes)
    // ════════════════════════════════════════════════════════════
    penumbra2_regfile u_regfile (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_rd_idx_a(rd_idx_a), .o_rd_data_a(rd_data_a),
        .i_rd_idx_b(rd_idx_b), .o_rd_data_b(rd_data_b),
        .i_wr_idx(wr_idx), .i_wr_data(wr_data), .i_wr_en(wr_en)
    );

    // ════════════════════════════════════════════════════════════
    // ID — decode / issue (owns the scoreboard)
    // ════════════════════════════════════════════════════════════
    penumbra2_id_stage u_id (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_ir(i_ir), .i_pc(i_pc), .i_next_pc(i_next_pc),
        .i_valid(i_valid), .i_fault_pending(1'b0), .i_fault_vec(4'd0),
        .i_supervisor(i_supervisor),
        .i_stall_in(ex_stall), .i_bubble(ex_branch_taken | wb_fault_commit | eret_commit),
        .o_stall(id_stall),
        .o_rd_idx_a(rd_idx_a), .o_rd_idx_b(rd_idx_b),
        .i_rd_data_a(rd_data_a), .i_rd_data_b(rd_data_b),
        .i_mem_dst(sb_mem_dst), .i_mem_dst_en(sb_mem_dst_en),
        .i_wb_dst(sb_wb_dst),   .i_wb_dst_en(sb_wb_dst_en),
        .i_aux_dst(sb_aux_dst), .i_aux_dst_en(sb_aux_dst_en),
        .o_op_class(idex_op_class), .o_alu_op(idex_alu_op), .o_divmul_op(idex_divmul_op),
        .o_op_a(idex_op_a), .o_op_b(idex_op_b), .o_store_data(idex_store_data),
        .o_cond(idex_cond),
        .o_writes_flags(), .o_reads_flags(), .o_flag_only(),
        .o_mem_op(idex_mem_op), .o_mem_size(idex_mem_size), .o_sign_ext(idex_sign_ext),
        .o_sys_dev(idex_sys_dev), .o_sys_reg(idex_sys_reg), .o_spr_sel(idex_spr_sel),
        .o_drain_commit(idex_drain_commit), .o_post_commit_wait(idex_post_commit_wait),
        .o_gpr_we(idex_gpr_we), .o_spr_we(idex_spr_we), .o_flag_we(idex_flag_we),
        .o_is_trap(),
        .o_phys_dst(idex_phys_dst), .o_phys_dst_aux(idex_phys_dst_aux),
        .o_phys_dst_aux_en(idex_phys_dst_aux_en),
        .o_pc(idex_pc), .o_next_pc(idex_next_pc),
        .o_valid(idex_valid), .o_fault_pending(idex_fault_pending), .o_fault_vec(idex_fault_vec)
    );

    // ════════════════════════════════════════════════════════════
    // EX — execute (ALU, flag bypass, branch, divmul, drain-commit)
    // ════════════════════════════════════════════════════════════
    penumbra2_ex_stage u_ex (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_op_class(idex_op_class), .i_alu_op(idex_alu_op), .i_divmul_op(idex_divmul_op),
        .i_op_a(idex_op_a), .i_op_b(idex_op_b), .i_store_data(idex_store_data),
        .i_cond(idex_cond),
        .i_mem_op(idex_mem_op), .i_mem_size(idex_mem_size), .i_sign_ext(idex_sign_ext),
        .i_sys_dev(idex_sys_dev), .i_sys_reg(idex_sys_reg), .i_spr_sel(idex_spr_sel),
        .i_drain_commit(idex_drain_commit), .i_post_commit_wait(idex_post_commit_wait),
        .i_gpr_we(idex_gpr_we), .i_spr_we(idex_spr_we), .i_flag_we(idex_flag_we),
        .i_phys_dst(idex_phys_dst), .i_phys_dst_aux(idex_phys_dst_aux),
        .i_phys_dst_aux_en(idex_phys_dst_aux_en),
        .i_pc(idex_pc), .i_next_pc(idex_next_pc),
        .i_valid(idex_valid), .i_fault_pending(idex_fault_pending), .i_fault_vec(idex_fault_vec),
        .i_sr_flags(spr_sr_flags),
        .i_wb_flags(memwb_flag_value), .i_wb_writes_flags(memwb_flag_we & memwb_valid),
        .i_stall_in(mem_stall), .i_wb_active(memwb_valid), .i_bubble(wb_fault_commit),
        .o_stall(ex_stall), .o_dc_commit(ex_dc_commit), .o_funit_stall(),
        .o_branch_taken(ex_branch_taken), .o_branch_target(o_branch_target),
        .o_op_class(exmem_op_class), .o_mem_op(exmem_mem_op),
        .o_mem_size(exmem_mem_size), .o_sign_ext(exmem_sign_ext),
        .o_sys_dev(), .o_sys_reg(),
        .o_spr_sel(exmem_spr_sel),
        .o_gpr_we(exmem_gpr_we), .o_spr_we(exmem_spr_we), .o_flag_we(exmem_flag_we),
        .o_result(exmem_result), .o_result_aux(exmem_result_aux),
        .o_store_data(exmem_store_data),
        .o_flag_value(exmem_flag_value),
        .o_phys_dst(exmem_phys_dst), .o_phys_dst_aux(exmem_phys_dst_aux),
        .o_phys_dst_aux_en(exmem_phys_dst_aux_en),
        .o_pc(exmem_pc),
        .o_valid(exmem_valid), .o_fault_pending(exmem_fault_pending), .o_fault_vec(exmem_fault_vec)
    );

    // ════════════════════════════════════════════════════════════
    // MEM — memory access (pass-through skeleton)
    // ════════════════════════════════════════════════════════════
    penumbra2_mem_stage u_mem (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_op_class(exmem_op_class), .i_mem_op(exmem_mem_op),
        .i_mem_size(exmem_mem_size), .i_sign_ext(exmem_sign_ext),
        .i_gpr_we(exmem_gpr_we), .i_spr_we(exmem_spr_we), .i_flag_we(exmem_flag_we),
        .i_spr_sel(exmem_spr_sel),
        .i_result(exmem_result), .i_result_aux(exmem_result_aux),
        .i_store_data(exmem_store_data),
        .i_flag_value(exmem_flag_value),
        .i_phys_dst(exmem_phys_dst), .i_phys_dst_aux(exmem_phys_dst_aux),
        .i_phys_dst_aux_en(exmem_phys_dst_aux_en),
        .i_pc(exmem_pc),
        .i_valid(exmem_valid), .i_fault_pending(exmem_fault_pending),
        .i_fault_vec(exmem_fault_vec),
        .i_stall_in(wb_stall), .i_bubble(wb_fault_commit),
        .o_stall(mem_stall),
        .o_dmem_addr(o_dmem_addr), .o_dmem_wdata(o_dmem_wdata),
        .o_dmem_byte_en(o_dmem_byte_en), .o_dmem_we(o_dmem_we),
        .o_dmem_en(o_dmem_en), .i_dmem_rdata(i_dmem_rdata),
        .o_op_class(memwb_op_class),
        .o_gpr_we(memwb_gpr_we), .o_spr_we(memwb_spr_we), .o_flag_we(memwb_flag_we),
        .o_spr_sel(memwb_spr_sel),
        .o_wb_value(memwb_wb_value), .o_wb_value_aux(memwb_wb_value_aux),
        .o_flag_value(memwb_flag_value),
        .o_phys_dst(memwb_phys_dst), .o_phys_dst_aux(memwb_phys_dst_aux),
        .o_phys_dst_aux_en(memwb_phys_dst_aux_en),
        .o_pc(memwb_pc),
        .o_valid(memwb_valid), .o_fault_pending(memwb_fault_pending),
        .o_fault_vec(memwb_fault_vec)
    );

    // ════════════════════════════════════════════════════════════
    // WB — writeback / commit
    // ════════════════════════════════════════════════════════════
    penumbra2_wb_stage u_wb (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_gpr_we(memwb_gpr_we), .i_spr_we(memwb_spr_we), .i_flag_we(memwb_flag_we),
        .i_spr_sel(memwb_spr_sel),
        .i_wb_value(memwb_wb_value), .i_wb_value_aux(memwb_wb_value_aux),
        .i_flag_value(memwb_flag_value),
        .i_phys_dst(memwb_phys_dst), .i_phys_dst_aux(memwb_phys_dst_aux),
        .i_phys_dst_aux_en(memwb_phys_dst_aux_en),
        .i_pc(memwb_pc),
        .i_valid(memwb_valid), .i_fault_pending(memwb_fault_pending),
        .i_fault_vec(memwb_fault_vec),
        .o_stall(wb_stall),
        .o_wr_idx(wr_idx), .o_wr_data(wr_data), .o_wr_en(wr_en),
        .o_flag_we(wb_flag_we), .o_flag_value(wb_flag_value),
        .o_spr_we(), .o_spr_sel(), .o_spr_value(),
        .o_fault_commit(wb_fault_commit), .o_fault_vec(o_fault_vec),
        .o_fault_pc(wb_fault_pc)
    );

    // The fault-commit and ERET-commit pulses are exposed to the core (IF
    // flush + vector-fetch / ERET redirect to EPC).
    assign o_fault_commit = wb_fault_commit;
    assign o_eret_commit  = eret_commit;

    // ════════════════════════════════════════════════════════════
    // Scoreboard's view of the downstream in-flight writers
    // ════════════════════════════════════════════════════════════
    // Every downstream writer is derived here, uniformly, from the
    // inter-stage register fields with one predicate: a stage holds a
    // pending GPR writer iff its instruction has a GPR destination, is a
    // live slot, and is not faulting. One place owning the predicate keeps
    // it from drifting, and is where the gen2.5 forwarding network — which
    // needs these same tags next to the result values — will later attach.
    // EX/MEM register -> the writer "in MEM"; MEM/WB register -> "in WB".
    // (The EX writer is ID's own registered output, fed back inside ID.)
    logic mem_writer, wb_writer;
    assign mem_writer = exmem_gpr_we & exmem_valid & ~exmem_fault_pending;
    assign wb_writer  = memwb_gpr_we & memwb_valid & ~memwb_fault_pending;

    assign sb_mem_dst    = exmem_phys_dst;
    assign sb_mem_dst_en = mem_writer;
    assign sb_wb_dst     = memwb_phys_dst;
    assign sb_wb_dst_en  = wb_writer;

    // The aux (divmul Rdh) is tracked wherever the dual-write instruction
    // sits — EX, MEM, or WB — each leg gated like the primaries. Priority
    // EX > MEM > WB; at most one is live at a moment ID can issue (an older
    // divmul in EX back-pressures the reader until the younger one drains).
    logic idex_aux_live, exmem_aux_live, memwb_aux_live;
    assign idex_aux_live  = idex_phys_dst_aux_en  & idex_valid  & ~idex_fault_pending;
    assign exmem_aux_live = exmem_phys_dst_aux_en & exmem_valid & ~exmem_fault_pending;
    assign memwb_aux_live = memwb_phys_dst_aux_en & memwb_valid & ~memwb_fault_pending;
    assign sb_aux_dst =
          idex_aux_live  ? idex_phys_dst_aux
        : exmem_aux_live ? exmem_phys_dst_aux
        :                  memwb_phys_dst_aux;
    assign sb_aux_dst_en = idex_aux_live | exmem_aux_live | memwb_aux_live;

    // ── Fetch back-pressure + commit observability ───────────────
    assign o_fetch_stall  = id_stall;
    assign o_branch_taken = ex_branch_taken;
    assign o_commit_idx   = wr_idx;
    assign o_commit_data  = wr_data;
    assign o_commit_we    = wr_en;

    // Retire observability: the MEM/WB slot leaving WB this cycle.
    assign o_retire_valid    = memwb_valid;
    assign o_retire_op_class = memwb_op_class;

    // ════════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ════════════════════════════════════════════════════════════

    // Aux-writer mutual exclusion. The scoreboard has a single aux port, so
    // sb_aux_dst priority-muxes EX>MEM>WB and exposes only the youngest live
    // aux destination. That is sound only because at most one aux writer is
    // live at any moment ID could issue — two live aux would hide the older
    // one's high-half (Rdh) destination and let a dependent reader issue early
    // (its primary Rd is still covered by i_mem/wb_dst; only Rdh is at risk).
    // Two aux can momentarily coexist (back-to-back divmuls: one entering EX as
    // the other reaches MEM/WB), but only while the younger divmul holds ID via
    // its ~33-cycle EX stall — so id_stall is the carve-out. If this ever fires
    // with id_stall low, the priority mux is dropping a real RAW hazard.
    assert property (@(posedge i_clk) disable iff (i_rst)
        id_stall || $onehot0({idex_aux_live, exmem_aux_live, memwb_aux_live}))
        else $error("penumbra2_spine: multiple aux writers live while ID can issue");

    // A GPR commit implies a retiring instruction: the regfile write port and
    // the retire pulse both come from the MEM/WB slot and must not decouple.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_commit_we |-> o_retire_valid)
        else $error("penumbra2_spine: GPR commit without a retiring instruction");

    /* verilator lint_on PINCONNECTEMPTY */
endmodule
