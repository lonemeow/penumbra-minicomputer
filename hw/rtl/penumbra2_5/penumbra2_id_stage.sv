// penumbra2_id_stage — Penumbra/2 instruction decode / issue stage.
//
// The integration point of the gen2 front half. Specified by the ID
// sections of doc/internals/penumbra2/{pipeline-stages,hazard-model,
// control-decode}.md. Each cycle it:
//   - decodes the IF2/ID instruction (penumbra2_decode),
//   - maps its architectural register references to physical scoreboard
//     entries (penumbra2_regmap),
//   - checks the scoreboard for a RAW hazard (penumbra2_scoreboard) and
//     stalls issue if a source is pending,
//   - drives the regfile read-index ports and selects the final ALU
//     operands (op_a/op_b) and the store-data value,
//   - latches the control bundle + operands + PC into the ID/EX
//     register under the back-pressure handshake.
//
// Boundary: the regfile is external (this stage drives its read indices
// and consumes the async read data; WB owns the write port). The
// scoreboard lives here. Its in-flight-writer inputs come from the
// stages downstream of EX — MEM and WB destinations and the divmul
// auxiliary (Rdh) arrive as ports — while the EX-stage writer is this
// stage's own registered phys_dst, fed back internally: the instruction
// "in EX" is exactly what ID latched last cycle.
//
// NZCV is not a scoreboard entry (it is forwarded in EX), so flag reads
// never appear as scoreboard sources and never stall here.

