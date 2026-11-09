// penumbra3_core -- Penumbra/3 CPU core: fetch + datapath spine.
//
// The bare pipelined core: the front end (penumbra3_if1_stage,
// penumbra3_if2_stage) through the elastic fetch buffer onto penumbra3_spine
// (ID->EX->MEM1->MEM2->WB + regfile + scoreboard + SPR/scratch files), plus
// penumbra3_fetch_replay (a miss's replay) and the two exception-path units
// -- penumbra3_vecfetch (vector-table read + handler redirect) and
// penumbra3_irq (recognition + EX-frontier injection, and the deferred bus
// fault). Everything memory-shaped lives outside, behind the port groups the
// machine integration (machine_penumbra3) binds to the duplicated-BRAM
// MMU/TLB, the split L1s, the uncore and the sysreg device complex:
//
//   - The fetch side: the launch (o_fetch_addr/o_fetch_en, the lookup the L1-I
//     and the MMU I-port sample together), their verdicts a cycle later, and
//     the L1-I's access (doc/internals/penumbra3/l1-cache.md). IF1 launches
//     and IF2 resolves; a miss is the L1-I's access, and
//     penumbra3_fetch_replay replays the fetch. The vector-fetch FSM reads
//     the vector table as an access with no lookup.
//   - The D-side memory ports the spine drives directly: the translation
//     launch/verdict (MEM1 launch, MEM2 verdict), the L1-D's launch, resolve,
//     store strobe and access (doc/internals/penumbra3/l1-cache.md), and the
//     WB-qualified MMU fault-register commit. The L1-D's store fault goes to
//     penumbra3_irq, which takes it as a deferred bus fault.
//   - The sysreg sideband: the RDSYS read selectors + launch strobe with the
//     response due at data-ready, and the WRSYS write port committed from EX.
//
// No halt: a real CPU never stops on an instruction. BREAK is a trap, taken at
// EX and vectored to VEC_BREAK, not a halt -- so the core has no o_halted. It
// exposes retire/commit observability and the exception/branch pulses; a
// consumer decides program end by watching them.

