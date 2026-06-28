// penumbra3_core -- Penumbra/3 CPU core: fetch + datapath spine.
//
// The bare pipelined core: the front end (penumbra3_if1_stage,
// penumbra3_if2_stage) through the elastic fetch buffer onto penumbra3_spine
// (ID->EX->MEM1->MEM2->WB + regfile + scoreboard + SPR/scratch files), plus the
// two exception-path units -- penumbra3_vecfetch (vector-table read + handler
// redirect) and penumbra3_irq (recognition + EX-frontier injection). Everything
// memory-shaped lives outside, behind the port groups the machine integration
// (machine_penumbra3) binds to the duplicated-BRAM MMU/TLB, the split L1s, the
// arbiter + registered bus master, the fill sequencer, and the sysreg device
// complex:
//
//   - The fetch front port: launch (o_fetch_addr/o_fetch_en, the registered
//     lookup the I-side L1 and the MMU I-port sample together), the request
//     (o_fetch_re, held level until the completion cycle), and the response
//     (i_fetch_rdata valid when i_fetch_busy is low -- drop-equals-valid). IF1
//     and the vector-fetch FSM share the port through the mux here: while
//     o_fetch_bypass (vecf active) is high the FSM owns it and its reads are
//     physical (MMU bypass, which also makes them uncacheable).
//   - The D-side memory ports the spine drives directly: the translation
//     launch/verdict (MEM1 launch, MEM2 verdict), the D-cache index/resolve +
//     store write, the registered line-fill transaction (load completion <-> bus
//     master), and the WB-qualified MMU fault-register commit.
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
    parameter logic [31:0] RESET_PC = 32'hFFFF_0000   // matches the hw-test --org convention
)(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── Interrupt lines (level, wired-OR external + timer) ───────
    input  logic                  i_irq,
    input  logic                  i_timer_irq,

    // ── Fetch front port (launch / request / response) ───────────
    output logic [31:0]           o_fetch_addr,
    output logic                  o_fetch_en,      // lookup launch (I-L1 + MMU I-port sample this)
    output logic                  o_fetch_re,      // word request, held until completion
    input  logic [31:0]           i_fetch_rdata,   // valid when i_fetch_busy low
    input  logic                  i_fetch_busy,
    input  logic                  i_fetch_fault,   // bus fault on the fetch, at i_fetch_busy drop

    // ── Fetch translation context + MMU I-port verdict ───────────
    output logic                  o_fetch_bypass,  // vector-fetch owns the port: physical read
    output logic                  o_fetch_user,    // fetch privilege (query + status info)
    input  logic                  i_fetch_mmu_fault,
    input  logic [31:0]           i_fetch_mmu_fault_status,

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

    // ── D-cache (MEM1 index launch / MEM2 resolve + store write) ──
    output logic                  o_dcache_en,
    output logic [31:0]           o_dcache_vaddr,
    input  logic                  i_dcache_hit,
    input  logic [31:0]           i_dcache_rdata,
    output logic [31:0]           o_dcache_paddr,
    output logic                  o_dcache_we,
    output logic [31:0]           o_dcache_wdata,
    output logic [3:0]            o_dcache_byte_en,

    // ── Sysreg sideband (RDSYS read; WRSYS write committed from EX) ─
    output logic                  o_sys_re,
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    input  logic [31:0]           i_sys_rdata,
    output logic                  o_sys_we,
    output logic [3:0]            o_sys_wr_dev,
    output logic [3:0]            o_sys_wr_reg,
    output logic [31:0]           o_sys_wdata,

    // ── Bus-master line fill (load completion <-> bus master) ────
    output logic                  o_launch_fill,
    output logic [31:0]           o_fill_paddr,
    output logic                  o_fill_cacheable,
    input  logic                  i_fill_done,
    input  logic [31:0]           i_fill_data,
    input  logic                  i_fill_fault,

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
    output bcause_e               o_bcause,          // stall-attribution for a non-retiring cycle

    // ── Exception / branch observability (trace markers) ─────────
    output logic                  o_fault_commit,
    output logic [3:0]            o_fault_vec,
    output logic                  o_eret_commit,
    output logic                  o_dc_commit,
    output logic                  o_branch_taken,
    output logic [31:0]           o_branch_target
);

    // ── Front-end wires ──────────────────────────────────────────
    logic [31:0] if1_fetch_addr;
    logic        if1_fetch_en;
    logic [31:0] if1_pc, if1_next_pc;
    logic        if1_valid;

    logic         if2_fetch_re;
    logic         if2_enq_valid;
    ctrl_bundle_t if2_enq_bundle;
    logic [31:0]  if2_enq_pc, if2_enq_next_pc, if2_enq_fault_status;
    logic [3:0]   if2_enq_fault_vec;
    logic         if2_enq_fault_pending;
    logic         if2_stall;     // IF2 -> IF1 back-pressure

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
    logic        insn_committed;
    logic        irq_inject;
    logic [3:0]  irq_vec;

    // ── Vector-fetch FSM wires ───────────────────────────────────
    logic        vecf_active;
    logic [31:0] vecf_fetch_addr;
    logic        vecf_fetch_en;
    logic        vecf_fetch_re;
    logic        vecf_redirect;
    logic [31:0] vecf_redirect_pc;

    // The committed SR.S (from the spine's SPR file) is the privilege source:
    // privilege changes only via drain-commit ops (exception entry / ERET),
    // which flush younger instructions, so every in-flight instruction shares
    // this one committed value. The D-side privilege rides the spine's
    // o_user_mode directly; the fetch side is its complement here.
    logic core_supervisor;
    assign core_supervisor = sr_s;
    assign o_fetch_user    = ~core_supervisor;

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
    logic        if1_redirect;      // steer PC + bubble IF1's slot: any event fired
    logic [31:0] if1_redirect_pc;   // the target for that redirect

    // The redirect is "any steering event"; only the target needs the priority
    // select. Since the events are mutually exclusive, the priority is arbitrary
    // and the final leg doubles as the (don't-care) idle target.
    assign if1_redirect = branch_taken | vecf_redirect | eret_commit | wrsys_resync;
    always_comb begin
        if      (branch_taken)  if1_redirect_pc = branch_target;
        else if (vecf_redirect) if1_redirect_pc = vecf_redirect_pc;
        else if (eret_commit)   if1_redirect_pc = epc;
        else                    if1_redirect_pc = wrsys_resync_pc;
    end

    // Any front-end kill bubbles IF2 + the fetch buffer: the four steering
    // events (folded into if1_redirect) plus a fault, which flushes the
    // wrong-path word the cycle it commits.
    logic if_flush;
    assign if_flush = if1_redirect | fault_commit;

    // The fetch port: IF1 drives it normally, the vector-fetch FSM while it owns
    // it (o_fetch_bypass tells the MMU those reads are physical). IF2's request
    // can never overlap the FSM's -- entry flushed IF2's slot first.
    assign o_fetch_addr   = vecf_active ? vecf_fetch_addr : if1_fetch_addr;
    assign o_fetch_en     = vecf_active ? vecf_fetch_en   : if1_fetch_en;
    assign o_fetch_re     = vecf_active ? vecf_fetch_re   : if2_fetch_re;
    assign o_fetch_bypass = vecf_active;

    // ══════════════════════════════════════════════════════════
    // IF1 -- PC + fetch address generation
    // ══════════════════════════════════════════════════════════
    penumbra3_if1_stage #(.RESET_PC(RESET_PC)) u_if1 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_stall_in(if2_stall | vecf_active),   // held while the FSM owns the fetch port
        .i_mem_busy(i_fetch_busy),
        .i_redirect(if1_redirect), .i_redirect_pc(if1_redirect_pc),
        .i_flush(fault_commit),                 // bubble the wrong-path fetch at the fault
        .i_fetch_stop(1'b0),                    // unused: the interrupt drains at the EX frontier
        .o_fetch_addr(if1_fetch_addr), .o_fetch_en(if1_fetch_en),
        .o_pc(if1_pc), .o_next_pc(if1_next_pc), .o_valid(if1_valid)
    );

    // ══════════════════════════════════════════════════════════
    // IF2 -- deliver fetched word (pre-decoded bundle) to the FIFO
    // ══════════════════════════════════════════════════════════
    penumbra3_if2_stage u_if2 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_pc(if1_pc), .i_next_pc(if1_next_pc), .i_valid(if1_valid),
        .i_ir(i_fetch_rdata),
        .i_mem_busy(i_fetch_busy), .i_mem_fault(i_fetch_fault),
        .o_fetch_re(if2_fetch_re),
        // I-side MMU verdict -- the registered, held result for the fetch IF1
        // launched (paired by the shared launch strobe).
        .i_user_mode(o_fetch_user),
        .i_mmu_fault(i_fetch_mmu_fault),
        .i_mmu_fault_status(i_fetch_mmu_fault_status),
        .i_flush(if_flush),
        .i_enq_ready(fbuf_enq_ready),
        .o_enq_valid(if2_enq_valid),
        .o_enq_bundle(if2_enq_bundle),
        .o_enq_pc(if2_enq_pc), .o_enq_next_pc(if2_enq_next_pc),
        .o_enq_fault_pending(if2_enq_fault_pending),
        .o_enq_fault_vec(if2_enq_fault_vec),
        .o_enq_fault_status(if2_enq_fault_status),
        .o_stall(if2_stall)
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
        .i_if_fault_status(id_fault_status), .i_fetch_busy(i_fetch_busy),
        .o_deq_ready(id_deq_ready),
        // D-side translation -- MEM1 launch, MEM2 verdict.
        .o_translate_en(o_translate_en), .o_translate_vaddr(o_translate_vaddr),
        .o_translate_acc_type(o_translate_acc_type), .o_mem_launch_hold(o_mem_launch_hold),
        .o_user_mode(o_user_mode),
        .i_translate_paddr(i_translate_paddr), .i_translate_cacheable(i_translate_cacheable),
        .i_translate_hit(i_translate_hit), .i_translate_miss_fault(i_translate_miss_fault),
        .i_translate_prot_fault(i_translate_prot_fault),
        // D-cache.
        .o_dcache_en(o_dcache_en), .o_dcache_vaddr(o_dcache_vaddr),
        .i_dcache_hit(i_dcache_hit), .i_dcache_rdata(i_dcache_rdata),
        .o_dcache_paddr(o_dcache_paddr), .o_dcache_we(o_dcache_we),
        .o_dcache_wdata(o_dcache_wdata), .o_dcache_byte_en(o_dcache_byte_en),
        // Sysreg.
        .o_sys_re(o_sys_re), .o_sys_dev(o_sys_dev), .o_sys_reg(o_sys_reg),
        .i_sys_rdata(i_sys_rdata),
        .o_sys_we(o_sys_we), .o_sys_wr_dev(o_sys_wr_dev), .o_sys_wr_reg(o_sys_wr_reg),
        .o_sys_wdata(o_sys_wdata),
        // Bus-master line fill.
        .o_launch_fill(o_launch_fill), .o_fill_paddr(o_fill_paddr),
        .o_fill_cacheable(o_fill_cacheable),
        .i_fill_done(i_fill_done), .i_fill_data(i_fill_data), .i_fill_fault(i_fill_fault),
        // Branch resolution -> the front end.
        .o_branch_taken(branch_taken), .o_branch_target(branch_target),
        // Exception entry / return / WRSYS resync -> the front end.
        .o_fault_commit(fault_commit), .o_fault_vec(fault_vec), .o_epc(epc),
        .o_eret_commit(eret_commit),
        .o_wrsys_resync(wrsys_resync), .o_wrsys_resync_pc(wrsys_resync_pc),
        // MMU fault-register commit -- WB's qualified strobe + payload.
        .o_mmu_fault_commit(o_mmu_fault_commit), .o_mmu_fault_vaddr(o_mmu_fault_vaddr),
        .o_mmu_fault_status(o_mmu_fault_status),
        // Interrupt support -- observability out, IRQ save-state in.
        .o_sr_s(sr_s), .o_sr_i(sr_i), .o_ei_commit(ei_commit), .o_dc_commit(dc_commit),
        .o_ex_valid(ex_valid), .o_ex_stall(ex_stall),
        .i_irq_inject(irq_inject), .i_irq_vec(irq_vec),
        // Observability.
        .o_insn_committed(insn_committed), .o_bcause(o_bcause),
        .o_retire_valid(o_retire_valid), .o_retire_pc(o_retire_pc),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data), .o_commit_we(o_commit_we)
    );

    // Instructions retired: a distinct WB instruction commit (insn_committed,
    // once per instruction -- a dual-write's second register cycle is excluded)
    // OR a drain-commit, which commits from EX and never reaches WB. The two are
    // mutually exclusive per cycle, so the OR counts each retire exactly once.
    assign o_insn_retired   = insn_committed | dc_commit;
    assign o_insn_committed = insn_committed;

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
    // its instruction as a synthetic fault) it borrows the fetch port (muxed
    // above) to read vector_table[vec<<2] -- the handler address the kernel
    // stored there -- then redirects IF1 to the handler.
    penumbra3_vecfetch u_vecfetch (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_fault_commit(fault_commit), .i_fault_vec(fault_vec),
        .i_mem_rdata(i_fetch_rdata),
        .i_mem_busy(i_fetch_busy),
        .o_active(vecf_active),
        .o_fetch_addr(vecf_fetch_addr), .o_fetch_en(vecf_fetch_en),
        .o_fetch_re(vecf_fetch_re),
        .o_redirect(vecf_redirect), .o_redirect_pc(vecf_redirect_pc)
    );

    // ══════════════════════════════════════════════════════════
    // Interrupt unit -- recognition + EX-frontier injection
    // ══════════════════════════════════════════════════════════
    penumbra3_irq u_irq (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_irq(i_irq), .i_timer_irq(i_timer_irq),
        .i_sr_i(sr_i), .i_ei_commit(ei_commit),
        .i_retire_valid(o_retire_valid), .i_dc_commit(dc_commit),
        .i_ex_valid(ex_valid), .i_ex_stall(ex_stall),
        .i_fault_commit(fault_commit), .i_vecf_active(vecf_active),
        .o_irq_inject(irq_inject), .o_irq_vec(irq_vec)
    );

endmodule