module penumbra2_id_stage
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── IF2/ID input: the instruction available to decode ────────
    input  logic [31:0]           i_ir,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,
    input  logic                  i_valid,          // 0 = bubble in
    input  logic                  i_btb_predicted,  // gen2.5: slot already BTB-predicted taken at fetch
    input  logic                  i_fault_pending,  // IF-stage fault
    input  logic [3:0]            i_fault_vec,
    input  logic [31:0]           i_fault_status,   // its composed payload; FAULT_NONE when none

    input  logic                  i_supervisor,     // SR.S

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic                  i_stall_in,       // EX cannot accept this cycle
    input  logic                  i_bubble,         // force a bubble this edge (taken-branch redirect / fault flush)
    input  logic                  i_fetch_busy,     // front end is waiting on the memory hierarchy (classifies an empty-fetch bubble)
    output logic                  o_local_stall,    // back-pressure (downstream-independent): the scoreboard interlock — also the perfctr hazard-stall cause
    output logic [BCAUSE_W-1:0]   o_bcause,         // stall cause carried by the ID/EX slot when it is a bubble

    // ── Regfile read interface (regfile is external) ─────────────
    output logic [SB_IDX_W-1:0]   o_rd_idx_a,
    output logic [SB_IDX_W-1:0]   o_rd_idx_b,
    input  logic [31:0]           i_rd_data_a,
    input  logic [31:0]           i_rd_data_b,

    // ── SPR-file source read (EPC/ESR have no regfile entry) ─────
    // A RDSPR of an SPR-file-backed SPR (EPC/ESR) reads its value here
    // instead of the regfile: o_spr_rd_sel selects it, i_spr_src_value
    // returns it (combinational, same cycle as the regfile read).
    output logic [3:0]            o_spr_rd_sel,
    input  logic [31:0]           i_spr_src_value,

    // ── Scoreboard: in-flight writers downstream of EX ───────────
    input  logic [SB_IDX_W-1:0]   i_mem_dst,
    input  logic                  i_mem_dst_en,
    input  logic [SB_IDX_W-1:0]   i_wb_dst,
    input  logic                  i_wb_dst_en,
    input  logic [SB_IDX_W-1:0]   i_aux_dst,        // divmul Rdh, wherever it is in flight
    input  logic                  i_aux_dst_en,

    // ── ID/EX register (to EX) ───────────────────────────────────
    output logic [OPC_W-1:0]      o_op_class,
    output logic [ALU_OP_W-1:0]   o_alu_op,
    output logic [1:0]            o_divmul_op,
    output logic [31:0]           o_op_a,
    output logic [31:0]           o_op_b,
    output logic [31:0]           o_store_data,
    output logic [3:0]            o_cond,
    output logic                  o_writes_flags,
    output logic                  o_reads_flags,
    output logic                  o_flag_only,
    output logic [MEM_OP_W-1:0]   o_mem_op,
    output logic [1:0]            o_mem_size,
    output logic                  o_sign_ext,
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    output logic [3:0]            o_spr_sel,
    output logic                  o_drain_commit,
    output logic                  o_post_commit_wait,
    output logic                  o_gpr_we,
    output logic                  o_spr_we,
    output logic                  o_flag_we,
    output logic                  o_is_trap,
    output logic [SB_IDX_W-1:0]   o_phys_dst,
    output logic [SB_IDX_W-1:0]   o_phys_dst_aux,
    output logic                  o_phys_dst_aux_en,  // aux dst present — drives the scoreboard aux
    output logic [31:0]           o_pc,
    output logic [31:0]           o_next_pc,
    output logic                  o_valid,
    output logic                  o_fault_pending,
    output logic [3:0]            o_fault_vec,
    output logic [31:0]           o_fault_status,

    // ── Branch-prediction confirmation tag (gen2.5: fetch-time BTB) ──
    output logic                  o_predicted_taken,  // BTB direction tag, registered into ID/EX for EX to confirm

    // ── GPR operand forwarding tags (gen2.5: to EX) ──────────────
    // The physical entry each operand reads and whether it is a forwardable
    // register source (not imm / PC / SPR-file). EX matches these against its
    // in-flight producers; they ride the ID/EX register alongside the operands.
    output logic [SB_IDX_W-1:0]   o_phys_src_a,
    output logic                  o_fwd_a_en,
    output logic [SB_IDX_W-1:0]   o_phys_src_b,
    output logic                  o_fwd_b_en,
    output logic                  o_issue             // a new instruction enters ID/EX this edge (EX operand-capture timing)
);

    // ── Decode (combinational) ───────────────────────────────────
    logic [OPC_W-1:0]    d_op_class;
    logic [3:0]          d_src_a_sel, d_src_b_sel, d_dst_sel, d_dst_aux_sel;
    logic                d_src_a_is_spr, d_src_a_en;
    logic                d_src_b_is_spr, d_src_b_en;
    logic                d_dst_is_spr, d_dst_en, d_dst_aux_en;
    logic [ALU_OP_W-1:0] d_alu_op;
    logic [1:0]          d_divmul_op;
    logic                d_a_from_pc, d_b_from_imm;
    logic                d_src_a_is_pc, d_src_b_is_pc;
    logic [31:0]         d_imm;
    logic [3:0]          d_cond;
    logic                d_writes_flags, d_reads_flags, d_flag_only;
    logic [MEM_OP_W-1:0] d_mem_op;
    logic [1:0]          d_mem_size;
    logic                d_sign_ext;
    logic [3:0]          d_sys_dev, d_sys_reg, d_spr_sel;
    logic                d_drain_commit, d_post_commit_wait;
    logic                d_gpr_we, d_spr_we, d_flag_we;
    logic                d_is_trap, d_illegal, d_priv_fault;
    logic [3:0]          d_fault_vec;

    penumbra2_decode u_decode (
        .i_ir(i_ir), .i_supervisor(i_supervisor),
        .o_op_class(d_op_class),
        .o_src_a_sel(d_src_a_sel), .o_src_a_is_spr(d_src_a_is_spr), .o_src_a_en(d_src_a_en),
        .o_src_b_sel(d_src_b_sel), .o_src_b_is_spr(d_src_b_is_spr), .o_src_b_en(d_src_b_en),
        .o_dst_sel(d_dst_sel),     .o_dst_is_spr(d_dst_is_spr),     .o_dst_en(d_dst_en),
        .o_dst_aux_sel(d_dst_aux_sel), .o_dst_aux_en(d_dst_aux_en),
        .o_alu_op(d_alu_op), .o_divmul_op(d_divmul_op),
        .o_a_from_pc(d_a_from_pc), .o_b_from_imm(d_b_from_imm),
        .o_src_a_is_pc(d_src_a_is_pc), .o_src_b_is_pc(d_src_b_is_pc),
        .o_imm(d_imm), .o_cond(d_cond),
        .o_writes_flags(d_writes_flags), .o_reads_flags(d_reads_flags), .o_flag_only(d_flag_only),
        .o_mem_op(d_mem_op), .o_mem_size(d_mem_size), .o_sign_ext(d_sign_ext),
        .o_sys_dev(d_sys_dev), .o_sys_reg(d_sys_reg), .o_spr_sel(d_spr_sel),
        .o_drain_commit(d_drain_commit), .o_post_commit_wait(d_post_commit_wait),
        .o_gpr_we(d_gpr_we), .o_spr_we(d_spr_we), .o_flag_we(d_flag_we),
        .o_is_trap(d_is_trap), .o_illegal(d_illegal), .o_priv_fault(d_priv_fault),
        .o_fault_vec(d_fault_vec)
    );

    // ── ISA→physical register mapping (combinational) ────────────
    logic [SB_IDX_W-1:0] phys_src_a, phys_src_b, phys_dst, phys_dst_aux;
    logic                phys_src_a_en, phys_src_b_en, phys_dst_en, phys_dst_aux_en;
    // cross_bank is informational: physical addressing already maps an
    // SPR-USP access to SB_USP, so no consumer here needs it.
    /* verilator lint_off UNUSEDSIGNAL */
    logic                cross_bank;
    /* verilator lint_on UNUSEDSIGNAL */

    penumbra2_regmap u_regmap (
        .i_supervisor(i_supervisor),
        .i_src_a_sel(d_src_a_sel), .i_src_a_is_spr(d_src_a_is_spr), .i_src_a_en(d_src_a_en),
        .i_src_b_sel(d_src_b_sel), .i_src_b_is_spr(d_src_b_is_spr), .i_src_b_en(d_src_b_en),
        .i_dst_sel(d_dst_sel),     .i_dst_is_spr(d_dst_is_spr),     .i_dst_en(d_dst_en),
        .i_dst_aux_sel(d_dst_aux_sel), .i_dst_aux_en(d_dst_aux_en),
        .o_src_a(phys_src_a), .o_src_a_en(phys_src_a_en),
        .o_src_b(phys_src_b), .o_src_b_en(phys_src_b_en),
        .o_dst(phys_dst),     .o_dst_en(phys_dst_en),
        .o_dst_aux(phys_dst_aux), .o_dst_aux_en(phys_dst_aux_en),
        .o_cross_bank(cross_bank)
    );

    // ── Regfile read drive (async read; data returns this cycle) ──
    assign o_rd_idx_a = phys_src_a;
    assign o_rd_idx_b = phys_src_b;

    // ── SPR-source-backed operand B (EPC/ESR/SCRn) ───────────────
    // ESR/EPC and the SCRn scratch SPRs are not regfile entries (the 16-entry
    // regfile returns 0 for their scoreboard indices) — their data comes from
    // the SPR file / scratch file. A RDSPR of any of them reads the SPR as
    // operand B (so ALU_PASS carries it), taking its value from
    // i_spr_src_value, which the spine muxes between the two files. USP reads
    // the regfile R14 bank normally (not SPR-source-backed).
    logic src_b_spr_file;
    assign src_b_spr_file = d_src_b_is_spr
                          & (d_src_b_sel == SPR_EPC | d_src_b_sel == SPR_ESR
                             | (d_src_b_sel >= SPR_SCR0 & d_src_b_sel <= SPR_SCR3));
    assign o_spr_rd_sel   = d_src_b_sel;

    // ── Operand select: ID produces the final ALU operands ───────
    // op_a/op_b are what EX feeds the ALU directly; store_data is the
    // raw port-B read (the value a store writes).
    // A register source reads as the live PC when the decoder flagged it
    // R15/PC (architecture.md: reading R15 yields the current PC). Folding that
    // in once, here at the port read, keeps the operand muxes below a plain
    // priority select and mirrors gen1, where the regfile itself returned PC
    // for an R15 read.
    logic [31:0] src_a_val, src_b_val;
    assign src_a_val = d_src_a_is_pc ? i_pc : i_rd_data_a;
    assign src_b_val = d_src_b_is_pc ? i_pc : i_rd_data_b;

    // op_a: d_a_from_pc is the branch-target path (PC + offset); otherwise the
    // source-A value. op_b is a priority select — an immediate or an SPR-file
    // source outranks the register read, so a store of PC (`stw pc, [b+off]`)
    // keeps its offset on op_b while the PC flows to store_data.
    logic [31:0] op_a_sel, op_b_sel, store_data_sel;
    assign op_a_sel = d_a_from_pc ? i_pc : src_a_val;
    always_comb begin
        if      (d_b_from_imm)   op_b_sel = d_imm;
        else if (src_b_spr_file) op_b_sel = i_spr_src_value;
        else                     op_b_sel = src_b_val;
    end
    assign store_data_sel = src_b_val;

    // ── Fault tag for the decoded instruction ────────────────────
    // An IF-stage fault outranks decode faults; SYSCALL/BREAK are
    // traps, not faults (they ride is_trap). A faulting or bubble slot
    // must be inert to the scoreboard.
    logic       insn_fault_pending;
    logic [3:0] insn_fault_vec;
    assign insn_fault_pending = i_fault_pending | d_illegal | d_priv_fault;
    assign insn_fault_vec     = i_fault_pending ? i_fault_vec : d_fault_vec;

    logic sb_eligible;   // this ID slot actually participates in the scoreboard
    assign sb_eligible = i_valid & ~insn_fault_pending;

    // ── Scoreboard ───────────────────────────────────────────────
    // EX writer = this stage's registered phys_dst (the insn now in
    // EX). ex_dst_en_r is its scoreboard-destination enable, registered
    // alongside the bundle and gated by validity / non-fault below.
    logic ex_dst_en_r;
    logic scoreboard_stall;             // conservative gen2 stall (kept for the subset assertion)
    logic [SB_NUM_ENTRIES-1:0] sb_valid;

    // The EX-stage writer (the slot ID latched last cycle), used by the
    // scoreboard and the load-use leg of the relaxed predicate.
    logic ex_writer_en;
    assign ex_writer_en = ex_dst_en_r & o_valid & ~o_fault_pending;

    // Whether that EX writer is a load/RDSYS — the producer kind whose value is
    // not yet at EX/MEM, so a consumer of it takes the 1-cycle load-use stall.
    // Consumed by the relaxed predicate (the TODO(human) below); the placeholder
    // does not read it yet.
    /* verilator lint_off UNUSEDSIGNAL */
    logic ex_loadlike;
    /* verilator lint_on UNUSEDSIGNAL */
    assign ex_loadlike = (o_op_class == OPC_LOAD) | (o_op_class == OPC_RDSYS);

    penumbra2_scoreboard u_scoreboard (
        .i_src_a(phys_src_a), .i_src_a_en(phys_src_a_en & sb_eligible),
        .i_src_b(phys_src_b), .i_src_b_en(phys_src_b_en & sb_eligible),
        .i_ex_dst(o_phys_dst),  .i_ex_dst_en(ex_writer_en),
        .i_mem_dst(i_mem_dst),  .i_mem_dst_en(i_mem_dst_en),
        .i_wb_dst(i_wb_dst),    .i_wb_dst_en(i_wb_dst_en),
        .i_aux_dst(i_aux_dst),  .i_aux_dst_en(i_aux_dst_en),
        .o_valid(sb_valid),
        .o_stall(scoreboard_stall)
    );

    // ── Operand forwardability (gen2.5: tags to EX) ──────────────
    // Whether each operand is a register read EX may override with a forwarded
    // value: not the PC / branch-target path, and not an SPR-file source
    // (EPC/ESR/SCRn — deferred). The port-B register feeds op_b for a reg/reg
    // op and store_data for a store, so its forwardability does not depend on
    // d_b_from_imm — EX routes it by op_class.
    logic fwd_a_en, fwd_b_en;
    assign fwd_a_en = phys_src_a_en & ~d_a_from_pc   & ~d_src_a_is_pc;
    assign fwd_b_en = phys_src_b_en & ~d_src_b_is_pc & ~src_b_spr_file;

    // Source-read participation — the same gate the scoreboard applies, so a
    // faulting or bubble slot never contributes a hazard stall.
    logic src_a_used, src_b_used;
    assign src_a_used = phys_src_a_en & sb_eligible;
    assign src_b_used = phys_src_b_en & sb_eligible;

    // ── Relaxed issue interlock (gen2.5 forwarding) ──────────────
    // gen2 stalls a reader whenever a source has any in-flight writer
    // (scoreboard_stall). gen2.5 forwards most of those, so a reader stalls
    // only when its source's youngest writer is one the forward network /
    // write-through cannot reach. Per the hazard-model "Relaxed stall
    // predicate", the residual cases keep a stall:
    //   - load-use: the source matches the EX-stage writer and that writer is
    //     a load/RDSYS (ex_writer_en & ex_loadlike, dst = o_phys_dst);
    //   - divmul aux: the source matches the in-flight Rdh (i_aux_dst /
    //     i_aux_dst_en) — aux forwarding is deferred;
    //   - SPR-file source: src_b_spr_file (EPC/ESR/SCRn) with a writer
    //     (~sb_valid) — SPR forwarding is deferred;
    //   - WRSYS value: its sysreg write datum bypasses the EX forward network
    //     (the spine reads idex_op_b), so its op_b cannot be forwarded.
    // Everything else a writer would have stalled (EX-stage ALU, any MEM- or
    // WB-stage producer) is reachable, so it must NOT stall here.
    //
    // Primitives available: src_a_used / src_b_used, sb_valid[<entry>],
    // phys_src_a / phys_src_b, o_phys_dst + ex_writer_en + ex_loadlike,
    // i_aux_dst + i_aux_dst_en, src_b_spr_file.
    // "Does either read source name this physical entry?" — the shared shape
    // of the load-use and divmul-aux legs, each gated by the fault/bubble-safe
    // participation bit so an inert slot never matches.
    //
    // Entry 0 (R0) is never a real producer: writes to it are discarded and it
    // reads as zero, so a destination of R0 must not stall a reader. This bites
    // the divmul-aux leg in particular — a divmul that discards its high half
    // targets Rdh=R0, making i_aux_dst=0, which would otherwise match every R0
    // reader. The scoreboard ties valid[R0]=1 for the same reason; this direct
    // match path needs the guard too (without it, hazard_stall can assert where
    // scoreboard_stall does not).
    function automatic logic src_reads(input logic [SB_IDX_W-1:0] entry);
        return (entry != '0)
            & ( (src_a_used & (phys_src_a == entry))
              | (src_b_used & (phys_src_b == entry)) );
    endfunction

    // Load-use: a reader of the EX-stage writer when that writer is a real
    // load/RDSYS — its value reaches the forward network one stage later, at
    // MEM/WB, so the reader takes one bubble. ex_writer_en qualifies the EX
    // slot as live (a bubble keeps a stale op_class, so ex_loadlike alone is
    // not enough).
    logic load_use_stall, divmul_aux_stall, spr_read_stall, wrsys_value_stall;
    assign load_use_stall   = ex_writer_en & ex_loadlike & src_reads(o_phys_dst);
    // Divmul aux: a reader of the in-flight Rdh — aux forwarding is deferred.
    assign divmul_aux_stall = i_aux_dst_en & src_reads(i_aux_dst);
    // SPR-file read (EPC/ESR/SCRn, operand B only) with a writer in flight —
    // SPR forwarding is deferred, so any writer keeps the conservative stall.
    assign spr_read_stall   = src_b_used & src_b_spr_file & ~sb_valid[phys_src_b];
    // WRSYS composes its sysreg write value from the registered ID/EX operand
    // in the spine (o_sys_wdata = idex_op_b), which bypasses the EX forward
    // network entirely — so that value cannot be forwarded. Keep the
    // conservative stall (like the SPR-file reads) so idex_op_b holds the
    // committed value by the time WRSYS drain-commits. Without this a WRSYS of
    // a just-computed value writes stale data — e.g. the TLB miss handler
    // installs a wrong PTE and re-faults forever.
    assign wrsys_value_stall = (d_op_class == OPC_WRSYS) & src_b_used & ~sb_valid[phys_src_b];

    logic hazard_stall;
    assign hazard_stall = load_use_stall | divmul_aux_stall | spr_read_stall | wrsys_value_stall;

    // ID's local stall — the issue interlock, downstream-independent (it does
    // not fold in i_stall_in). The spine ORs it with the downstream stalls to
    // form the back-pressure to IF, and surfaces it as the perfctr's hazard
    // cause. The pre-refactor o_stall was o_local_stall | i_stall_in.
    assign o_local_stall = i_valid & hazard_stall;

    // ── Issue / back-pressure control ────────────────────────────
    // can_issue : the ID instruction is eligible to advance into EX —
    //             a real, non-RAW-stalled slot. A fault-tagged insn is
    //             eligible too: it never scoreboard-stalls (sb_eligible
    //             gates its sources off) and faults at WB.
    // issue     : it actually advances into EX this edge — latch the
    //             decoded bundle + operands into ID/EX.
    // next_valid: the ID/EX valid bit after this edge.
    // (Back-pressure to IF is composed in the spine from o_local_stall and the
    // downstream stalls; i_stall_in here is that composed downstream stall.)
    logic can_issue, issue, next_valid;
    assign can_issue = i_valid & ~hazard_stall;
    // issue is high on every cycle a new slot is latched into ID/EX — including
    // back-to-back issues, so it is a per-cycle level, not a one-shot pulse. The
    // spine registers it into idex_first_cycle to seed EX's operand-capture latch.
    assign o_issue = issue;

    always_comb begin
        if (i_bubble) begin
            next_valid = 1'b0;          // flush the in-flight insn to a bubble
            issue      = 1'b0;
        end else if (i_stall_in) begin
            next_valid = o_valid;       // hold ID/EX unchanged
            issue      = 1'b0;
        end else begin
            next_valid = can_issue;     // advance: issue if eligible, else bubble
            issue      = can_issue;
        end
    end

    // ── ID/EX bubble cause ────────────────────────────────────────
    // Mirrors the next_valid priority above: the cause stamped on the ID/EX
    // slot whenever ID injects a bubble (a valid issue is charged to nothing).
    // ID is the head of the datapath, so it does not forward an upstream cause;
    // it classifies the two front-end bubble kinds here. A redirect kill
    // (i_bubble) and a cold/refill gap are front-end flush (FLUSH); an empty
    // fetch slot while the front end is memory-bound is IFETCH — the split is
    // by i_fetch_busy at the starved cycle, which is exactly the doc's
    // IFETCH-vs-FLUSH boundary (memory wait vs refill). A live slot that cannot
    // issue is a scoreboard interlock (HAZARD). An issuing slot is valid, so its
    // cause is moot (NONE).
    logic [BCAUSE_W-1:0] next_bcause;
    always_comb begin
        if      (i_bubble)         next_bcause = BCAUSE_FLUSH;
        else if (i_stall_in)       next_bcause = o_bcause;   // hold the carried cause
        else if (~i_valid)         next_bcause = i_fetch_busy ? BCAUSE_IFETCH : BCAUSE_FLUSH;
        else if (hazard_stall)     next_bcause = BCAUSE_HAZARD;
        else                       next_bcause = BCAUSE_NONE; // issuing: a valid slot, charged to nothing
    end

    // ── Branch-prediction confirmation tag (gen2.5) ──────────────
    // Prediction is fetch-time only: the BTB (core fork) predicts direct
    // branches and the i_btb_predicted tag rides the slot so EX can confirm the
    // already-steered direction. A direct branch the BTB missed, and every
    // indirect jump / return, resolves in EX — carried as predicted-not-taken,
    // so EX redirects on the misprediction. The tag is registered into ID/EX
    // below.

    // ── ID/EX register ───────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid  <= 1'b0;
            o_bcause <= BCAUSE_FLUSH;       // cold pipe: the fill bubbles are front-end
        end else begin
            o_valid  <= next_valid;
            o_bcause <= next_bcause;        // travels with the slot, valid or bubble
            if (issue) begin
                // A faulting slot is inert, so it must not report a real op
                // class downstream: an IF-faulted word's garbage decode could
                // otherwise read as BREAK/SYSCALL at the retire port and (e.g.)
                // trip the program-end pulse. Neutralise it to a benign ALU
                // class — the same reason o_is_trap is gated below.
                o_op_class         <= insn_fault_pending ? OPC_ALU : d_op_class;
                o_alu_op           <= d_alu_op;
                o_divmul_op        <= d_divmul_op;
                o_op_a             <= op_a_sel;
                o_op_b             <= op_b_sel;
                o_store_data       <= store_data_sel;
                o_cond             <= d_cond;
                o_predicted_taken  <= i_btb_predicted;
                o_phys_src_a       <= phys_src_a;
                o_fwd_a_en         <= fwd_a_en;
                o_phys_src_b       <= phys_src_b;
                o_fwd_b_en         <= fwd_b_en;
                o_writes_flags     <= d_writes_flags;
                o_reads_flags      <= d_reads_flags;
                o_flag_only        <= d_flag_only;
                o_mem_op           <= d_mem_op;
                o_mem_size         <= d_mem_size;
                o_sign_ext         <= d_sign_ext;
                o_sys_dev          <= d_sys_dev;
                o_sys_reg          <= d_sys_reg;
                o_spr_sel          <= d_spr_sel;
                o_drain_commit     <= d_drain_commit;
                o_post_commit_wait <= d_post_commit_wait;
                o_gpr_we           <= d_gpr_we;
                o_spr_we           <= d_spr_we;
                o_flag_we          <= d_flag_we;
                // Fault outranks trap in the exception priority, so a
                // faulting slot is never also a trap (an IF-faulted word
                // could otherwise decode as SYSCALL/BREAK).
                o_is_trap          <= d_is_trap & ~insn_fault_pending;
                o_phys_dst         <= phys_dst;
                o_phys_dst_aux     <= phys_dst_aux;
                o_phys_dst_aux_en  <= phys_dst_aux_en;
                o_pc               <= i_pc;
                o_next_pc          <= i_next_pc;
                o_fault_pending    <= insn_fault_pending;
                o_fault_vec        <= insn_fault_vec;
                // The carried payload is self-qualifying: an IF address fault
                // arrives with its composed status, and i_fault_status is
                // FAULT_NONE otherwise — so a decode fault raised here rides
                // FAULT_NONE with no mux.
                o_fault_status     <= i_fault_status;
                ex_dst_en_r        <= phys_dst_en;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // Non-structural preconditions this stage owns.
    // ══════════════════════════════════════════════════════════
    always_comb begin
        // Issue precondition: an instruction advances into EX only when
        // it is a real, non-hazard-stalled slot and we are neither
        // flushing it nor back-pressured. The gate is hazard_stall — the
        // relaxed interlock that actually blocks issue — not the raw
        // scoreboard_stall (gen2.5 forwarding lets a reader issue past a
        // scoreboard stall by design; the subset assertion below proves
        // hazard_stall never exceeds scoreboard_stall).
        assert (!issue || (i_valid && !hazard_stall && !i_bubble && !i_stall_in))
            else $error("penumbra2_id_stage: issue without a clean issue precondition");
    end

    // A committed ID/EX slot is never both faulting and trapping —
    // fault outranks trap, and is_trap is suppressed when the slot
    // faults.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_valid |-> !(o_fault_pending && o_is_trap))
        else $error("penumbra2_id_stage: ID/EX slot is both faulting and trapping");

    // Every ID/EX bubble carries a real cause — the commit point charges the
    // non-retiring cycle to it, so a NONE tag on a bubble would leave a cycle
    // unaccounted.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_valid || o_bcause != BCAUSE_NONE)
        else $error("penumbra2_id_stage: ID/EX bubble with no stall cause");

    // Forwarding only ever removes stalls: the relaxed interlock must be a
    // subset of the conservative scoreboard stall. If hazard_stall asserts
    // where scoreboard_stall does not, the relaxed predicate is stalling a
    // source with no in-flight writer — an over-stall logic error. It cannot
    // catch over-relaxation (issuing when it should stall); the gen2.5
    // result-equivalence suite is the net for that.
    assert property (@(posedge i_clk) disable iff (i_rst)
        hazard_stall |-> scoreboard_stall)
        else $error("penumbra2_id_stage: relaxed interlock stalls outside the scoreboard stall");

endmodule
