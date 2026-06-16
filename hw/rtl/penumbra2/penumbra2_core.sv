// penumbra2_core — Penumbra/2 CPU core: fetch + datapath spine.
//
// The bare pipelined core: the front end (penumbra2_if1_stage,
// penumbra2_if2_stage) onto penumbra2_spine (ID->EX->MEM->WB + regfile +
// scoreboard + flag bypass + SPR file), plus the two exception-path units —
// penumbra2_vecfetch (vector-table read + handler redirect) and penumbra2_irq
// (recognition + drain-and-take). Everything memory-shaped lives outside,
// behind three port groups the machine integration (machine_penumbra2) binds
// to the MMU, the split VIPT L1s, and the sysreg device complex:
//
//   - The fetch front port: launch (o_fetch_addr/o_fetch_en, the registered
//     lookup the I-side L1 and the MMU port A sample together), the request
//     (o_fetch_re, held level until the completion cycle), and the response
//     (i_fetch_rdata valid when i_fetch_busy is low — drop-equals-valid).
//     IF1 and the vector-fetch FSM share the port through the mux here:
//     while o_fetch_bypass (vecf active) is high the FSM owns it and its
//     reads are physical (the MMU's force-bypass, which also makes them
//     uncacheable — vector reads pass through the I-L1 without installing).
//   - The dmem front port (o_dmem_* / i_dmem_*): MEM's launch/request/
//     completion access, same contract, plus the MMU port-B query strobes
//     and verdict the spine's MEM stage drives and consumes.
//   - The sysreg sideband: the RDSYS read selectors + launch strobe with the
//     response due at data-ready (o_sys_* / i_sys_rdata), and the WRSYS
//     write port committed from EX (o_sys_wr_* / o_sys_wdata / o_sys_we).
//
// A core-level integration ties the busy inputs low and pairs the ports with
// a flat stand-in (unified_mem) — every access then completes on its resolve
// cycle, the original bring-up timing. The WB-qualified fault-commit strobe
// (o_mmu_fault_commit + payload) latches the MMU's architectural
// FAULT_ADDR/FAULT_STATUS only for a retiring address-carrying fault.
//
// No halt: a real CPU never stops on an instruction. BREAK is a trap, taken at
// EX and vectored to VEC_BREAK (gen1 likewise vectors BREAK to its monitor),
// not a halt — so the core has no o_halted. It exposes retire-observability
// (o_retire_valid + o_retire_op_class) and a consumer decides program end by
// watching it: a BREAK carries no architectural write, so it is invisible on
// the commit port, but the cycle it takes its trap it still leaves WB as a
// valid slot, visible here as a retiring OPC_BREAK. In-order commit guarantees
// every instruction older than the BREAK has already retired by then.
// A perfctr counts o_insn_retired (a WB retirement OR a drain-commit, which
// commits from EX and never reaches WB), not o_retire_valid alone — the latter
// would miss every ERET/WRSYS/EI/DI.

