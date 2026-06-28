// penumbra3_spine -- Penumbra/3 ID->EX->MEM1->MEM2->WB datapath integration.
//
// The first time the gen3 stages run as a pipeline rather than in isolation. It
// instantiates the five datapath stages around the shared register / SPR /
// scratch files, and closes the loops between them:
//   - the inter-stage handoffs (each stage holds its own output register, so
//     this is producer.o_* -> consumer.i_* wiring, carrying the packed
//     ctrl_bundle_t + dpath_payload_t),
//   - the operand- and flag-forward *sources* into EX (the network lives in EX;
//     the spine feeds it the MEM2-resolved and MEM2/WB-register producers),
//   - the scoreboard clear (the back end tells ID when a tracked producer's
//     value has landed),
//   - the back-pressure / freeze distribution -- the timing-critical part: the
//     registered load_pending freezes the launch side and issue, the MEM2/WB
//     register is held only by WB back-pressure, and a fault/branch redirect
//     flushes the younger slots (see doc/internals/penumbra3/load-completion.md
//     "What the pipe gate freezes").
//
// The MMU/TLB and the caches live outside (the machine layer binds them to the
// exposed launch/verdict ports), the same boundary gen2 uses -- the TLB is
// shared coherently with the I-side copy, so it is not a spine-private module.
//
// Deferred-loud: a faulting load completion (o_complete_fault, e.g. a bus fault
// on a fill or an erroring device probe) needs load_complete to park the
// faulting PC + descriptor before the spine can take it precisely. Until that
// extension lands, an assertion catches it rather than mis-taking it silently;
// it is not exercised by the skeleton (no fill fault) and is needed before boot
// device-probing.

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

    // ── D-cache (MEM1 index launch / MEM2 resolve + store write; external) ──
    output logic                  o_dcache_en,
    output logic [31:0]           o_dcache_vaddr,
    input  logic                  i_dcache_hit,
    input  logic [31:0]           i_dcache_rdata,
    output logic [31:0]           o_dcache_paddr,
    output logic                  o_dcache_we,
    output logic [31:0]           o_dcache_wdata,
    output logic [3:0]            o_dcache_byte_en,

    // ── Sysreg read (MEM1 launch) + write (WRSYS drain-commit) ──
    output logic                  o_sys_re,
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    input  logic [31:0]           i_sys_rdata,
    output logic                  o_sys_we,
    output logic [3:0]            o_sys_wr_dev,
    output logic [3:0]            o_sys_wr_reg,
    output logic [31:0]           o_sys_wdata,

    // ── Bus-master line fill (load_complete <-> bus master; external) ──
    output logic                  o_launch_fill,
    output logic [31:0]           o_fill_paddr,
    output logic                  o_fill_cacheable,
    input  logic                  i_fill_done,
    input  logic [31:0]           i_fill_data,
    input  logic                  i_fill_fault,

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
    output logic                  o_sr_s,
    output logic                  o_sr_i,
    output logic                  o_ei_commit,
    output logic                  o_dc_commit,
    input  logic                  i_irq_inject,
    input  logic [3:0]            i_irq_vec,

    // ── Observability (perfctr / trace) ──
    output logic                  o_insn_committed,
    output bcause_e               o_bcause,
    output logic                  o_retire_valid,
    output logic [31:0]           o_retire_pc,
    output logic [SB_IDX_W-1:0]   o_commit_idx,       // regfile write port (commit / load completion)
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we
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
    logic                lc_complete, lc_complete_fault;
    logic [SB_IDX_W-1:0] lc_complete_dest;
    logic [31:0]         lc_complete_value;
    logic                load_pending;

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

    // EX side-effects
    logic                ex_local_stall, ex_dc_commit, ex_branch_taken;

    // ════════════════════════════════════════════════════════════
    // Back-pressure / freeze distribution
    // ════════════════════════════════════════════════════════════
    // The local stalls each stage exposes (downstream-independent), plus the
    // registered back-end hold. These three are the only inputs the freeze
    // composition needs; everything below is derived from them.
    //   - load_pending  : registered flop from MEM2's load_complete (NOT a live
    //                     cache/TLB verdict -- that is the whole gen2 fix).
    //   - wb_local_stall: WB's dual-write hold (flop-shallow).
    //   - ex_local_stall: EX's divmul / drain-commit hold (flop-shallow).

    logic launch_hold;    // freeze MEM1 + the MEM1/MEM2 register + dtranslate + cache
    logic mem2_reg_hold;  // freeze the MEM2/WB register
    logic ex_stall_in;    // MEM1 cannot accept (-> EX)
    logic id_stall_in;    // EX cannot accept (-> ID register hold)
    logic id_pipe_hold;   // registered back-end hold (-> ID issue gate)

    // TODO(human): the freeze distribution.
    //
    // Drive the five signals above from {load_pending, wb_local_stall,
    // ex_local_stall}, per the load-completion freeze contract and the stall
    // audit:
    //   - launch_hold: the launch side (MEM1 register, dtranslate, cache) holds
    //     when a load is pending OR the MEM2/WB register is back-pressured.
    //   - mem2_reg_hold: the MEM2/WB register holds ONLY on WB back-pressure --
    //     it must NOT include load_pending (it drains older work to commit while
    //     a load waits; folding load_pending in re-presents a committed
    //     multi-cycle writeback). This is the gen3 carve-out.
    //   - ex_stall_in: MEM1 cannot accept a new slot this cycle (its register is
    //     frozen).
    //   - id_stall_in: EX cannot accept -- EX is itself stalling OR is
    //     back-pressured. (This must include load_pending so a load-frozen EX
    //     holds the ID/EX register instead of bubbling and losing the slot.)
    //   - id_pipe_hold: the registered back-end hold the issue gate reads.
    always_comb begin
        launch_hold   = load_pending | wb_local_stall;
        mem2_reg_hold = wb_local_stall;
        ex_stall_in   = load_pending | wb_local_stall;
        id_stall_in   = ex_local_stall | load_pending | wb_local_stall;
        id_pipe_hold  = load_pending;
    end

    // Inclusive EX stall: used to gate the branch bubble so a held branch (e.g.
    // back-pressured behind a load) is not discarded before it resolves.
    logic ex_stall;
    assign ex_stall = ex_local_stall | ex_stall_in;

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

    // Per-stage flush (i_bubble). A fault commit at WB is the oldest, so it
    // flushes every younger slot (ID/EX/MEM1/MEM2). A taken branch flushes only
    // its wrong-path successor in ID -- and only once it advances out of EX
    // (~ex_stall keeps a held branch from being discarded). ERET / WRSYS-resync
    // fire with the pipe drained, so they only flush ID.
    logic id_bubble, younger_flush;
    assign younger_flush = wb_fault_commit;
    assign id_bubble = (ex_branch_taken & ~ex_stall) | younger_flush
                     | eret_commit | wrsys_resync_q;

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
    // synthetic fault), so wb_fault_commit covers both and wb_fault_pc is the
    // committing slot's own PC either way.
    logic        save_state;
    logic [31:0] save_pc;
    assign save_state = wb_fault_commit;
    assign save_pc    = wb_fault_pc;

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
    // One write port, muxed between the WB commit and a completed (missed) load.
    // They are temporally exclusive: while a load is pending the MEM2/WB
    // register drains to bubbles, so no WB commit coincides with o_complete.
    logic [SB_IDX_W-1:0] rf_rd_idx_a, rf_rd_idx_b, rf_wr_idx;
    logic [31:0]         rf_rd_data_a, rf_rd_data_b, rf_wr_data;
    logic                rf_wr_en;
    assign rf_wr_en   = lc_complete ? ~lc_complete_fault : wb_regfile_we;
    assign rf_wr_idx  = lc_complete ? lc_complete_dest   : wb_regfile_idx;
    assign rf_wr_data = lc_complete ? lc_complete_value  : wb_regfile_data;

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
        if (lc_complete) begin
            sb_clr_en  = 1'b1;
            sb_clr_idx = lc_complete_dest;
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
    // Operand forward sources: MEM2-resolved (two-ahead / load-use) from
    // mem2.o_fwd; the MEM2/WB register (three-ahead) from the mem2wb wires.
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
        .i_stall_in(ex_stall_in), .i_wb_active(mem2wb_valid), .i_bubble(younger_flush),
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
        .o_dcache_paddr(o_dcache_paddr), .o_dcache_we(o_dcache_we),
        .o_dcache_wdata(o_dcache_wdata), .o_dcache_byte_en(o_dcache_byte_en),
        .i_sys_rdata(i_sys_rdata),
        .o_launch_fill(o_launch_fill), .o_fill_paddr(o_fill_paddr),
        .o_fill_cacheable(o_fill_cacheable),
        .i_fill_done(i_fill_done), .i_fill_data(i_fill_data), .i_fill_fault(i_fill_fault),
        .o_load_pending(load_pending),
        .o_complete(lc_complete), .o_complete_dest(lc_complete_dest),
        .o_complete_value(lc_complete_value), .o_complete_fault(lc_complete_fault),
        .o_fwd_result(mem2_fwd_result), .o_fwd_dst(mem2_fwd_dst),
        .o_fwd_we(mem2_fwd_we), .o_fwd_valid(mem2_fwd_valid),
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
    assign o_fault_commit = wb_fault_commit;
    assign o_fault_vec    = wb_fault_vec;
    assign o_eret_commit  = eret_commit;
    assign o_ei_commit    = ei_commit;
    assign o_dc_commit    = ex_dc_commit;

    // MMU fault-register commit: the WB fault-commit qualified to the faults that
    // carry a data address (status type != FAULT_NONE). The payload rides the
    // MEM2/WB register, so the latch captures the fault that actually retires.
    assign o_mmu_fault_commit = wb_fault_commit & (wb_fault_status[3:0] != FAULT_NONE);
    assign o_mmu_fault_vaddr  = wb_fault_vaddr;
    assign o_mmu_fault_status = wb_fault_status;

    assign o_insn_committed = wb_insn_committed;
    assign o_retire_valid   = mem2wb_valid;
    assign o_retire_pc      = mem2wb_payload.pc;
    assign o_commit_idx     = rf_wr_idx;
    assign o_commit_data    = rf_wr_data;
    assign o_commit_we      = rf_wr_en;

    // ════════════════════════════════════════════════════════════
    // Assertions -- sim-only (Verilator --assert); stripped at synth.
    // ════════════════════════════════════════════════════════════
    // A faulting load completion needs load_complete to park the faulting PC +
    // descriptor before it can be taken precisely; that extension is not in yet,
    // so catch it loudly rather than mis-take it. Not exercised by the skeleton.
    always_comb
        assert (!lc_complete_fault)
            else $error("penumbra3_spine: faulting load completion not yet handled (needs load_complete PC park)");

    // The regfile write port is shared: a WB commit and a load completion must
    // never drive it the same cycle (the freeze drains WB to bubbles while a
    // load is pending).
    always_comb
        assert (!(lc_complete && wb_regfile_we))
            else $error("penumbra3_spine: WB commit collided with a load completion on the write port");

    /* verilator lint_on PINCONNECTEMPTY */
endmodule
