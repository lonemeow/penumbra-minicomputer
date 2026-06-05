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
// Scope of this milestone: straight-line ALU/divmul streams ending in BREAK.
// Not yet wired: the taken-branch redirect (spine.o_branch_* is exposed but
// the PC redirect into IF1 is deferred to the branch milestone), the I-side
// fault path (no MMU; IF2 raises no fault), and loads/stores (the MEM data
// path is still a skeleton). The instruction memory is a flat BRAM stand-in
// (bram_mem) with the streaming registered-read contract the IF1/IF2 split is
// built around — a real BRAM-backed I-cache replaces it later, behind the
// same IF interface.
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
    parameter logic [31:0] RESET_PC   = 32'hFFFF_0000,  // matches the hw-test --org convention
    parameter int          IMEM_WORDS = 4096,           // 16 KB instruction memory
    parameter string       INIT_FILE  = "program.hex"   // $readmemh image for the i-mem
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

    // IF2's fault outputs and the spine's branch-redirect outputs have no
    // consumer in this milestone (no MMU, no PC redirect yet).
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

    // ══════════════════════════════════════════════════════════
    // IF1 — PC + fetch address generation
    // ══════════════════════════════════════════════════════════
    penumbra2_if1_stage #(.RESET_PC(RESET_PC)) u_if1 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_stall_in(if2_stall),
        .o_fetch_addr(if1_fetch_addr), .o_fetch_en(if1_fetch_en),
        .o_pc(if1_pc), .o_next_pc(if1_next_pc), .o_valid(if1_valid)
    );

    // ══════════════════════════════════════════════════════════
    // Instruction memory — flat BRAM stand-in (read-only here)
    // ══════════════════════════════════════════════════════════
    bram_mem #(.MEM_WORDS(IMEM_WORDS), .INIT_FILE(INIT_FILE)) u_imem (
        .i_clk(i_clk),
        .i_addr(if1_fetch_addr),
        .i_wdata(32'b0), .i_byte_en(4'b0), .i_we(1'b0),
        .i_en(if1_fetch_en),
        .o_rdata(imem_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // IF2 — deliver fetched word + PC to ID
    // ══════════════════════════════════════════════════════════
    penumbra2_if2_stage u_if2 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_pc(if1_pc), .i_next_pc(if1_next_pc), .i_valid(if1_valid),
        .i_ir(imem_rdata),
        .i_stall_in(fetch_stall),
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
        .o_branch_taken(), .o_branch_target()
    );

    /* verilator lint_on PINCONNECTEMPTY */
endmodule
