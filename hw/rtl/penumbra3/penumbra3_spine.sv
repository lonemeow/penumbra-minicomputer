// penumbra3_spine -- Penumbra/3 ID->EX->MEM1->MEM2->WB datapath integration.
//
// Instantiates the five datapath stages around the shared register / SPR /
// scratch files, and closes the loops between them:
//   - the inter-stage handoffs (each stage holds its own output register, so
//     this is producer.o_* -> consumer.i_* wiring, carrying the packed
//     ctrl_bundle_t + dpath_payload_t),
//   - the operand- and flag-forward *sources* into EX (the network lives in EX;
//     the spine feeds it the MEM2-resolved and MEM2/WB-register producers),
//   - the scoreboard clear (the back end tells ID when a tracked producer's
//     value has landed),
//   - the back-pressure / freeze distribution -- the timing-critical part: the
//     registered mem_pending freezes the launch side and issue, the MEM2/WB
//     register is held only by WB back-pressure, and a fault/branch redirect
//     flushes the younger slots (see "What the pipe gate freezes" in
//     doc/internals/penumbra3/memory-completion.md).
//
// The MMU/TLB and the caches live outside (the machine layer binds them to the
// exposed launch/verdict ports): the TLB is shared coherently with the
// I-side copy, so it is not a spine-private module.
//
// A bus fault on a parked access commits from its completion, in WB's place.
// A deferred bus fault, on a cacheable store's write, enters at EX instead,
// injected as an interrupt is (doc/internals/penumbra3/memory-completion.md,
// "Fault precision").

