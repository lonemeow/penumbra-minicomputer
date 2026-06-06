// penumbra2_core — Penumbra/2 CPU core: fetch + datapath spine.
//
// The first time the gen2 core fetches and executes its own instructions.
// It wires the front end (penumbra2_if1_stage, an instruction memory,
// penumbra2_if2_stage) onto penumbra2_spine (ID->EX->MEM->WB + regfile +
// scoreboard + flag bypass), closing the fetch loop the spine's testbench
// used to drive by hand:
//   - PC -> IF1 -> i-mem -> IF2 -> spine -> commit,
//   - the back-pressure chain spine(fetch_stall) -> IF2 -> IF1 (hold PC).
//
// Scope of this milestone: straight-line, control-flow, and load/store
// streams ending in BREAK. The taken-branch redirect is closed: EX resolves a
// branch (spine.o_branch_*), IF1 steers PC to the target, and IF1/IF2/ID bubble
// the three wrong-path slots (the 3-bubble flush). Loads and stores run through
// the MEM stage against the data port of a single unified memory (unified_mem):
// fetch and data share one address space and one backing array, presented
// through two ports — the role the real split L1 I/D caches play over unified
// physical memory. A store is therefore visible to a later fetch, which the
// exception path needs (the kernel writes the vector table; the vector fetch
// reads it). Synchronous faults are taken: a data fault commits at WB, which
// flushes the pipeline (ID/EX/MEM in the spine; IF1/IF2 here), pulses
// save-state into the SPR file, and launches penumbra2_vecfetch — which reads
// the handler address from the vector table over the (muxed) fetch port and
// redirects PC to it. ERET returns (SR←ESR, PC←EPC) and SYSCALL/BREAK raise as
// traps at EX, both routed through that same entry/redirect path. Still not
// wired: the MMU (both fault paths), the real BRAM-backed L1 caches, RDSYS, and
// interrupts. unified_mem is a stand-in with the streaming registered-read
// contract the IF1/IF2 split and the MEM single-STALL are built around —
// real BRAM-backed caches replace it later behind the IF and dmem
// interfaces.
//
// No halt: a real CPU never stops on an instruction. BREAK is a trap, taken at
// EX and vectored to VEC_BREAK (gen1 likewise vectors BREAK to its monitor),
// not a halt — so the core has no o_halted. It exposes retire-observability
// (o_retire_valid + o_retire_op_class) and a testbench decides program end by
// watching it: a BREAK carries no architectural write, so it is invisible on
// the commit port, but the cycle it takes its trap it still leaves WB as a
// valid slot, visible here as a retiring OPC_BREAK. In-order commit guarantees
// every instruction older than the BREAK has already retired by then, so a
// testbench that stops there sees final architectural state. o_retire_valid
// doubles as the insns-retired
// pulse a perfctr would count on real hardware.