module penumbra3_core
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
#(
    parameter  logic [31:0] RESET_PC = 32'hFFFF_0000,  // the hw-test --org convention
    parameter  int          IL1_WAYS = 2               // the L1-I's ways
)(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── Interrupt lines (level, wired-OR external + timer) ───────
    input  logic                  i_irq,
    input  logic                  i_timer_irq,

    // ── Fetch launch (the L1-I and the MMU I-port sample this together) ──
    output logic [31:0]           o_fetch_addr,
    output logic                  o_fetch_en,      // low holds both lookups
    output logic                  o_fetch_user,    // fetch privilege (query + status info)

    // ── MMU I-port verdict ───────────────────────────────────────
    input  logic [31:0]           i_fetch_paddr,
    input  logic                  i_fetch_cacheable,
    input  logic                  i_fetch_mmu_fault,
    input  logic [31:0]           i_fetch_mmu_fault_status,

    // ── L1-I (verdict, counting, discard, access) ────────────────
    // The L1 launches on o_fetch_en. A miss and the vector read are its
    // accesses, one at a time.
    input  logic [IL1_WAYS-1:0]       i_icache_way_hit,
    input  logic [IL1_WAYS-1:0][31:0] i_icache_way_word,
    input  logic                      i_icache_hit,
    output logic                      o_icache_count_read,
    output logic                      o_icache_discard,     // IF2's slot is flushed
    output logic                      o_icache_access_req,
    output logic [31:0]               o_icache_access_addr, // physical
    output logic                      o_icache_access_cacheable,
    input  logic                      i_icache_busy,
    input  logic                      i_icache_access_done,
    input  logic [31:0]               i_icache_access_word,
    input  logic                      i_icache_access_fault,

    // ── D-side translation (MEM1 launch / MEM2 verdict) ──────────
    output logic                  o_translate_en,
    output logic [31:0]           o_translate_vaddr,
    output logic [2:0]            o_translate_acc_type,
    output logic                  o_mem_launch_hold,  // freeze the dtranslate + cache launch side
    output logic                  o_user_mode,        // D-side translate/access privilege
    input  logic [31:0]           i_translate_paddr,
    input  logic                  i_translate_cacheable,
    input  logic                  i_translate_hit,
    input  logic                  i_translate_miss_fault,
    input  logic                  i_translate_prot_fault,

    // ── L1-D (MEM1 launch / MEM2 resolve, store strobe, counting, access) ──
    // The L1 launches on o_dcache_en outside o_mem_launch_hold. A store's
    // data and lanes serve its hit write and its access both.
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
    input  logic                  i_dcache_store_fault,      // an earlier write faulted
    input  logic [31:0]           i_dcache_store_fault_addr, // its physical address

    // ── Sysreg sideband (RDSYS read; WRSYS write committed from EX) ─
    output logic                  o_sys_re,
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    input  logic [31:0]           i_sys_rdata,
    output logic                  o_sys_we,
    output logic [3:0]            o_sys_wr_dev,
    output logic [3:0]            o_sys_wr_reg,
    output logic [31:0]           o_sys_wdata,
    // A device finishing a write's side effect, registered: fetch holds
    // until it falls, so the instruction after the WRSYS sees the effect.
    input  logic                  i_sys_busy,

    // ── MMU fault-register commit (WB's qualified strobe + payload) ──
    output logic                  o_mmu_fault_commit,
    output logic [31:0]           o_mmu_fault_vaddr,
    output logic [31:0]           o_mmu_fault_status,

    // ── Commit observability (WB regfile write port) ─────────────
    output logic [SB_IDX_W-1:0]   o_commit_idx,
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we,

    // ── Retire observability ─────────────────────────────────────
    output logic                  o_insn_committed,  // distinct-instruction WB commit
    output logic                  o_insn_retired,    // retire pulse incl. drain-commit (perfctr)
    output logic                  o_retire_valid,    // the slot leaving WB
    output logic [31:0]           o_retire_pc,
    output op_class_e             o_retire_op_class,
    output logic [31:0]           o_retire_sr,       // the committed SR as it retires
    output bcause_e               o_bcause,          // stall-attribution for a non-retiring cycle

    // ── Exception / branch observability (trace markers) ─────────
    output logic                  o_fault_commit,
    output logic [3:0]            o_fault_vec,
    output logic                  o_eret_commit,
    output logic                  o_dc_commit,
    output logic [31:0]           o_dc_commit_pc,
    output op_class_e             o_dc_commit_op_class,
    output logic                  o_branch_taken,
    output logic [31:0]           o_branch_target,
    output logic [31:0]           o_branch_pc,

    // ── Pipeline occupancy (trace): the slots in EX and MEM2 ─────
    output logic [31:0]           o_ex_pc,
    output logic                  o_ex_valid,
    output logic [31:0]           o_mem_pc,
    output logic                  o_mem_valid
);

    // ── Front-end wires ──────────────────────────────────────────
    logic [31:0] if1_pc, if1_next_pc;
    logic        if1_valid;

    logic         if2_miss;
    logic         if2_enq_fire;
    logic         if2_enq_valid;
    ctrl_bundle_t if2_enq_bundle;
    logic [31:0]  if2_enq_pc, if2_enq_next_pc, if2_enq_fault_status;
    logic [3:0]   if2_enq_fault_vec;
    logic         if2_enq_fault_pending;
    logic         if2_hold;      // IF2 -> IF1 back-pressure

    // ── Fetch replay wires ───────────────────────────────────────
    logic        frep_replay, frep_replayed, frep_deliver;
    logic [31:0] frep_replay_pc;

    // ── Fetch buffer handshake ───────────────────────────────────
    logic              fbuf_enq_ready, fbuf_deq_valid;
    localparam int     FBUF_W = $bits(ctrl_bundle_t) + 32 + 32 + 1 + 4 + 32;
    logic [FBUF_W-1:0] fbuf_deq_data;

    // ── Spine head (fetch-buffer dequeue, unpacked) ──────────────
    ctrl_bundle_t id_bundle;
    logic [31:0]  id_pc, id_next_pc, id_fault_status;
    logic [3:0]   id_fault_vec;
    logic         id_fault_pending;
    assign {id_bundle, id_pc, id_next_pc, id_fault_status, id_fault_vec, id_fault_pending}
             = fbuf_deq_data;
    logic         id_deq_ready;  // spine consumes the head (-> fetch buffer i_deq_ready)

    // ── Branch / exception / interrupt wires ─────────────────────
    logic        branch_taken;
    logic [31:0] branch_target;
    logic        fault_commit;
    logic [3:0]  fault_vec;
    logic [31:0] epc;
    logic        eret_commit;
    logic        wrsys_resync;
    logic [31:0] wrsys_resync_pc;

    logic        sr_s, sr_i, ei_commit, dc_commit, ex_valid, ex_stall;
    logic        insn_committed, retire_valid;
    logic        irq_inject;
    logic [3:0]  irq_vec;
    logic        deferred_inject;
    logic [31:0] deferred_addr;

    logic        mmu_fault_commit;
    logic [31:0] mmu_fault_vaddr, mmu_fault_status;

    // ── Vector-fetch FSM wires ───────────────────────────────────
    logic        vecf_active;
    logic        vecf_req;
    logic [31:0] vecf_req_addr;
    logic        vecf_redirect;
    logic [31:0] vecf_redirect_pc;

    // The committed SR.S (from the spine's SPR file) is the privilege source:
    // privilege changes only via drain-commit ops (exception entry / ERET),
    // which flush younger instructions, so every in-flight instruction shares
    // this one committed value. The D-side privilege rides the spine's
    // o_user_mode directly; the fetch side is its complement here.
    logic core_supervisor, fetch_user;
    assign core_supervisor = sr_s;
    assign fetch_user      = ~core_supervisor;
    assign o_fetch_user    = fetch_user;

    // ══════════════════════════════════════════════════════════
    // Front-end redirect / flush composition
    // ══════════════════════════════════════════════════════════
    // Four control-flow events know their redirect target and steer PC: a taken
    // branch, the vector fetch reaching its handler, an ERET returning to EPC,
    // and a WRSYS context-sync re-fetch. They are mutually exclusive in time --
    // each one's flush drains the others' shadow -- so only one is ever active.
    //   branch:       branch_taken  -> branch_target
    //   vector fetch: vecf_redirect -> vecf_redirect_pc
    //   ERET:         eret_commit   -> epc
    //   WRSYS resync: wrsys_resync  -> wrsys_resync_pc
    // A fault flushes the wrong-path slot now but does NOT steer here -- its
    // redirect comes later, via the vector fetch (IF1's fault flush is i_flush
    // = fault_commit, wired below).
    logic        if_redirect;       // one of the four fired
    logic [31:0] if1_redirect_pc;   // the target IF1 steers to

    // A fifth steers IF1 without being a flush: a miss's replay discards only
    // what fetch launched behind the miss, and what the fetch buffer holds is
    // older than it. Any of the four is older than the fetch that missed, so
    // the replay takes the last leg, which doubles as the (don't-care) idle
    // target.
    assign if_redirect = branch_taken | vecf_redirect | eret_commit | wrsys_resync;
    always_comb begin
        if      (branch_taken)  if1_redirect_pc = branch_target;
        else if (vecf_redirect) if1_redirect_pc = vecf_redirect_pc;
        else if (eret_commit)   if1_redirect_pc = epc;
        else if (wrsys_resync)  if1_redirect_pc = wrsys_resync_pc;
        else                    if1_redirect_pc = frep_replay_pc;
    end

    // Any front-end kill bubbles IF2 + the fetch buffer: the four steering
    // events plus a fault, which flushes the wrong-path word the cycle it
    // commits.
    logic if_flush;
    assign if_flush = if_redirect | fault_commit;

    // IF2 also loses the slot behind a miss to the miss's replay.
    logic if2_flush;
    assign if2_flush = if_flush | frep_replay;

    // ══════════════════════════════════════════════════════════
    // IF1 -- PC + fetch address generation
    // ══════════════════════════════════════════════════════════
    penumbra3_if1_stage #(.RESET_PC(RESET_PC)) u_if1 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_stall_in(if2_hold | i_icache_busy | vecf_active | i_sys_busy),
        .i_redirect(if_redirect | frep_replay), .i_redirect_pc(if1_redirect_pc),
        .i_flush(fault_commit),                 // bubble the wrong-path fetch at the fault
        .i_fetch_stop(1'b0),                    // unused: the interrupt drains at the EX frontier
        .o_fetch_addr(o_fetch_addr), .o_fetch_en(o_fetch_en),
        .o_pc(if1_pc), .o_next_pc(if1_next_pc), .o_valid(if1_valid)
    );

    // ══════════════════════════════════════════════════════════
    // IF2 -- deliver fetched word (pre-decoded bundle) to the FIFO
    // ══════════════════════════════════════════════════════════
    penumbra3_if2_stage #(
        .WAYS(IL1_WAYS)
    ) u_if2 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_pc(if1_pc), .i_next_pc(if1_next_pc), .i_valid(if1_valid),
        // The L1-I's and the MMU's registered, held results for the fetch IF1
        // launched (paired by the shared launch strobe).
        .i_way_hit(i_icache_way_hit), .i_way_word(i_icache_way_word),
        .i_hit(i_icache_hit),
        .i_cacheable(i_fetch_cacheable),
        .i_user_mode(fetch_user),
        .i_mmu_fault(i_fetch_mmu_fault),
        .i_mmu_fault_status(i_fetch_mmu_fault_status),
        .i_replayed(frep_replayed),
        .i_deliver(frep_deliver), .i_deliver_word(i_icache_access_word),
        .i_deliver_fault(i_icache_access_fault),
        .o_miss(if2_miss),
        .i_flush(if2_flush),
        .i_enq_ready(fbuf_enq_ready),
        .o_enq_valid(if2_enq_valid),
        .o_enq_fire(if2_enq_fire),
        .o_enq_bundle(if2_enq_bundle),
        .o_enq_pc(if2_enq_pc), .o_enq_next_pc(if2_enq_next_pc),
        .o_enq_fault_pending(if2_enq_fault_pending),
        .o_enq_fault_vec(if2_enq_fault_vec),
        .o_enq_fault_status(if2_enq_fault_status),
        .o_count_read(o_icache_count_read),
        .o_hold(if2_hold)
    );

    // ══════════════════════════════════════════════════════════
    // The L1-I's access, and the replay of a miss
    // ══════════════════════════════════════════════════════════
    // A miss is the access of the lookup IF2 resolves; the vector read has no
    // lookup, and comes only while IF1 is held and no access is in flight.
    assign o_icache_access_req       = if2_miss | vecf_req;
    assign o_icache_access_addr      = vecf_req ? vecf_req_addr : i_fetch_paddr;
    assign o_icache_access_cacheable = !vecf_req && i_fetch_cacheable;

    // IF2's flush throws its slot's lookup away.
    assign o_icache_discard = if2_flush;

    penumbra3_fetch_replay u_fetch_replay (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_miss(if2_miss), .i_miss_pc(if1_pc),
        .i_busy(i_icache_busy), .i_access_done(i_icache_access_done),
        .i_flush(if_flush), .i_slot_left(if2_enq_fire),
        .o_replay(frep_replay), .o_replay_pc(frep_replay_pc),
        .o_replayed(frep_replayed), .o_deliver(frep_deliver)
    );

    // ══════════════════════════════════════════════════════════
    // Fetch buffer -- elastic IF2 -> ID decoupling (breaks the stall path)
    // ══════════════════════════════════════════════════════════
    // The back-end stall reaches only the dequeue side; IF2 back-pressures on
    // the buffer's registered o_enq_ready instead, so a back-end stall never
    // reaches o_fetch_en combinationally.
    penumbra3_fetch_buffer #(.PAYLOAD_W(FBUF_W), .DEPTH(2)) u_fbuf (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_flush(if_flush),
        .i_enq_valid(if2_enq_valid),
        .i_enq_data({if2_enq_bundle, if2_enq_pc, if2_enq_next_pc,
                     if2_enq_fault_status, if2_enq_fault_vec, if2_enq_fault_pending}),
        .o_enq_ready(fbuf_enq_ready),
        .o_deq_valid(fbuf_deq_valid),
        .o_deq_data(fbuf_deq_data),
        .i_deq_ready(id_deq_ready)
    );

    // ══════════════════════════════════════════════════════════
    // Datapath spine -- ID -> EX -> MEM1 -> MEM2 -> WB
    // ══════════════════════════════════════════════════════════
    penumbra3_spine u_spine (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bundle(id_bundle), .i_pc(id_pc), .i_next_pc(id_next_pc),
        .i_valid(fbuf_deq_valid),
        .i_if_fault_pending(id_fault_pending), .i_if_fault_vec(id_fault_vec),
        .i_if_fault_status(id_fault_status), .i_fetch_busy(i_icache_busy),
        .o_deq_ready(id_deq_ready),
        // D-side translation -- MEM1 launch, MEM2 verdict.
        .o_translate_en(o_translate_en), .o_translate_vaddr(o_translate_vaddr),
        .o_translate_acc_type(o_translate_acc_type), .o_mem_launch_hold(o_mem_launch_hold),
        .o_user_mode(o_user_mode),
        .i_translate_paddr(i_translate_paddr), .i_translate_cacheable(i_translate_cacheable),
        .i_translate_hit(i_translate_hit), .i_translate_miss_fault(i_translate_miss_fault),
        .i_translate_prot_fault(i_translate_prot_fault),
        // L1-D.
        .o_dcache_en(o_dcache_en), .o_dcache_vaddr(o_dcache_vaddr),
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
        // Sysreg.
        .o_sys_re(o_sys_re), .o_sys_dev(o_sys_dev), .o_sys_reg(o_sys_reg),
        .i_sys_rdata(i_sys_rdata),
        .o_sys_we(o_sys_we), .o_sys_wr_dev(o_sys_wr_dev), .o_sys_wr_reg(o_sys_wr_reg),
        .o_sys_wdata(o_sys_wdata),
        // Branch resolution -> the front end.
        .o_branch_taken(branch_taken), .o_branch_target(branch_target),
        // Exception entry / return / WRSYS resync -> the front end.
        .o_fault_commit(fault_commit), .o_fault_vec(fault_vec), .o_epc(epc),
        .o_eret_commit(eret_commit),
        .o_wrsys_resync(wrsys_resync), .o_wrsys_resync_pc(wrsys_resync_pc),
        // MMU fault-register commit -- WB's qualified strobe + payload.
        .o_mmu_fault_commit(mmu_fault_commit), .o_mmu_fault_vaddr(mmu_fault_vaddr),
        .o_mmu_fault_status(mmu_fault_status),
        // Interrupt support -- observability out, IRQ save-state in.
        .o_sr_s(sr_s), .o_sr_i(sr_i), .o_ei_commit(ei_commit), .o_dc_commit(dc_commit),
        .o_ex_valid(ex_valid), .o_ex_stall(ex_stall),
        .i_irq_inject(irq_inject), .i_irq_vec(irq_vec),
        .i_deferred_inject(deferred_inject), .i_deferred_addr(deferred_addr),
        // Observability.
        .o_insn_committed(insn_committed), .o_bcause(o_bcause),
        .o_retire_valid(retire_valid), .o_retire_pc(o_retire_pc),
        .o_retire_op_class(o_retire_op_class), .o_retire_sr(o_retire_sr),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data), .o_commit_we(o_commit_we),
        .o_dc_commit_pc(o_dc_commit_pc), .o_dc_commit_op_class(o_dc_commit_op_class),
        .o_branch_pc(o_branch_pc),
        .o_ex_pc(o_ex_pc), .o_mem_pc(o_mem_pc), .o_mem_valid(o_mem_valid)
    );

    // Instructions retired: a distinct instruction commit, at WB or with a
    // parked access's completion (insn_committed, once per instruction -- a
    // dual-write's second register cycle is excluded) OR a drain-commit, which
    // commits from EX and never reaches WB. The two are mutually exclusive per
    // cycle, so the OR counts each retire exactly once.
    assign o_insn_retired   = insn_committed | dc_commit;
    assign o_insn_committed = insn_committed;
    assign o_retire_valid   = retire_valid;
    assign o_ex_valid       = ex_valid;

    assign o_mmu_fault_commit = mmu_fault_commit;
    assign o_mmu_fault_vaddr  = mmu_fault_vaddr;
    assign o_mmu_fault_status = mmu_fault_status;

    // Trace observability: the same commit pulses the front end consumes.
    assign o_fault_commit  = fault_commit;
    assign o_fault_vec     = fault_vec;
    assign o_eret_commit   = eret_commit;
    assign o_dc_commit     = dc_commit;
    assign o_branch_taken  = branch_taken;
    assign o_branch_target = branch_target;

    // ══════════════════════════════════════════════════════════
    // Vector-fetch FSM -- reads the handler address, redirects PC
    // ══════════════════════════════════════════════════════════
    // On a fault commit (an interrupt is one such commit -- the EX stage tags
    // its instruction as a synthetic fault) it reads vector_table[vec<<2] --
    // the handler address the kernel stored there -- as the L1-I's access,
    // then redirects IF1 to the handler.
    penumbra3_vecfetch u_vecfetch (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_fault_commit(fault_commit), .i_fault_vec(fault_vec),
        .i_busy(i_icache_busy),
        .i_access_done(i_icache_access_done), .i_access_word(i_icache_access_word),
        .o_active(vecf_active),
        .o_req(vecf_req), .o_req_addr(vecf_req_addr),
        .o_redirect(vecf_redirect), .o_redirect_pc(vecf_redirect_pc)
    );

    // ══════════════════════════════════════════════════════════
    // Interrupt unit -- recognition + EX-frontier injection, and the
    // deferred bus fault
    // ══════════════════════════════════════════════════════════
    // The deferred fault is held until it commits: only it commits with its
    // type, which the MMU's fault registers take.
    logic deferred_commit;
    assign deferred_commit = mmu_fault_commit
                           & (mmu_fault_status[3:0] == FAULT_DEFERRED_BUS);

    penumbra3_irq u_irq (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_irq(i_irq), .i_timer_irq(i_timer_irq),
        .i_sr_i(sr_i), .i_ei_commit(ei_commit),
        .i_retire_valid(retire_valid), .i_dc_commit(dc_commit),
        .i_ex_valid(ex_valid), .i_ex_stall(ex_stall),
        .i_fault_commit(fault_commit), .i_vecf_active(vecf_active),
        .i_store_fault(i_dcache_store_fault),
        .i_store_fault_addr(i_dcache_store_fault_addr),
        .i_deferred_commit(deferred_commit),
        .o_irq_inject(irq_inject), .o_irq_vec(irq_vec),
        .o_deferred_inject(deferred_inject), .o_deferred_addr(deferred_addr)
    );

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // The fetch side's accesses come one at a time.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(if2_miss && vecf_req))
        else $error("penumbra3_core: a miss and the vector read in one cycle");

    // The front end holds no lookup across an access: the replay or a flush
    // discards the slot behind it. So the L1-I's busy ends with its done.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_icache_access_done && i_icache_busy))
        else $error("penumbra3_core: the L1-I stayed busy past its done");

endmodule