(* keep_hierarchy = "yes" *)
module penumbra3_spine
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── Fetch-FIFO head in (pre-decoded bundle + PC + IF-side fault) ──
    input  ctrl_bundle_t          i_bundle,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,
    input  logic                  i_valid,
    input  logic                  i_if_fault_pending,
    input  logic [3:0]            i_if_fault_vec,
    input  logic [31:0]           i_if_fault_status,
    input  logic                  i_fetch_busy,
    output logic                  o_deq_ready,        // consume the FIFO head

    // ── Translation (MEM1 launch out / MEM2 verdict in; external dtranslate) ──
    output logic                  o_translate_en,
    output logic [31:0]           o_translate_vaddr,
    output logic [2:0]            o_translate_acc_type,
    output logic                  o_mem_launch_hold,  // freeze dtranslate + cache launch side
    output logic                  o_user_mode,        // translate/access privilege
    input  logic [31:0]           i_translate_paddr,
    input  logic                  i_translate_cacheable,
    input  logic                  i_translate_hit,
    input  logic                  i_translate_miss_fault,
    input  logic                  i_translate_prot_fault,

    // ── L1-D (MEM1 launch / MEM2 resolve, store strobe, counting, access) ──
    // doc/internals/penumbra3/l1-cache.md. A store's data and lanes serve
    // its hit write and its access both.
    output logic                  o_dcache_en,
    output logic [31:0]           o_dcache_vaddr,
    input  logic                  i_dcache_hit,
    input  logic [31:0]           i_dcache_rdata,
    output logic                  o_dcache_we,
    output logic [31:0]           o_dcache_wdata,
    output logic [3:0]            o_dcache_byte_en,
    output logic                  o_dcache_count_read,
    output logic                  o_dcache_count_write,
    output logic                  o_dcache_access_req,
    output logic                  o_dcache_access_we,
    output logic [31:0]           o_dcache_access_addr,
    output logic                  o_dcache_access_cacheable,
    input  logic                  i_dcache_busy,
    input  logic                  i_dcache_access_done,
    input  logic [31:0]           i_dcache_access_word,
    input  logic                  i_dcache_access_fault,

    // ── Sysreg read (MEM1 launch) + write (WRSYS drain-commit) ──
    output logic                  o_sys_re,
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    input  logic [31:0]           i_sys_rdata,
    output logic                  o_sys_we,
    output logic [3:0]            o_sys_wr_dev,
    output logic [3:0]            o_sys_wr_reg,
    output logic [31:0]           o_sys_wdata,

    // ── Branch resolution (to the front end) ──
    output logic                  o_branch_taken,
    output logic [31:0]           o_branch_target,

    // ── Exception entry / return / WRSYS resync (to the front end) ──
    output logic                  o_fault_commit,
    output logic [3:0]            o_fault_vec,
    output logic [31:0]           o_epc,
    output logic                  o_eret_commit,
    output logic                  o_wrsys_resync,
    output logic [31:0]           o_wrsys_resync_pc,

    // ── MMU fault-register commit (FADDR / FSTAT latch) ──
    output logic                  o_mmu_fault_commit,
    output logic [31:0]           o_mmu_fault_vaddr,
    output logic [31:0]           o_mmu_fault_status,

    // ── Interrupt unit ──
    // o_ex_valid / o_ex_stall expose the EX frontier the interrupt unit gates
    // on: an interrupt is injected only when EX holds a real instruction and is
    // not back-pressured, so EPC and the saved SR land on a clean boundary.
    output logic                  o_sr_s,
    output logic                  o_sr_i,
    output logic                  o_ei_commit,
    output logic                  o_dc_commit,
    output logic                  o_ex_valid,
    output logic                  o_ex_stall,
    input  logic                  i_irq_inject,
    input  logic [3:0]            i_irq_vec,
    input  logic                  i_deferred_inject,
    input  logic [31:0]           i_deferred_addr,

    // ── Observability (perfctr / trace) ──
    output logic                  o_insn_committed,
    output bcause_e               o_bcause,
    output logic                  o_retire_valid,
    output logic [31:0]           o_retire_pc,
    output op_class_e             o_retire_op_class,
    output logic [31:0]           o_retire_sr,        // the committed SR as it retires
    output logic [SB_IDX_W-1:0]   o_commit_idx,       // regfile write port (commit / load completion)
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we,
    // A drain-commit retires from EX; a branch resolves there.
    output logic [31:0]           o_dc_commit_pc,
    output op_class_e             o_dc_commit_op_class,
    output logic [31:0]           o_branch_pc,
    // The slots in EX and MEM2, for a trace of the pipeline's occupancy.
    output logic [31:0]           o_ex_pc,
    output logic [31:0]           o_mem_pc,
    output logic                  o_mem_valid
);

    // Several sub-module outputs are deliberately unconsumed here (the ID
    // perfctr local-stall, EX's funit-stall, the SPR file's ESR readback); the
    // empty pin connections below are intentional.
    /* verilator lint_off PINCONNECTEMPTY */

    // ════════════════════════════════════════════════════════════
    // Inter-stage wires (each stage owns its output register)
    // ════════════════════════════════════════════════════════════
    // ID/EX
    ctrl_bundle_t        idex_bundle;
    logic [31:0]         idex_op_a, idex_op_b, idex_store_data;
    logic [SB_IDX_W-1:0] idex_phys_dst, idex_phys_dst_aux;
    logic                idex_phys_dst_we, idex_phys_dst_aux_we;
    logic [SB_IDX_W-1:0] idex_phys_src_a, idex_phys_src_b;
    logic                idex_src_a_fwdable, idex_src_b_fwdable;
    logic [31:0]         idex_pc, idex_next_pc;
    logic                idex_valid, idex_fault_pending;
    logic [3:0]          idex_fault_vec;
    logic [31:0]         idex_fault_status;
    bcause_e             idex_bcause;

    // EX/MEM1
    ctrl_bundle_t        exmem1_bundle;
    dpath_payload_t      exmem1_payload;
    logic [31:0]         exmem1_store_data;
    logic                exmem1_valid;
    bcause_e             exmem1_bcause;

    // MEM1/MEM2
    ctrl_bundle_t        mem1mem2_bundle;
    dpath_payload_t      mem1mem2_payload;
    logic [31:0]         mem1mem2_store_wdata;
    logic [3:0]          mem1mem2_byte_en;
    logic                mem1mem2_align_fault;
    logic                mem1mem2_valid;
    bcause_e             mem1mem2_bcause;

    // MEM2/WB
    ctrl_bundle_t        mem2wb_bundle;
    dpath_payload_t      mem2wb_payload;
    logic                mem2wb_valid;
    bcause_e             mem2wb_bcause;

    // MEM2 forward + back-end completion
    logic [31:0]         mem2_fwd_result;
    logic [SB_IDX_W-1:0] mem2_fwd_dst;
    logic                mem2_fwd_we, mem2_fwd_valid;
    logic                mc_complete, mc_complete_we, mc_complete_retires;
    logic [SB_IDX_W-1:0] mc_complete_dest;
    logic [31:0]         mc_complete_value;
    logic                mc_complete_fault;
    logic [31:0]         mc_complete_pc, mc_complete_vaddr, mc_complete_status;
    logic                mem_pending;

    // WB write/commit strobes
    logic [SB_IDX_W-1:0] wb_regfile_idx;
    logic [31:0]         wb_regfile_data;
    logic                wb_regfile_we;
    logic                wb_spr_we, wb_flag_we;
    logic [3:0]          wb_spr_sel, wb_flag_value;
    logic [31:0]         wb_spr_value;
    logic                wb_local_stall, wb_insn_committed;
    logic                wb_fault_commit;
    logic [3:0]          wb_fault_vec;
    logic [31:0]         wb_fault_pc, wb_fault_vaddr, wb_fault_status;
    logic                fault_commit;
    logic [3:0]          fault_vec;
    logic [31:0]         fault_pc, fault_vaddr, fault_status;

    // EX side-effects
    logic                ex_local_stall, ex_dc_commit, ex_branch_taken;

    // ════════════════════════════════════════════════════════════
    // Back-pressure / freeze distribution
    // ════════════════════════════════════════════════════════════
    // The local stalls each stage exposes (downstream-independent), plus the
    // registered back-end hold. These three are the only inputs the freeze
    // composition needs; everything below is derived from them.
    //   - mem_pending   : the L1-D's registered busy, through MEM2, never a
    //                     live cache/TLB verdict.
    //   - wb_local_stall: WB's dual-write hold (flop-shallow).
    //   - ex_local_stall: EX's divmul / drain-commit hold (flop-shallow).

    logic launch_hold;    // freeze MEM1 + the MEM1/MEM2 register + dtranslate + cache;
                          // MEM2 reads it as "my slot stays"
    logic mem2_reg_hold;  // freeze the MEM2/WB register
    logic ex_stall_in;    // MEM1 cannot accept (-> EX)
    logic id_stall_in;    // EX cannot accept (-> ID register hold)
    logic id_pipe_hold;   // registered back-end hold (-> ID issue gate)

    // The freeze distribution: the five signals above are driven from
    // {mem_pending, wb_local_stall, ex_local_stall}, per the load-completion
    // freeze contract and the stall audit:
    //   - launch_hold: the launch side (MEM1 register, dtranslate, cache) holds
    //     when a load is pending OR the MEM2/WB register is back-pressured.
    //   - mem2_reg_hold: the MEM2/WB register holds ONLY on WB back-pressure --
    //     it must NOT include mem_pending (it drains older work to commit while
    //     a load waits; folding mem_pending in re-presents a committed
    //     multi-cycle writeback).
    //   - ex_stall_in: MEM1 cannot accept a new slot this cycle (its register is
    //     frozen).
    //   - id_stall_in: EX cannot accept -- EX is itself stalling OR is
    //     back-pressured. (This must include mem_pending so a load-frozen EX
    //     holds the ID/EX register instead of bubbling and losing the slot.)
    //   - id_pipe_hold: the registered back-end hold the issue gate reads.
    always_comb begin
        launch_hold   = mem_pending | wb_local_stall;
        mem2_reg_hold = wb_local_stall;
        ex_stall_in   = mem_pending | wb_local_stall;
        id_stall_in   = ex_local_stall | mem_pending | wb_local_stall;
        id_pipe_hold  = mem_pending;
    end

    // Inclusive EX stall: used to gate the branch bubble so a held branch (e.g.
    // back-pressured behind a load) is not discarded before it resolves.
    logic ex_stall;
    assign ex_stall = ex_local_stall | ex_stall_in;

    // EX-frontier observability for the interrupt unit (the clean-boundary gate).
    assign o_ex_valid = idex_valid;
    assign o_ex_stall = ex_stall;

    // ════════════════════════════════════════════════════════════
    // Redirect / flush distribution
    // ════════════════════════════════════════════════════════════
    // Drain-commit effects, gated from EX's single commit pulse by the op held
    // in EX. EX owns the *when*; the integration owns the *what*.
    logic eret_commit, ei_commit, di_commit;
    assign eret_commit = ex_dc_commit & (idex_bundle.op_class == OPC_ERET);
    assign ei_commit   = ex_dc_commit & (idex_bundle.op_class == OPC_EI);
    assign di_commit   = ex_dc_commit & (idex_bundle.op_class == OPC_DI);

    // WRSYS sysreg write, composed from the held ID/EX fields at the commit; the
    // value rode op_b (the Rd read). Context-synchronizing: one cycle later (the
    // post-commit-wait window, by when the device has latched) re-fetch the
    // successor so it observes the new state.
    assign o_sys_we     = ex_dc_commit & (idex_bundle.op_class == OPC_WRSYS);
    assign o_sys_wr_dev = idex_bundle.sys_dev;
    assign o_sys_wr_reg = idex_bundle.sys_reg;
    assign o_sys_wdata  = idex_op_b;

    logic        wrsys_resync_q;
    logic [31:0] wrsys_resync_pc_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) wrsys_resync_q <= 1'b0;
        else       wrsys_resync_q <= o_sys_we;
        if (o_sys_we) wrsys_resync_pc_q <= idex_next_pc;
    end
    assign o_wrsys_resync    = wrsys_resync_q;
    assign o_wrsys_resync_pc = wrsys_resync_pc_q;

    // A fault commits from one of two places, each the oldest in flight then:
    // WB with a fault its slot carries, or a parked access's completion with
    // a bus fault. A pending access drains WB to bubbles, so they never meet.
    always_comb begin
        if (mc_complete_fault) begin
            fault_commit = 1'b1;
            fault_vec    = VEC_BUS_FAULT;
            fault_pc     = mc_complete_pc;
            fault_vaddr  = mc_complete_vaddr;
            fault_status = mc_complete_status;
        end else begin
            fault_commit = wb_fault_commit;
            fault_vec    = wb_fault_vec;
            fault_pc     = wb_fault_pc;
            fault_vaddr  = wb_fault_vaddr;
            fault_status = wb_fault_status;
        end
    end

    // Per-stage flush (i_bubble). A fault commit is the oldest, so it flushes
    // every younger slot (ID/EX/MEM1/MEM2). A taken branch flushes only its
    // wrong-path successor in ID -- and only once it advances out of EX
    // (~ex_stall keeps a held branch from being discarded). ERET / WRSYS-resync
    // fire with the pipe drained, so they only flush ID.
    logic id_bubble, younger_flush;
    assign younger_flush = fault_commit;
    assign id_bubble = (ex_branch_taken & ~ex_stall) | younger_flush
                     | eret_commit | wrsys_resync_q;

    // ════════════════════════════════════════════════════════════
    // Drain-commit gate
    // ════════════════════════════════════════════════════════════
    // ERET/WRSYS/EI/DI commit from EX, ahead of the back end, so EX holds one
    // until everything older than it is finished. EX itself watches its
    // EX/MEM1 register; this is everything past that.
    logic back_end_active;
    assign back_end_active = mem1mem2_valid | mem2wb_valid | mem_pending | mc_complete;

    // ════════════════════════════════════════════════════════════
    // Privileged save-state files (SR / EPC / ESR, scratch)
    // ════════════════════════════════════════════════════════════
    logic        supervisor;
    logic [3:0]  sr_flags;
    logic [31:0] sr_committed;
    assign o_sr_s     = supervisor;
    assign o_user_mode = ~supervisor;

    // A fault commit drives the save-state pulse (EPC <- faulting PC, ESR <- SR,
    // S=1, I=0); an interrupt rides the same path (EX tags its insn as a
    // synthetic fault), so fault_commit covers both and fault_pc is the
    // committing instruction's own PC either way.
    logic        save_state;
    logic [31:0] save_pc;
    assign save_state = fault_commit;
    assign save_pc    = fault_pc;

    // WB's single SPR strobe routes by SPR number: EPC/ESR to the SPR file, SCRn
    // to the scratch file. USP is regfile-backed, so WB drives it on the regfile
    // port, never here.
    logic        spr_file_we, scr_we;
    assign spr_file_we = wb_spr_we & (wb_spr_sel == SPR_EPC | wb_spr_sel == SPR_ESR);
    assign scr_we      = wb_spr_we & (wb_spr_sel >= SPR_SCR0) & (wb_spr_sel <= SPR_SCR3);

    logic [3:0]  id_spr_rd_sel;
    logic [31:0] spr_rd_value, scr_rd_value, spr_operand_value;
    logic        id_rd_is_scr;
    assign id_rd_is_scr      = (id_spr_rd_sel >= SPR_SCR0) & (id_spr_rd_sel <= SPR_SCR3);
    assign spr_operand_value = id_rd_is_scr ? scr_rd_value : spr_rd_value;

    penumbra3_spr_file u_spr (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_flag_we(wb_flag_we), .i_flag_value(wb_flag_value),
        .i_save_state(save_state), .i_save_pc(save_pc),
        .i_eret(eret_commit), .i_ei(ei_commit), .i_di(di_commit),
        .i_spr_we(spr_file_we), .i_spr_sel(wb_spr_sel), .i_spr_value(wb_spr_value),
        .i_rd_sel(id_spr_rd_sel), .o_rd_value(spr_rd_value),
        .o_sr_flags(sr_flags), .o_sr_s(supervisor), .o_sr_i(o_sr_i),
        .o_sr_read(sr_committed), .o_epc(o_epc), .o_esr()
    );

    penumbra3_scratch_file u_scr (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_we(scr_we), .i_w_sel(wb_spr_sel), .i_w_value(wb_spr_value),
        .i_rd_sel(id_spr_rd_sel), .o_rd_value(scr_rd_value)
    );

    // ════════════════════════════════════════════════════════════
    // Register file (ID reads; WB commit + load completion write)
    // ════════════════════════════════════════════════════════════
    // One write port, muxed between the WB commit and a parked load's
    // completion; a store's completion writes no register. They are
    // temporally exclusive: while an access is pending the MEM2/WB register
    // drains to bubbles, so no WB commit coincides with o_complete.
    logic [SB_IDX_W-1:0] rf_rd_idx_a, rf_rd_idx_b, rf_wr_idx;
    logic [31:0]         rf_rd_data_a, rf_rd_data_b, rf_wr_data;
    logic                rf_wr_en;
    assign rf_wr_en   = mc_complete ? mc_complete_we & ~mc_complete_fault : wb_regfile_we;
    assign rf_wr_idx  = mc_complete ? mc_complete_dest   : wb_regfile_idx;
    assign rf_wr_data = mc_complete ? mc_complete_value  : wb_regfile_data;

    penumbra3_regfile u_regfile (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_rd_idx_a(rf_rd_idx_a), .o_rd_data_a(rf_rd_data_a),
        .i_rd_idx_b(rf_rd_idx_b), .o_rd_data_b(rf_rd_data_b),
        .i_wr_idx(rf_wr_idx), .i_wr_data(rf_wr_data), .i_wr_en(rf_wr_en)
    );

    // ════════════════════════════════════════════════════════════
    // Scoreboard clear (the back end releases a tracked producer)
    // ════════════════════════════════════════════════════════════
    // A tracked producer (load / divmul / rdsys) clears its bit when its value
    // lands: a missed load on completion (o_complete), a divmul aux on its WB
    // writeback cycle, every other producer at its WB commit. One clear port;
    // the three sources do not contend (the freeze geometry keeps o_complete off
    // any WB commit, and a divmul holds WB for its two commit cycles alone). The
    // aux WB cycle is the held divmul slot writing a register while not counting
    // a new retire.
    logic                sb_clr_en;
    logic [SB_IDX_W-1:0] sb_clr_idx;
    logic                aux_wb_cycle;
    assign aux_wb_cycle = mem2wb_valid & ~wb_insn_committed & wb_regfile_we;
    always_comb begin
        if (mc_complete & mc_complete_we) begin
            sb_clr_en  = 1'b1;
            sb_clr_idx = mc_complete_dest;
        end else if (aux_wb_cycle) begin
            sb_clr_en  = 1'b1;
            sb_clr_idx = wb_regfile_idx;
        end else begin
            sb_clr_en  = wb_insn_committed & mem2wb_payload.phys_dst_we;
            sb_clr_idx = mem2wb_payload.phys_dst;
        end
    end

    // ════════════════════════════════════════════════════════════
    // ID -- issue (owns the scoreboard)
    // ════════════════════════════════════════════════════════════
    // Forward broadcasts are tied off in this increment (correctness-first:
    // dependents on a tracked producer wait for the registered scoreboard clear,
    // not a same-cycle release). They are driven in the forwarding-release
    // increment.
    penumbra3_id_stage u_id (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bundle(i_bundle), .i_pc(i_pc), .i_next_pc(i_next_pc),
        .i_valid(i_valid),
        .i_if_fault_pending(i_if_fault_pending), .i_if_fault_vec(i_if_fault_vec),
        .i_if_fault_status(i_if_fault_status),
        .o_deq_ready(o_deq_ready),
        .i_supervisor(supervisor),
        .i_stall_in(id_stall_in), .i_bubble(id_bubble),
        .i_fault_flush(younger_flush),
        .i_fetch_busy(i_fetch_busy), .i_pipe_hold(id_pipe_hold),
        .o_local_stall(/* perfctr; surfaced via o_bcause at commit */),
        .o_rd_idx_a(rf_rd_idx_a), .o_rd_idx_b(rf_rd_idx_b),
        .i_rd_data_a(rf_rd_data_a), .i_rd_data_b(rf_rd_data_b),
        .o_spr_rd_sel(id_spr_rd_sel), .i_spr_src_value(spr_operand_value),
        .i_clr_en(sb_clr_en), .i_clr_idx(sb_clr_idx),
        .i_fwd0_en(1'b0), .i_fwd0_idx('0), .i_fwd1_en(1'b0), .i_fwd1_idx('0),
        .o_ex_bundle(idex_bundle),
        .o_ex_op_a(idex_op_a), .o_ex_op_b(idex_op_b), .o_ex_store_data(idex_store_data),
        .o_ex_phys_dst(idex_phys_dst), .o_ex_phys_dst_we(idex_phys_dst_we),
        .o_ex_phys_dst_aux(idex_phys_dst_aux), .o_ex_phys_dst_aux_we(idex_phys_dst_aux_we),
        .o_ex_phys_src_a(idex_phys_src_a), .o_ex_src_a_fwdable(idex_src_a_fwdable),
        .o_ex_phys_src_b(idex_phys_src_b), .o_ex_src_b_fwdable(idex_src_b_fwdable),
        .o_ex_pc(idex_pc), .o_ex_next_pc(idex_next_pc),
        .o_ex_valid(idex_valid), .o_ex_fault_pending(idex_fault_pending),
        .o_ex_fault_vec(idex_fault_vec), .o_ex_fault_status(idex_fault_status),
        .o_ex_bcause(idex_bcause)
    );

    // ════════════════════════════════════════════════════════════
    // EX -- execute (ALU, forwarding, flags, branch, divmul, drain-commit)
    // ════════════════════════════════════════════════════════════
    // Operand forward sources: two-ahead from mem2.o_fwd (results computed
    // before MEM2); three-ahead, loads and sysreg reads included, from the
    // MEM2/WB register.
    // Flag forward sources: the MEM1/MEM2 producer and the MEM2/WB producer.
    penumbra3_ex_stage u_ex (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bundle(idex_bundle),
        .i_op_a(idex_op_a), .i_op_b(idex_op_b), .i_store_data(idex_store_data),
        .i_phys_dst(idex_phys_dst), .i_phys_dst_we(idex_phys_dst_we),
        .i_phys_dst_aux(idex_phys_dst_aux), .i_phys_dst_aux_we(idex_phys_dst_aux_we),
        .i_pc(idex_pc), .i_next_pc(idex_next_pc), .i_valid(idex_valid),
        .i_fault_pending(idex_fault_pending), .i_fault_vec(idex_fault_vec),
        .i_fault_status(idex_fault_status), .i_bcause(idex_bcause),
        .i_phys_src_a(idex_phys_src_a), .i_src_a_fwdable(idex_src_a_fwdable),
        .i_phys_src_b(idex_phys_src_b), .i_src_b_fwdable(idex_src_b_fwdable),
        .i_mem2_result(mem2_fwd_result), .i_mem2_phys_dst(mem2_fwd_dst),
        .i_mem2_dst_we(mem2_fwd_we), .i_mem2_valid(mem2_fwd_valid),
        .i_wb_result(mem2wb_payload.value), .i_wb_phys_dst(mem2wb_payload.phys_dst),
        .i_wb_dst_we(mem2wb_payload.phys_dst_we), .i_wb_valid(mem2wb_valid),
        .i_sr_flags(sr_flags),
        .i_mem2_flags(mem1mem2_payload.flags),
        .i_mem2_writes_flags(mem1mem2_valid & mem1mem2_bundle.flags_updater
                             & ~mem1mem2_payload.fault_pending),
        .i_wb_flags(mem2wb_payload.flags),
        .i_wb_writes_flags(mem2wb_valid & mem2wb_bundle.flags_updater
                           & ~mem2wb_payload.fault_pending),
        .i_sr_committed(sr_committed),
        .i_irq_inject(i_irq_inject), .i_irq_vec(i_irq_vec),
        .i_deferred_inject(i_deferred_inject), .i_deferred_addr(i_deferred_addr),
        .i_stall_in(ex_stall_in), .i_back_end_active(back_end_active),
        .i_bubble(younger_flush),
        .o_local_stall(ex_local_stall), .o_dc_commit(ex_dc_commit), .o_funit_stall(),
        .o_branch_taken(ex_branch_taken), .o_branch_target(o_branch_target),
        .o_mem1_bundle(exmem1_bundle), .o_mem1_payload(exmem1_payload),
        .o_mem1_store_data(exmem1_store_data), .o_mem1_valid(exmem1_valid),
        .o_mem1_bcause(exmem1_bcause)
    );
    assign o_branch_taken = ex_branch_taken;

    // ════════════════════════════════════════════════════════════
    // MEM1 -- memory access launch
    // ════════════════════════════════════════════════════════════
    penumbra3_mem1_stage u_mem1 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bundle(exmem1_bundle), .i_payload(exmem1_payload),
        .i_store_data(exmem1_store_data), .i_valid(exmem1_valid),
        .i_bcause(exmem1_bcause),
        .i_hold(launch_hold), .i_bubble(younger_flush),
        .o_translate_en(o_translate_en), .o_translate_vaddr(o_translate_vaddr),
        .o_translate_acc_type(o_translate_acc_type),
        .o_dcache_en(o_dcache_en), .o_dcache_vaddr(o_dcache_vaddr),
        .o_sys_re(o_sys_re), .o_sys_dev(o_sys_dev), .o_sys_reg(o_sys_reg),
        .o_mem2_bundle(mem1mem2_bundle), .o_mem2_payload(mem1mem2_payload),
        .o_mem2_store_wdata(mem1mem2_store_wdata), .o_mem2_byte_en(mem1mem2_byte_en),
        .o_mem2_align_fault(mem1mem2_align_fault), .o_mem2_valid(mem1mem2_valid),
        .o_mem2_bcause(mem1mem2_bcause)
    );
    assign o_mem_launch_hold = launch_hold;

    // ════════════════════════════════════════════════════════════
    // MEM2 -- memory access resolve + back-end completion
    // ════════════════════════════════════════════════════════════
    penumbra3_mem2_stage u_mem2 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bundle(mem1mem2_bundle), .i_payload(mem1mem2_payload),
        .i_store_wdata(mem1mem2_store_wdata), .i_byte_en(mem1mem2_byte_en),
        .i_align_fault(mem1mem2_align_fault), .i_valid(mem1mem2_valid),
        .i_bcause(mem1mem2_bcause), .i_user_mode(~supervisor),
        .i_translate_paddr(i_translate_paddr), .i_translate_cacheable(i_translate_cacheable),
        .i_translate_hit(i_translate_hit), .i_translate_miss_fault(i_translate_miss_fault),
        .i_translate_prot_fault(i_translate_prot_fault),
        .i_dcache_hit(i_dcache_hit), .i_dcache_rdata(i_dcache_rdata),
        .o_dcache_we(o_dcache_we),
        .o_dcache_wdata(o_dcache_wdata), .o_dcache_byte_en(o_dcache_byte_en),
        .o_dcache_count_read(o_dcache_count_read),
        .o_dcache_count_write(o_dcache_count_write),
        .o_dcache_access_req(o_dcache_access_req),
        .o_dcache_access_we(o_dcache_access_we),
        .o_dcache_access_addr(o_dcache_access_addr),
        .o_dcache_access_cacheable(o_dcache_access_cacheable),
        .i_dcache_busy(i_dcache_busy), .i_dcache_access_done(i_dcache_access_done),
        .i_dcache_access_word(i_dcache_access_word),
        .i_dcache_access_fault(i_dcache_access_fault),
        .i_sys_rdata(i_sys_rdata),
        .o_mem_pending(mem_pending),
        .o_complete(mc_complete), .o_complete_we(mc_complete_we),
        .o_complete_retires(mc_complete_retires),
        .o_complete_dest(mc_complete_dest), .o_complete_value(mc_complete_value),
        .o_complete_fault(mc_complete_fault), .o_complete_pc(mc_complete_pc),
        .o_complete_vaddr(mc_complete_vaddr), .o_complete_status(mc_complete_status),
        .o_fwd_result(mem2_fwd_result), .o_fwd_dst(mem2_fwd_dst),
        .o_fwd_we(mem2_fwd_we), .o_fwd_valid(mem2_fwd_valid),
        .i_launch_hold(launch_hold),
        .i_hold(mem2_reg_hold), .i_bubble(younger_flush),
        .o_wb_bundle(mem2wb_bundle), .o_wb_payload(mem2wb_payload),
        .o_wb_valid(mem2wb_valid), .o_wb_bcause(mem2wb_bcause)
    );

    // ════════════════════════════════════════════════════════════
    // WB -- writeback / commit
    // ════════════════════════════════════════════════════════════
    penumbra3_wb_stage u_wb (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bundle(mem2wb_bundle), .i_payload(mem2wb_payload),
        .i_valid(mem2wb_valid), .i_bcause(mem2wb_bcause),
        .o_local_stall(wb_local_stall),
        .o_insn_committed(wb_insn_committed), .o_bcause(o_bcause),
        .o_regfile_idx(wb_regfile_idx), .o_regfile_data(wb_regfile_data),
        .o_regfile_we(wb_regfile_we),
        .o_spr_we(wb_spr_we), .o_spr_sel(wb_spr_sel), .o_spr_value(wb_spr_value),
        .o_flag_we(wb_flag_we), .o_flag_value(wb_flag_value),
        .o_fault_commit(wb_fault_commit), .o_fault_vec(wb_fault_vec),
        .o_fault_pc(wb_fault_pc), .o_fault_vaddr(wb_fault_vaddr),
        .o_fault_status(wb_fault_status)
    );

    // ════════════════════════════════════════════════════════════
    // Exposed commit / fault / observability
    // ════════════════════════════════════════════════════════════
    assign o_fault_commit = fault_commit;
    assign o_fault_vec    = fault_vec;
    assign o_eret_commit  = eret_commit;
    assign o_ei_commit    = ei_commit;
    assign o_dc_commit    = ex_dc_commit;

    // MMU fault-register commit: the fault commit qualified to the faults that
    // carry a data address (status type != FAULT_NONE), so the latch captures
    // the fault that actually retires.
    assign o_mmu_fault_commit = fault_commit & (fault_status[3:0] != FAULT_NONE);
    assign o_mmu_fault_vaddr  = fault_vaddr;
    assign o_mmu_fault_status = fault_status;

    // An instruction retires at WB, or with its parked access's completion. A
    // pending access drains WB to bubbles, so the two never share a cycle.
    assign o_insn_committed = wb_insn_committed | mc_complete_retires;
    assign o_retire_valid   = mem2wb_valid | mc_complete_retires;
    assign o_retire_pc      = mc_complete_retires ? mc_complete_pc : mem2wb_payload.pc;
    assign o_retire_sr      = sr_committed;
    assign o_commit_idx     = rf_wr_idx;
    assign o_commit_data    = rf_wr_data;
    assign o_commit_we      = rf_wr_en;

    // A completion that retires is a load's or an uncached store's.
    always_comb begin
        if (mc_complete_retires) o_retire_op_class = mc_complete_we ? OPC_LOAD : OPC_STORE;
        else                     o_retire_op_class = mem2wb_bundle.op_class;
    end

    assign o_dc_commit_pc       = idex_pc;
    assign o_dc_commit_op_class = idex_bundle.op_class;
    assign o_branch_pc          = idex_pc;
    assign o_ex_pc              = idex_pc;
    assign o_mem_pc             = mem1mem2_payload.pc;
    assign o_mem_valid          = mem1mem2_valid;

    // ════════════════════════════════════════════════════════════
    // Assertions -- sim-only (Verilator --assert); stripped at synth.
    // ════════════════════════════════════════════════════════════
    // WB and a completion share the regfile write port, the retire report and
    // the fault commit, and a completion's fault has to be the oldest in
    // flight: the freeze drains WB to bubbles while an access is pending.
    always_comb
        assert (!(mc_complete && mem2wb_valid))
            else $error("penumbra3_spine: an access completed with a live slot in WB");

    /* verilator lint_on PINCONNECTEMPTY */
endmodule
