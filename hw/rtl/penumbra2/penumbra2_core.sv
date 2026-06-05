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
// reads it). Still not wired: the MMU (both fault paths), the real BRAM-backed
// L1 caches, and RDSYS. unified_mem is a stand-in with the streaming
// registered-read contract the IF1/IF2 split and the MEM single-STALL are built
// around — real BRAM-backed caches replace it later behind the IF and dmem
// interfaces.
//
// No halt: a real CPU never stops on an instruction. BREAK is a trap (taken
// at EX once the exception unit exists), not a halt — gen1 likewise vectors
// BREAK to its monitor. So the core has no o_halted. It exposes
// retire-observability (o_retire_valid + o_retire_op_class) and a testbench
// decides program end by watching it: a BREAK carries no architectural write,
// so it is invisible on the commit port, but it is visible here as a retiring
// OPC_BREAK. In-order commit guarantees every instruction older than the
// BREAK has already retired by then, so a testbench that stops there sees
// final architectural state. o_retire_valid doubles as the insns-retired
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

    // ── Data-memory wires (MEM <-> data BRAM stand-in) ───────────
    logic [31:0] dmem_addr, dmem_wdata, dmem_rdata;
    logic [3:0]  dmem_byte_en;
    logic        dmem_we, dmem_en;

    // ══════════════════════════════════════════════════════════
    // IF1 — PC + fetch address generation
    // ══════════════════════════════════════════════════════════
    penumbra2_if1_stage #(.RESET_PC(RESET_PC)) u_if1 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_stall_in(if2_stall),
        .i_redirect(branch_taken), .i_redirect_pc(branch_target),
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
        .i_flush(branch_taken),
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
        .i_dmem_rdata(dmem_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Unified memory — one backing array, two ports (fetch + data)
    // ══════════════════════════════════════════════════════════
    // Port A serves IF1's fetch; port B serves MEM's data access. One address
    // space (ROM region holds the INIT_FILE code at 0xFFFF_0000, RAM region is
    // low), so a store is visible to a later fetch — the property exception
    // entry needs to read a software-written vector table.
    unified_mem #(.REGION_WORDS(MEM_REGION_WORDS), .INIT_FILE(INIT_FILE)) u_mem (
        .i_clk(i_clk),
        .i_a_addr(if1_fetch_addr), .i_a_en(if1_fetch_en), .o_a_rdata(imem_rdata),
        .i_b_addr(dmem_addr), .i_b_wdata(dmem_wdata), .i_b_byte_en(dmem_byte_en),
        .i_b_we(dmem_we), .i_b_en(dmem_en), .o_b_rdata(dmem_rdata)
    );

    // ── Assertion (sim-only; stripped at synth) ──────────────────
    // A redirect target is word-aligned. Format-B targets are PC + (off<<2),
    // structurally aligned; a register-sourced JMP could be misaligned, which
    // in the full design raises an I-side alignment fault. That path is not
    // wired yet (no MMU), so a misaligned redirect would silently fetch garbage
    // from the flat i-mem — this is a bring-up guard until the fault path lands.
    assert property (@(posedge i_clk) disable iff (i_rst)
        branch_taken |-> branch_target[1:0] == 2'b00)
        else $error("penumbra2_core: misaligned branch redirect target");

    /* verilator lint_on PINCONNECTEMPTY */
endmodule