module penumbra2_core
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
#(
    parameter logic [31:0] RESET_PC         = 32'hFFFF_0000,  // matches the hw-test --org convention
    parameter int          MEM_REGION_WORDS = 4096,           // words per region (RAM, ROM)
    parameter string       INIT_FILE        = "program.hex"   // $readmemh image (loaded into ROM region)
)(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── Interrupt lines (level, wired-OR external + timer) ───────
    input  logic                  i_irq,
    input  logic                  i_timer_irq,

    // ── Commit observability (WB regfile write port) ─────────────
    output logic [SB_IDX_W-1:0]   o_commit_idx,
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we,

    // ── Retire observability (the instruction leaving WB) ────────
    // o_retire_valid is the insns-retired pulse (real-hw perfctr source).
    // o_retire_op_class lets a consumer act on a retiring instruction the
    // commit port can't see — the testbench reads it to detect a retiring
    // BREAK as program end. There is deliberately no halt here (see header).
    output logic                  o_retire_valid,
    output logic [OPC_W-1:0]      o_retire_op_class
);

    // IF2's fault outputs have no consumer in this milestone (no MMU yet).
    /* verilator lint_off PINCONNECTEMPTY */

    // ── Front-end wires ──────────────────────────────────────────
    logic [31:0] if1_fetch_addr;
    logic        if1_fetch_en;
    logic [31:0] if1_pc, if1_next_pc;
    logic        if1_valid;
    logic [31:0] imem_rdata;
    logic [31:0] if2_ir, if2_pc, if2_next_pc;
    logic        if2_valid;

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
    logic        vecf_redirect;     // steer PC to the handler
    logic [31:0] vecf_redirect_pc;

    // ── ERET return (EX drain-commit -> front end) ───────────────
    logic        eret_commit;       // an ERET is committing: redirect PC ← EPC
    logic [31:0] epc;               // saved exception PC

    // ── Interrupt entry (interrupt unit <-> spine + front end) ────
    logic        sr_i, ei_commit, dc_commit, pipe_busy;   // from spine
    logic        irq_fetch_stop;    // freeze IF1 at the boundary while draining
    logic        irq_entry;         // take the interrupt: save-state + vector fetch
    logic [3:0]  irq_vec;
    logic [31:0] irq_epc;

    // The vector-fetch FSM is launched by either entry source — a fault commit
    // (handler from the faulting PC) or an interrupt entry — carrying that
    // source's vector. They are mutually exclusive (a fault preempts the drain).
    logic        vecf_launch;
    logic [3:0]  vecf_launch_vec;
    assign vecf_launch     = fault_commit | irq_entry;
    assign vecf_launch_vec = fault_commit ? fault_vec : irq_vec;

    // ── Front-end redirect / flush composition + fetch-port mux ──
    // Three control-flow events steer or flush IF: a taken branch, the vector
    // fetch reaching its handler, and an ERET returning to EPC. They are
    // mutually exclusive in time.
    logic        if1_redirect;      // steer PC + bubble IF1's slot
    logic [31:0] if1_redirect_pc;   // the target for that redirect
    logic        if2_flush;         // bubble IF2's slot
    logic [31:0] fetch_addr_mux;
    logic        fetch_en_mux;

    // TODO(human): compose if1_redirect, if1_redirect_pc, and if2_flush from
    // the control-flow events. Sources:
    //   branch:       branch_taken     → branch_target
    //   vector fetch: vecf_redirect    → vecf_redirect_pc
    //   ERET:         eret_commit      → epc
    //   fault:        fault_commit     (flushes IF2's wrong-path word now, but
    //                 does NOT steer PC here — its redirect comes later, via the
    //                 vector fetch; IF1's fault flush is i_flush, wired below).
    // if1_redirect steers PC for the three that know their target; if2_flush
    // bubbles IF2 for any front-end kill including the fault. Pick a priority
    // for if1_redirect_pc (the steering sources never coincide).

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
        end else if (fault_commit) begin
            if2_flush       = 1'b1;
        end
    end

    assign fetch_addr_mux  = vecf_active ? vecf_fetch_addr : if1_fetch_addr;
    assign fetch_en_mux    = vecf_active ? vecf_fetch_en   : if1_fetch_en;

    // ── Data-memory wires (MEM <-> data port of the unified memory) ─
    logic [31:0] dmem_addr, dmem_wdata, dmem_rdata;
    logic [3:0]  dmem_byte_en;
    logic        dmem_we, dmem_en;

    // ══════════════════════════════════════════════════════════
    // IF1 — PC + fetch address generation
    // ══════════════════════════════════════════════════════════
    penumbra2_if1_stage #(.RESET_PC(RESET_PC)) u_if1 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_stall_in(if2_stall | vecf_active),     // held while the FSM owns the fetch port
        .i_redirect(if1_redirect), .i_redirect_pc(if1_redirect_pc),
        .i_flush(fault_commit),                    // bubble the wrong-path fetch at the fault
        .i_fetch_stop(irq_fetch_stop),             // freeze at the boundary while draining for an IRQ
        .o_fetch_addr(if1_fetch_addr), .o_fetch_en(if1_fetch_en),
        .o_pc(if1_pc), .o_next_pc(if1_next_pc), .o_valid(if1_valid)
    );

    // Instruction fetch reads port A of the unified memory (instantiated below,
    // alongside the data port). The fetched word lands in imem_rdata next cycle.

    // ══════════════════════════════════════════════════════════
    // IF2 — deliver fetched word + PC to ID
    // ══════════════════════════════════════════════════════════
    penumbra2_if2_stage u_if2 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_pc(if1_pc), .i_next_pc(if1_next_pc), .i_valid(if1_valid),
        .i_ir(imem_rdata),
        .i_stall_in(fetch_stall),
        .i_flush(if2_flush),
        .o_stall(if2_stall),
        .o_ir(if2_ir), .o_pc(if2_pc), .o_next_pc(if2_next_pc),
        .o_valid(if2_valid),
        .o_fault_pending(), .o_fault_vec()
    );

    // ══════════════════════════════════════════════════════════
    // Datapath spine — ID -> EX -> MEM -> WB
    // ══════════════════════════════════════════════════════════
    penumbra2_spine u_spine (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_ir(if2_ir), .i_pc(if2_pc), .i_next_pc(if2_next_pc),
        .i_valid(if2_valid),
        .i_supervisor(1'b1),                 // out of reset in supervisor mode
        .o_fetch_stall(fetch_stall),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data),
        .o_commit_we(o_commit_we),
        .o_retire_valid(o_retire_valid), .o_retire_op_class(o_retire_op_class),
        .o_branch_taken(branch_taken), .o_branch_target(branch_target),
        .o_dmem_addr(dmem_addr), .o_dmem_wdata(dmem_wdata),
        .o_dmem_byte_en(dmem_byte_en), .o_dmem_we(dmem_we), .o_dmem_en(dmem_en),
        .i_dmem_rdata(dmem_rdata),
        // Exception entry — drives the IF flush + the vector-fetch FSM below.
        // ERET commit redirects PC ← EPC through the same front-end path.
        .o_fault_commit(fault_commit), .o_fault_vec(fault_vec), .o_epc(epc),
        .o_eret_commit(eret_commit),
        // Interrupt support — observability out, IRQ save-state in.
        .o_sr_i(sr_i), .o_ei_commit(ei_commit), .o_dc_commit(dc_commit),
        .o_pipe_busy(pipe_busy),
        .i_irq_entry(irq_entry), .i_irq_epc(irq_epc)
    );

    // ══════════════════════════════════════════════════════════
    // Vector-fetch FSM — reads the handler address, redirects PC
    // ══════════════════════════════════════════════════════════
    // On either entry source (a fault commit or an interrupt entry) it borrows
    // the fetch port (port A, muxed above) to read vector_table[vec<<2] from the
    // RAM region — the handler address the kernel stored there — then redirects
    // IF1 to the handler.
    penumbra2_vecfetch u_vecfetch (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_fault_commit(vecf_launch), .i_fault_vec(vecf_launch_vec),
        .i_mem_rdata(imem_rdata),
        .o_active(vecf_active),
        .o_fetch_addr(vecf_fetch_addr), .o_fetch_en(vecf_fetch_en),
        .o_redirect(vecf_redirect), .o_redirect_pc(vecf_redirect_pc)
    );

    // ══════════════════════════════════════════════════════════
    // Interrupt unit — recognition + drain-and-take
    // ══════════════════════════════════════════════════════════
    // Recognizes an eligible IRQ, stops the front end at the boundary, drains
    // the in-flight stream, then pulses irq_entry — which save-states the
    // boundary PC (into the spine's SPR file) and launches the vector fetch
    // above. The drained signal spans the whole pipe: IF2 here plus the spine's
    // ID/EX/MEM/WB (pipe_busy).
    penumbra2_irq u_irq (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_irq(i_irq), .i_timer_irq(i_timer_irq),
        .i_sr_i(sr_i), .i_ei_commit(ei_commit),
        .i_retire_valid(o_retire_valid), .i_dc_commit(dc_commit),
        .i_pipe_busy(if2_valid | pipe_busy),
        .i_boundary_pc(if1_fetch_addr),
        .i_fault_commit(fault_commit), .i_vecf_active(vecf_active),
        .o_fetch_stop(irq_fetch_stop),
        .o_irq_entry(irq_entry), .o_irq_vec(irq_vec), .o_irq_epc(irq_epc)
    );

    // ══════════════════════════════════════════════════════════
    // Unified memory — one backing array, two ports (fetch + data)
    // ══════════════════════════════════════════════════════════
    // Port A serves IF1's fetch; port B serves MEM's data access. One address
    // space (ROM region holds the INIT_FILE code at 0xFFFF_0000, RAM region is
    // low), so a store is visible to a later fetch — the property exception
    // entry needs to read a software-written vector table.
    // Port A's address/enable are muxed (fetch_addr_mux/fetch_en_mux): IF1
    // drives them normally, the vector-fetch FSM while it owns the port.
    unified_mem #(.REGION_WORDS(MEM_REGION_WORDS), .INIT_FILE(INIT_FILE)) u_mem (
        .i_clk(i_clk),
        .i_a_addr(fetch_addr_mux), .i_a_en(fetch_en_mux), .o_a_rdata(imem_rdata),
        .i_b_addr(dmem_addr), .i_b_wdata(dmem_wdata), .i_b_byte_en(dmem_byte_en),
        .i_b_we(dmem_we), .i_b_en(dmem_en), .o_b_rdata(dmem_rdata)
    );

    // ── Assertion (sim-only; stripped at synth) ──────────────────
    // A redirect target is word-aligned — for a branch (PC + off<<2, always
    // aligned) and for a vector-fetch handler address (a misaligned handler
    // address in the vector table is a kernel setup bug). A register-sourced
    // JMP could be misaligned, which the full design raises as an I-side
    // alignment fault; that I-fetch fault path is not wired yet (no MMU), so
    // this stays a bring-up guard until it lands.
    assert property (@(posedge i_clk) disable iff (i_rst)
        if1_redirect |-> if1_redirect_pc[1:0] == 2'b00)
        else $error("penumbra2_core: misaligned redirect target");

    /* verilator lint_on PINCONNECTEMPTY */
endmodule