module penumbra2_core
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
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
    output logic                  o_fetch_en,      // lookup launch (I-L1 + MMU port A sample this)
    output logic                  o_fetch_re,      // word request, held until completion
    input  logic [31:0]           i_fetch_rdata,   // valid when i_fetch_busy low
    input  logic                  i_fetch_busy,
    input  logic                  i_fetch_fault,   // bus fault on the fetch, at i_fetch_busy drop

    // ── Fetch translation context + MMU port-A verdict ───────────
    output logic                  o_fetch_bypass,  // vector-fetch owns the port: physical read
    output logic                  o_fetch_user,    // fetch privilege (query + status info)
    input  logic                  i_fetch_mmu_fault,
    input  logic [31:0]           i_fetch_mmu_fault_status,

    // ── Data front port (launch / request / response) ────────────
    output logic [31:0]           o_dmem_addr,
    output logic [31:0]           o_dmem_wdata,
    output logic [3:0]            o_dmem_byte_en,
    output logic                  o_dmem_re,
    output logic                  o_dmem_we,
    output logic                  o_dmem_en,
    input  logic [31:0]           i_dmem_rdata,
    input  logic                  i_dmem_busy,
    input  logic                  i_dmem_fault,    // bus fault on the data access, at i_dmem_busy drop

    // ── MMU D-side translate (port B: query at launch, verdict at data-ready) ──
    output logic [31:0]           o_mmu_vaddr,
    output logic [2:0]            o_mmu_access_type,
    output logic                  o_mmu_user,
    output logic                  o_mmu_req,
    input  logic                  i_mmu_fault,
    input  logic [31:0]           i_mmu_fault_status,

    // ── MMU fault-register commit (WB's qualified strobe + payload) ──
    output logic                  o_mmu_fault_commit,
    output logic [31:0]           o_mmu_fault_vaddr,
    output logic [31:0]           o_mmu_fault_status,

    // ── Sysreg sideband (RDSYS read; response due at data-ready) ─
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    output logic                  o_sys_re,
    input  logic [31:0]           i_sys_rdata,

    // ── Sysreg write port (WRSYS commit from EX) ─────────────────
    output logic [3:0]            o_sys_wr_dev,
    output logic [3:0]            o_sys_wr_reg,
    output logic [31:0]           o_sys_wdata,
    output logic                  o_sys_we,

    // ── Commit observability (WB regfile write port) ─────────────
    output logic [SB_IDX_W-1:0]   o_commit_idx,
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we,

    // ── Retire observability (the instruction leaving WB) ────────
    output logic                  o_retire_valid,
    output logic [OPC_W-1:0]      o_retire_op_class,
    output logic                  o_insn_retired,    // retire pulse incl. drain-commit (perfctr)
    output logic [31:0]           o_retire_pc,       // the retiring instruction's PC (trace)
    output logic [31:0]           o_retire_sr,       // architectural SR as the instruction commits

    // ── Stall-attribution observability (perfctr) ────────────────
    // Per-cause stall signals, surfaced for the machine's CPU performance
    // counters. They can overlap; the perfctr resolves them by head-of-line
    // priority. Pure observation — they drive no pipeline logic.
    output logic                  o_stall_funit,     // EX waiting on the divmul unit
    output logic                  o_stall_load,      // MEM holding for a load access
    output logic                  o_stall_store,     // MEM holding for a store access

    // ── Exception observability (trace markers) ──────────────────
    // The same fault/ERET commit pulses the front end already acts on,
    // surfaced for the trace stream so a consumer can annotate traps.
    output logic                  o_fault_commit,    // a fault is taken this cycle
    output logic [3:0]            o_fault_vec,       // its vector number
    output logic                  o_eret_commit,     // an ERET is committing this cycle

    // ── Drain-commit observability (EI/DI/WRSYS/ERET retire from EX) ──
    // These never reach WB, so they are absent from o_retire_*; surfaced
    // separately so the trace stream has no gaps over sysreg/mode code.
    output logic                  o_dc_commit,
    output logic [31:0]           o_dc_commit_pc,
    output logic [OPC_W-1:0]      o_dc_commit_op_class,

    // ── Branch-resolution observability (trace) ──────────────────
    // The EX branch resolve that steers the front end — surfaced so a
    // resolved-but-flushed branch (one that redirects fetch yet never
    // retires) is visible, which a retire-only trace cannot show.
    output logic                  o_branch_taken,
    output logic [31:0]           o_branch_target,
    output logic [31:0]           o_branch_pc,

    // ── Pipeline occupancy (trace): instruction in EX / MEM (WB = retire) ──
    output logic [31:0]           o_ex_pc,
    output logic                  o_ex_valid,
    output logic [31:0]           o_mem_pc,
    output logic                  o_mem_valid
);

    // ── Front-end wires ──────────────────────────────────────────
    logic [31:0] if1_fetch_addr;
    logic        if1_fetch_en;
    logic [31:0] if1_pc, if1_next_pc;
    logic        if1_valid;
    logic [31:0] if2_ir, if2_pc, if2_next_pc;
    logic        if2_valid;
    logic        if2_fetch_re;
    logic        if2_fault_pending;
    logic [3:0]  if2_fault_vec;
    logic [31:0] if2_fault_status;

    // ── Handshake wires ──────────────────────────────────────────
    logic        if2_stall;     // IF2 -> IF1 back-pressure
    logic        fetch_stall;   // spine (ID) -> IF2 back-pressure

    // ── Taken-branch redirect (EX -> front end) ──────────────────
    logic        branch_taken;  // flush IF1/IF2 + steer PC this cycle
    logic [31:0] branch_target; // resolved branch target

    // ── Exception entry (WB -> front end via the vector-fetch FSM) ─
    logic        fault_commit;  // a fault is taken this cycle (flush IF1/IF2, launch entry)
    logic [3:0]  fault_vec;
    logic        vecf_active;       // FSM owns the fetch port + holds IF1
    logic [31:0] vecf_fetch_addr;
    logic        vecf_fetch_en;
    logic        vecf_fetch_re;
    logic        vecf_redirect;     // steer PC to the handler
    logic [31:0] vecf_redirect_pc;

    // ── ERET return (EX drain-commit -> front end) ───────────────
    logic        eret_commit;       // an ERET is committing: redirect PC ← EPC
    logic [31:0] epc;               // saved exception PC

    // ── WRSYS context-sync re-fetch (EX drain-commit -> front end) ─
    logic        wrsys_resync;      // re-fetch after a WRSYS (post-commit-wait release)
    logic [31:0] wrsys_resync_pc;   // its target = the WRSYS's sequential successor

    // ── Interrupt support (interrupt unit <-> spine + front end) ──
    logic        sr_s, sr_i, ei_commit, dc_commit, ex_stall;   // from spine
    logic        irq_inject;        // take the interrupt: tag the EX instruction (a synthetic fault)
    logic [3:0]  irq_vec;

    // The vector-fetch FSM is launched by a fault commit at WB. An interrupt is
    // one such commit now — the EX stage tags its instruction as a synthetic
    // fault — so fault_commit / fault_vec carry the interrupt's entry and vector
    // too, with no separate launch source.
    logic        vecf_launch;
    logic [3:0]  vecf_launch_vec;
    assign vecf_launch     = fault_commit;
    assign vecf_launch_vec = fault_vec;

    // ── Front-end redirect / flush composition + fetch-port mux ──
    // Three control-flow events steer or flush IF: a taken branch, the vector
    // fetch reaching its handler, and an ERET returning to EPC. They are
    // mutually exclusive in time.
    logic        if1_redirect;      // steer PC + bubble IF1's slot
    logic [31:0] if1_redirect_pc;   // the target for that redirect
    logic        if2_flush;         // bubble IF2's slot

    // The front-end redirect/flush, composed from the control-flow events:
    //   branch:       branch_taken     → branch_target
    //   vector fetch: vecf_redirect    → vecf_redirect_pc
    //   ERET:         eret_commit      → epc
    //   fault:        fault_commit     (flushes IF2's wrong-path word now, but
    //                 does NOT steer PC here — its redirect comes later, via the
    //                 vector fetch; IF1's fault flush is i_flush, wired below).
    // if1_redirect steers PC for the three that know their target (branch, then
    // vector fetch, then ERET — the order is arbitrary since they never
    // coincide); if2_flush bubbles IF2 for any front-end kill including the fault.

    always_comb begin
        // Idle defaults — overridden by the events below. Without these the
        // unassigned paths (fault-only, no-event) would infer latches.
        if1_redirect    = 1'b0;
        if1_redirect_pc = branch_target;   // don't-care while if1_redirect=0
        if2_flush       = 1'b0;
        if (branch_taken) begin
            if1_redirect    = 1'b1;
            if1_redirect_pc = branch_target;
            if2_flush       = 1'b1;
        end else if (vecf_redirect) begin
            if1_redirect    = 1'b1;
            if1_redirect_pc = vecf_redirect_pc;
            if2_flush       = 1'b1;
        end else if (eret_commit) begin
            if1_redirect    = 1'b1;
            if1_redirect_pc = epc;
            if2_flush       = 1'b1;
        end else if (wrsys_resync) begin
            // WRSYS is context-synchronizing: re-fetch its successor so the
            // following instructions observe the new sysreg state.
            if1_redirect    = 1'b1;
            if1_redirect_pc = wrsys_resync_pc;
            if2_flush       = 1'b1;
        end else if (fault_commit) begin
            if2_flush       = 1'b1;
        end
    end

    // The fetch port: IF1 drives it normally, the vector-fetch FSM while it
    // owns it (o_fetch_bypass tells the MMU those reads are physical). IF2's
    // request can never overlap the FSM's — entry flushed IF2's slot first.
    assign o_fetch_addr   = vecf_active ? vecf_fetch_addr : if1_fetch_addr;
    assign o_fetch_en     = vecf_active ? vecf_fetch_en   : if1_fetch_en;
    assign o_fetch_re     = vecf_active ? vecf_fetch_re   : if2_fetch_re;
    assign o_fetch_bypass = vecf_active;

    // The committed SR.S (from the spine's SPR file) is the privilege source:
    // one site feeds the spine's decode-time checks and both MMU queries, plus
    // the fetch-side user bit. Privilege changes only via drain-commit ops
    // (exception entry / ERET), which flush younger instructions, so every
    // in-flight instruction shares this one committed value.
    logic core_supervisor;
    assign core_supervisor = sr_s;
    assign o_fetch_user    = ~core_supervisor;

    // ══════════════════════════════════════════════════════════
    // IF1 — PC + fetch address generation
    // ══════════════════════════════════════════════════════════
    penumbra2_if1_stage #(.RESET_PC(RESET_PC)) u_if1 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_stall_in(if2_stall | vecf_active),     // held while the FSM owns the fetch port
        .i_mem_busy(i_fetch_busy),
        .i_redirect(if1_redirect), .i_redirect_pc(if1_redirect_pc),
        .i_flush(fault_commit),                    // bubble the wrong-path fetch at the fault
        .i_fetch_stop(1'b0),                       // unused: the interrupt no longer drains at fetch
        .o_fetch_addr(if1_fetch_addr), .o_fetch_en(if1_fetch_en),
        .o_pc(if1_pc), .o_next_pc(if1_next_pc), .o_valid(if1_valid)
    );

    // ══════════════════════════════════════════════════════════
    // IF2 — deliver fetched word + PC to ID
    // ══════════════════════════════════════════════════════════
    penumbra2_if2_stage u_if2 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_pc(if1_pc), .i_next_pc(if1_next_pc), .i_valid(if1_valid),
        .i_ir(i_fetch_rdata),
        .i_mem_busy(i_fetch_busy), .i_mem_fault(i_fetch_fault),
        .o_fetch_re(if2_fetch_re),
        // I-side MMU verdict — port A's registered, held result for the
        // fetch IF1 launched (paired by the shared launch strobe).
        .i_user_mode(o_fetch_user),
        .i_mmu_fault(i_fetch_mmu_fault),
        .i_mmu_fault_status(i_fetch_mmu_fault_status),
        .i_stall_in(fetch_stall),
        .i_flush(if2_flush),
        .o_stall(if2_stall),
        .o_ir(if2_ir), .o_pc(if2_pc), .o_next_pc(if2_next_pc),
        .o_valid(if2_valid),
        .o_fault_pending(if2_fault_pending), .o_fault_vec(if2_fault_vec),
        .o_fault_status(if2_fault_status)
    );

    // ══════════════════════════════════════════════════════════
    // Datapath spine — ID -> EX -> MEM -> WB
    // ══════════════════════════════════════════════════════════
    penumbra2_spine u_spine (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_ir(if2_ir), .i_pc(if2_pc), .i_next_pc(if2_next_pc),
        .i_valid(if2_valid),
        .i_fault_pending(if2_fault_pending), .i_fault_vec(if2_fault_vec),
        .i_fault_status(if2_fault_status),
        .i_supervisor(core_supervisor),
        .o_fetch_stall(fetch_stall),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data),
        .o_commit_we(o_commit_we),
        .o_retire_valid(o_retire_valid), .o_retire_op_class(o_retire_op_class),
        .o_retire_pc(o_retire_pc), .o_retire_sr(o_retire_sr),
        .o_branch_taken(branch_taken), .o_branch_target(branch_target),
        .o_branch_pc(o_branch_pc),
        .o_ex_pc(o_ex_pc), .o_ex_valid(o_ex_valid),
        .o_mem_pc(o_mem_pc), .o_mem_valid(o_mem_valid),
        .o_dmem_addr(o_dmem_addr), .o_dmem_wdata(o_dmem_wdata),
        .o_dmem_byte_en(o_dmem_byte_en), .o_dmem_re(o_dmem_re),
        .o_dmem_we(o_dmem_we), .o_dmem_en(o_dmem_en),
        .i_dmem_rdata(i_dmem_rdata), .i_dmem_busy(i_dmem_busy),
        .i_dmem_fault(i_dmem_fault),
        // MMU D-side translate — MEM's port-B query, verdict at data-ready.
        .o_mmu_vaddr(o_mmu_vaddr), .o_mmu_access_type(o_mmu_access_type),
        .o_mmu_user(o_mmu_user), .o_mmu_req(o_mmu_req),
        .i_mmu_fault(i_mmu_fault), .i_mmu_fault_status(i_mmu_fault_status),
        // MMU fault-register commit — WB's qualified strobe + payload.
        .o_mmu_fault_commit(o_mmu_fault_commit),
        .o_mmu_fault_vaddr(o_mmu_fault_vaddr),
        .o_mmu_fault_status(o_mmu_fault_status),
        // Sysreg sideband — MEM's RDSYS read against the external device complex.
        .o_sys_dev(o_sys_dev), .o_sys_reg(o_sys_reg), .o_sys_re(o_sys_re),
        .i_sys_rdata(i_sys_rdata),
        // Sysreg write port — WRSYS commit from EX.
        .o_sys_wr_dev(o_sys_wr_dev), .o_sys_wr_reg(o_sys_wr_reg),
        .o_sys_wdata(o_sys_wdata), .o_sys_we(o_sys_we),
        // WRSYS context-sync — re-fetch the successor under the new state.
        .o_wrsys_resync(wrsys_resync), .o_wrsys_resync_pc(wrsys_resync_pc),
        // Exception entry — drives the IF flush + the vector-fetch FSM below.
        // ERET commit redirects PC ← EPC through the same front-end path.
        .o_fault_commit(fault_commit), .o_fault_vec(fault_vec), .o_epc(epc),
        .o_eret_commit(eret_commit),
        // Stall-attribution observability — straight through to the machine perfctr.
        .o_stall_funit(o_stall_funit), .o_stall_load(o_stall_load),
        .o_stall_store(o_stall_store),
        // Interrupt support — observability out, IRQ save-state in.
        .o_sr_s(sr_s), .o_sr_i(sr_i), .o_ei_commit(ei_commit), .o_dc_commit(dc_commit),
        .o_dc_commit_pc(o_dc_commit_pc), .o_dc_commit_op_class(o_dc_commit_op_class),
        .o_ex_stall(ex_stall),
        .i_irq_inject(irq_inject), .i_irq_vec(irq_vec)
    );

    // Instructions retired: a normal WB retirement OR a drain-commit — which
    // commits from EX and never enters WB, so o_retire_valid alone would miss
    // every ERET/WRSYS/EI/DI. The two sources are mutually exclusive per cycle
    // (a drain-commit only fires once MEM/WB are drained, i.e. o_retire_valid is
    // 0), so the OR counts each retiring instruction exactly once.
    assign o_insn_retired = o_retire_valid | dc_commit;

    // Trace observability: the same commit pulses the front end consumes for
    // its flush / vector-fetch / ERET redirects, surfaced as ports.
    assign o_fault_commit = fault_commit;
    assign o_fault_vec    = fault_vec;
    assign o_eret_commit  = eret_commit;
    assign o_dc_commit    = dc_commit;
    assign o_branch_taken  = branch_taken;
    assign o_branch_target = branch_target;

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_retire_valid && dc_commit))
        else $error("penumbra2_core: WB retire and drain-commit collided (double-count)");

    // ══════════════════════════════════════════════════════════
    // Vector-fetch FSM — reads the handler address, redirects PC
    // ══════════════════════════════════════════════════════════
    // On either entry source (a fault commit or an interrupt entry) it borrows
    // the fetch port (muxed above) to read vector_table[vec<<2] — the handler
    // address the kernel stored there — then redirects IF1 to the handler.
    // The launch waits out any in-flight (killed) transaction on the port.
    penumbra2_vecfetch u_vecfetch (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_fault_commit(vecf_launch), .i_fault_vec(vecf_launch_vec),
        .i_mem_rdata(i_fetch_rdata),
        .i_mem_busy(i_fetch_busy),
        .o_active(vecf_active),
        .o_fetch_addr(vecf_fetch_addr), .o_fetch_en(vecf_fetch_en),
        .o_fetch_re(vecf_fetch_re),
        .o_redirect(vecf_redirect), .o_redirect_pc(vecf_redirect_pc)
    );

    // ══════════════════════════════════════════════════════════
    // Interrupt unit — recognition + EX-frontier injection
    // ══════════════════════════════════════════════════════════
    // Recognizes an eligible IRQ and, when the EX slot is a clean committable
    // boundary, pulses irq_inject — which tags the EX instruction as a synthetic
    // fault (penumbra2_ex_stage). It then rides the fault path: save-state
    // EPC ← its own PC, squash younger, vector fetch. The clean-boundary gate is
    // ~ex_stall (no older access still in flight) so EPC and the saved SR land
    // on a clean boundary — "don't take an interrupt while stalled", structural.
    penumbra2_irq u_irq (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_irq(i_irq), .i_timer_irq(i_timer_irq),
        .i_sr_i(sr_i), .i_ei_commit(ei_commit),
        .i_retire_valid(o_retire_valid), .i_dc_commit(dc_commit),
        .i_ex_valid(o_ex_valid), .i_ex_stall(ex_stall),
        .i_fault_commit(fault_commit), .i_vecf_active(vecf_active),
        .o_irq_inject(irq_inject), .o_irq_vec(irq_vec)
    );

endmodule
