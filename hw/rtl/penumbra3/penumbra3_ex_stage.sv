// penumbra3_ex_stage -- Penumbra/3 execute stage.
//
// The integration point of the datapath spine's middle, mirroring how
// penumbra3_id_stage assembles the front half. Each cycle it:
//   - forwards the two register operands from the youngest in-flight
//     producer (the operand-forwarding network) and selects the final ALU
//     inputs,
//   - feeds those operands to the combinational ALU (penumbra3_alu),
//   - forwards the youngest in-flight NZCV (penumbra3_flag_bypass) and hands
//     its carry to the ALU for ADC/SBC,
//   - resolves control transfers -- cond_eval against the forwarded NZCV,
//     branch target from the ALU (PC + offset), JMP target from op_a -- and
//     drives the front-end redirect,
//   - launches a divmul on the shared peer unit and holds until it completes,
//   - sequences drain-commit ops (ERET / WRSYS / EI / DI), and
//   - latches the EX/MEM1 register under the back-pressure handshake.
//
// Forwarding model. EX owns operand forwarding. The scoreboard tracks only the
// non-forwardable producers (loads, divmul, sysreg reads); every other
// dependency is supplied here from an in-flight result, so those producers
// carry no scoreboard bit. The issue-release broadcasts ID matches against are
// driven by the spine from the back end, where loads and sysreg reads resolve,
// not from here.
//
// Forwarding boundaries. The consumer in EX reads operands captured into the
// ID/EX register one cycle earlier, so a producer's result reaches it from
// one of three places, youngest first:
//   - EX/MEM1 (the instruction this stage latched last cycle) -- self-feedback
//     from o_mem1_*, the same shape penumbra3_flag_bypass uses for its MEM1 leg;
//   - MEM1/MEM2 (two ahead) -- this is also the load-use bypass path;
//   - MEM2/WB (three ahead).
// A write-first regfile closes the four-deep distance. A load/sysreg read is
// NOT forwardable from EX/MEM1 (it has only computed its address there; its
// value resolves in the back end), so the self-feedback leg excludes them --
// those are exactly the producers the scoreboard tracks.

module penumbra3_ex_stage
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // -- ID/EX input: the issued instruction ----------------------
    input  ctrl_bundle_t          i_bundle,
    input  logic [31:0]           i_op_a,           // ID-selected operand A (PC or src-a read)
    input  logic [31:0]           i_op_b,           // ID-selected operand B (imm / SPR / src-b read)
    input  logic [31:0]           i_store_data,     // src-b read, for a store
    input  logic [SB_IDX_W-1:0]   i_phys_dst,
    input  logic                  i_phys_dst_we,    // occupies a scoreboard entry / regfile write
    input  logic [SB_IDX_W-1:0]   i_phys_dst_aux,
    input  logic                  i_phys_dst_aux_we,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,        // PC + 4: BL/JALR link value
    input  logic                  i_valid,          // 0 = bubble in
    input  logic                  i_fault_pending,
    input  logic [3:0]            i_fault_vec,
    input  logic [31:0]           i_fault_status,   // carried payload; FAULT_NONE when none
    input  bcause_e               i_bcause,         // cause carried by an incoming ID/EX bubble

    // -- Operand-forward source qualifiers (from ID/EX) -----------
    // The physical source indices plus a bit per operand marking it a
    // forwardable register read (not PC / immediate / SPR-file). EX matches
    // the indices against the in-flight producers below.
    input  logic [SB_IDX_W-1:0]   i_phys_src_a,
    input  logic                  i_src_a_fwdable,
    input  logic [SB_IDX_W-1:0]   i_phys_src_b,
    input  logic                  i_src_b_fwdable,

    // -- Back-end forward sources (driven by the spine) -----------
    // The resolved results sitting in the MEM1/MEM2 and MEM2/WB registers.
    // A held or unresolved slot presents valid=0.
    input  logic [31:0]           i_mem2_result,
    input  logic [SB_IDX_W-1:0]   i_mem2_phys_dst,
    input  logic                  i_mem2_dst_we,
    input  logic                  i_mem2_valid,
    input  logic [31:0]           i_wb_result,
    input  logic [SB_IDX_W-1:0]   i_wb_phys_dst,
    input  logic                  i_wb_dst_we,
    input  logic                  i_wb_valid,

    // -- Flag bypass external sources (MEM1 producer is internal) -
    input  logic [3:0]            i_sr_flags,        // committed SR NZCV
    input  logic [3:0]            i_mem2_flags,      // MEM2 in-flight producer
    input  logic                  i_mem2_writes_flags,
    input  logic [3:0]            i_wb_flags,        // WB in-flight producer
    input  logic                  i_wb_writes_flags,

    // -- Committed SR word (RDSPR SR returns S/I from here + bypassed NZCV) --
    // Only [31:4] (S/I/reserved) is consumed; [3:0] (committed NZCV) is
    // replaced by the flag bypass, so the low nibble is intentionally unused.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0]           i_sr_committed,
    /* verilator lint_on UNUSEDSIGNAL */

    // -- Interrupt injection (from the interrupt unit) ------------
    // An eligible interrupt is taken by tagging this instruction as a
    // synthetic fault here, so it rides the precise-fault path. Its own
    // exception outranks the interrupt, leaving the IRQ pending to retake.
    input  logic                  i_irq_inject,
    input  logic [3:0]            i_irq_vec,

    // -- Pipeline handshake ---------------------------------------
    input  logic                  i_stall_in,        // MEM1 cannot accept this cycle
    input  logic                  i_wb_active,       // WB holds a live (non-bubble) insn
    input  logic                  i_bubble,          // force this insn to a bubble (fault flush)
    output logic                  o_local_stall,     // back-pressure to ID (downstream-independent)
    output logic                  o_dc_commit,       // drain-commit insn commits this cycle
    output logic                  o_funit_stall,     // stall cause: waiting on divmul (perfctr)

    // -- Branch resolution (to IF: redirect + flush IF1/IF2/ID) ---
    output logic                  o_branch_taken,
    output logic [31:0]           o_branch_target,

    // -- EX/MEM1 register (to MEM1) -------------------------------
    // The control bundle is carried unchanged; the resolved datapath travels in
    // the payload struct. store_data rides its own wire -- MEM2 consumes it, so
    // it never reaches WB and is not part of the through-payload.
    output ctrl_bundle_t          o_mem1_bundle,
    output dpath_payload_t        o_mem1_payload,
    output logic [31:0]           o_mem1_store_data,
    output logic                  o_mem1_valid,
    output bcause_e               o_mem1_bcause
);

    // ================================================================
    // Operand forwarding network
    // ================================================================
    // Three in-flight producer slots, youngest (index 0) first:
    //   [0] EX/MEM1  -- this stage's own registered result (self-feedback).
    //                   Loads and sysreg reads have no value here yet (only an
    //                   address), so they are excluded -- those are exactly the
    //                   scoreboard-tracked producers a dependent stalls on.
    //   [1] MEM1/MEM2 -- the load-use bypass distance; resolved back-end result.
    //   [2] MEM2/WB   -- three ahead; resolved back-end result.
    // A write-first regfile covers the four-deep distance, so no fourth slot.
    localparam int FWD_N = 3;

    logic [SB_IDX_W-1:0] fwd_dst   [FWD_N];
    logic                fwd_valid [FWD_N];
    logic [31:0]         fwd_data  [FWD_N];

    // [0] EX/MEM1 self-feedback. mem1_result_ready gates off the producers
    // whose value is not yet computed at MEM1 (loads, sysreg reads).
    logic mem1_result_ready;
    assign mem1_result_ready = (o_mem1_bundle.op_class != OPC_LOAD)
                             & (o_mem1_bundle.op_class != OPC_RDSYS);
    assign fwd_dst[0]   = o_mem1_payload.phys_dst;
    assign fwd_valid[0] = o_mem1_valid & o_mem1_payload.phys_dst_we
                        & ~o_mem1_payload.fault_pending & mem1_result_ready;
    assign fwd_data[0]  = o_mem1_payload.value;

    // [1] MEM1/MEM2 and [2] MEM2/WB: resolved back-end results from the spine.
    assign fwd_dst[1]   = i_mem2_phys_dst;
    assign fwd_valid[1] = i_mem2_valid & i_mem2_dst_we;
    assign fwd_data[1]  = i_mem2_result;

    assign fwd_dst[2]   = i_wb_phys_dst;
    assign fwd_valid[2] = i_wb_valid & i_wb_dst_we;
    assign fwd_data[2]  = i_wb_result;

    // A matched forward: hit + the value to substitute for the regfile read.
    typedef struct packed {
        logic        hit;
        logic [31:0] val;
    } fwd_result_t;

    // Pick the youngest in-flight producer whose destination matches src_idx.
    // Reads the module-level fwd_* arrays above.
    function automatic fwd_result_t fwd_lookup(input logic [SB_IDX_W-1:0] src_idx);
        // Default: no producer matches, so EX keeps the ID/EX-registered read.
        fwd_lookup = '{hit: 1'b0, val: 32'b0};
        for (int k = 0; k < FWD_N; k++) begin
            if (fwd_valid[k] && fwd_dst[k] == src_idx) begin
                fwd_lookup = '{hit: 1'b1, val: fwd_data[k]};
                break;
            end
        end
    endfunction

    // Apply forwarding to the two register reads, then re-select the operands.
    // op_b and store_data both look up src-b, but qualify differently. A
    // forward overrides op_b only when op_b is itself a forwardable register
    // read (i_src_b_fwdable) -- an immediate / PC / SPR-file operand B is never
    // touched. A store's data, though, always comes from src-b even when op_b
    // carries the immediate EA offset (i_src_b_fwdable then low), so it has its
    // own qualifier: src-b is a plain GPR for every store.
    fwd_result_t fa, fb;
    always_comb begin
        fa = fwd_lookup(i_phys_src_a);
        fb = fwd_lookup(i_phys_src_b);
    end

    logic store_fwdable;
    assign store_fwdable = (i_bundle.mem_op == MEM_STORE)
                         & i_bundle.src_b_en & ~i_bundle.src_b_is_pc;

    logic [31:0] op_a, op_b, store_data;
    assign op_a       = (i_src_a_fwdable & fa.hit) ? fa.val : i_op_a;
    assign op_b       = (i_src_b_fwdable & fb.hit) ? fb.val : i_op_b;
    assign store_data = (store_fwdable   & fb.hit) ? fb.val : i_store_data;

    // ================================================================
    // Flag bypass: youngest in-flight NZCV for the EX reader
    // ================================================================
    // The MEM1 producer is this stage's own registered output. A bubble or
    // faulting slot is not a committed flag writer, so it is gated off. The
    // carry bit feeds the ALU (ADC/SBC); all four bits feed the branch
    // condition below.
    logic [3:0] fwd_flags;
    logic       mem1_writes_flags;
    assign mem1_writes_flags = o_mem1_valid & o_mem1_bundle.flags_updater & ~o_mem1_payload.fault_pending;

    penumbra3_flag_bypass u_flag_bypass (
        .i_sr_flags          (i_sr_flags),
        .i_mem1_flags        (o_mem1_payload.flags),
        .i_mem1_writes_flags (mem1_writes_flags),
        .i_mem2_flags        (i_mem2_flags),
        .i_mem2_writes_flags (i_mem2_writes_flags),
        .i_wb_flags          (i_wb_flags),
        .i_wb_writes_flags   (i_wb_writes_flags),
        .o_flags             (fwd_flags)
    );

    // ================================================================
    // ALU compute
    // ================================================================
    // NZCV bundle packs as SR[3:0]: N=0 Z=1 C=2 V=3. ADC/SBC take the
    // forwarded carry (bundle bit 2); other ops ignore the carry-in.
    logic        carry_in;
    logic [31:0] alu_result;
    logic        alu_n, alu_z, alu_c, alu_v;
    logic [3:0]  alu_flags;

    assign carry_in = fwd_flags[2];

    penumbra3_alu u_alu (
        .i_a        (op_a),
        .i_b        (op_b),
        .i_op       (i_bundle.alu_op),
        .i_carry_in (carry_in),
        .o_result   (alu_result),
        .o_flag_z   (alu_z),
        .o_flag_n   (alu_n),
        .o_flag_c   (alu_c),
        .o_flag_v   (alu_v)
    );

    assign alu_flags = {alu_v, alu_c, alu_z, alu_n};

    // ================================================================
    // Result select
    // ================================================================
    // A linking control transfer (BL / JALR) writes PC+4 to its link reg, so
    // its result is next_pc. RDSPR SR composes the live status word: committed
    // S/I from i_sr_committed, NZCV from the flag bypass (so it reads like any
    // other flag reader). Everything else takes the ALU result (also the EA
    // for memory ops). divmul overrides this at the register latch below.
    logic is_link, is_rdspr_sr, is_divmul;
    assign is_divmul   = (i_bundle.op_class == OPC_DIVMUL);
    assign is_link     = (i_bundle.op_class == OPC_BRANCH || i_bundle.op_class == OPC_JMP)
                       & i_bundle.dst_we;
    assign is_rdspr_sr = (i_bundle.op_class == OPC_RDSPR) & (i_bundle.spr_sel == SPR_SR);

    logic [31:0] result_value;
    always_comb begin
        if      (is_link)     result_value = i_next_pc;
        else if (is_rdspr_sr) result_value = {i_sr_committed[31:4], fwd_flags};
        else                  result_value = alu_result;
    end

    // ================================================================
    // Branch resolution
    // ================================================================
    // EX resolves every control transfer. The condition is evaluated against
    // the forwarded NZCV by the shared cond_eval (the cond encoding is ISA).
    // For Format B the ALU already computed the target (PC + offset); JMP/JALR
    // carry the target in op_a (forwarded).
    logic cond_taken;
    cond_eval u_cond_eval (
        .i_flag_n (fwd_flags[0]),
        .i_flag_z (fwd_flags[1]),
        .i_flag_c (fwd_flags[2]),
        .i_flag_v (fwd_flags[3]),
        .i_cond   (i_bundle.cond),
        .o_taken  (cond_taken)
    );

    logic        branch_redirect;
    logic [31:0] branch_target;
    always_comb begin
        case (i_bundle.op_class)
            OPC_JMP: begin
                branch_redirect = 1'b1;
                branch_target   = op_a;
            end
            OPC_BRANCH: begin
                branch_redirect = cond_taken;
                branch_target   = alu_result;
            end
            default: begin
                branch_redirect = 1'b0;
                branch_target   = 32'b0;
            end
        endcase
    end

    // A taken redirect counts only for a real, non-faulting, un-flushed branch
    // slot; a bubble or a flushed slot must not steer the front end.
    assign o_branch_taken  = branch_redirect & i_valid & ~i_fault_pending & ~i_bubble;
    assign o_branch_target = branch_target;

    // ================================================================
    // Drain-commit FSM
    // ================================================================
    // ERET/WRSYS/EI/DI must order their commit against older in-flight
    // instructions (which observe pre-commit SR) and younger ones (post-commit
    // state). The insn holds in EX -- it never advances into MEM1/WB -- while
    // MEM1/MEM2/WB drain; once drained it commits (o_dc_commit pulse) and, for
    // WRSYS, EX holds upstream one extra cycle (post_wait_q) so the device
    // latches before the next insn can observe it.
    //
    // MEM1 occupancy is EX's own EX/MEM1 register (o_mem1_valid); only WB needs
    // a separate occupancy feedback (i_wb_active). MEM2 drains as the EX/MEM1
    // bubble propagates, so the in-flight back end empties from o_mem1_valid
    // and i_wb_active together.
    logic dc_here;          // a drain-commit insn occupies EX
    logic drained;          // the back end holds no live instruction
    logic dc_commit_now;
    logic post_wait_q;      // the WRSYS post-commit extra-hold cycle

    assign dc_here       = i_valid & i_bundle.drain_commit & ~i_fault_pending & ~i_bubble;
    assign drained       = ~o_mem1_valid & ~i_wb_active;
    assign dc_commit_now = dc_here & drained & ~post_wait_q;
    assign o_dc_commit   = dc_commit_now;

    always_ff @(posedge i_clk) begin
        if (i_rst) post_wait_q <= 1'b0;
        else       post_wait_q <= dc_commit_now & i_bundle.commit_wait;
    end

    // ================================================================
    // divmul peer unit
    // ================================================================
    // MUL/MULU/DIV/DIVU run on the shared multi-cycle divmul unit. EX holds
    // i_start high while the divmul is the EX insn -- the unit edge-detects it,
    // so it triggers once -- and stalls while o_busy. The cycle o_busy falls,
    // both halves are valid to latch (low -> result, high -> result_aux). A
    // DIV0 never iterates: o_fault pulses on the start cycle with o_busy low,
    // completing in one cycle carrying VEC_ARITH.
    //
    // The unit's 5-bit op is the encoding divmul.sv decodes, kept local here
    // (as it is there) rather than in the shared ISA package. divmul_op
    // (ISA op[1:0]) selects it.
    localparam logic [4:0] DM_OP_MUL  = 5'b01101;
    localparam logic [4:0] DM_OP_MULU = 5'b01110;
    localparam logic [4:0] DM_OP_DIV  = 5'b01111;
    localparam logic [4:0] DM_OP_DIVU = 5'b10000;

    logic        divmul_in_ex;   // a valid divmul occupies EX (the level)
    logic        dm_started;     // the divmul in EX has already been launched
    logic [4:0]  dm_op;
    logic        dm_start, dm_busy, dm_fault, dm_z, dm_n;
    logic [31:0] dm_lo, dm_hi;

    // Declared here because the divmul launch FF references `advance`; both are
    // assigned in the handshake block below.
    logic        advance, next_valid;
    bcause_e     next_bcause;

    assign divmul_in_ex = is_divmul & i_valid & ~i_fault_pending & ~i_bubble;

    always_comb begin
        case (i_bundle.divmul_op)
            2'b00:   dm_op = DM_OP_MUL;
            2'b01:   dm_op = DM_OP_MULU;
            2'b10:   dm_op = DM_OP_DIV;
            default: dm_op = DM_OP_DIVU;
        endcase
    end

    // Pulse start on a divmul's first EX cycle: dm_started latches once we
    // launch and holds until the instruction advances out of EX (the next
    // divmul re-arms). Holding until advance -- not until the unit goes idle --
    // keeps a finished-but-back-pressured divmul from re-launching.
    assign dm_start = divmul_in_ex & ~dm_started;

    always_ff @(posedge i_clk) begin
        if (i_rst)         dm_started <= 1'b0;
        else if (advance)  dm_started <= 1'b0;   // left EX -> re-arm
        else if (dm_start) dm_started <= 1'b1;   // launched this divmul
    end

    divmul u_divmul (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_a         (op_a),
        .i_b         (op_b),
        .i_op        (dm_op),
        .i_start     (dm_start),
        .o_busy      (dm_busy),
        .o_fault     (dm_fault),
        .o_result_lo (dm_lo),
        .o_result_hi (dm_hi),
        .o_flag_z    (dm_z),
        .o_flag_n    (dm_n)
    );

    // While the unit is busy EX holds the insn; it advances the cycle busy
    // clears (with results, or a DIV0 fault). The hold tracks the level
    // (divmul-in-EX), not the launch pulse.
    logic dm_stall;
    assign dm_stall      = divmul_in_ex & dm_busy;
    assign o_funit_stall = dm_stall;

    // ================================================================
    // Issue / back-pressure control
    // ================================================================
    // EX accepts a new ID/EX instruction every cycle except when it is
    // back-pressured (MEM1 via i_stall_in), sequencing a drain-commit, or
    // waiting on the divmul unit. i_bubble (the fault flush) forces the
    // in-flight slot to a bubble and wins over everything.
    always_comb begin
        if (i_bubble) begin
            next_valid = 1'b0;          // flush wins
            advance    = 1'b0;
        end else if (post_wait_q) begin
            next_valid = 1'b0;          // WRSYS already committed; leave as a bubble
            advance    = 1'b0;
        end else if (dc_here) begin
            // Drain-commit insn: never advances into MEM1/WB. Inject a bubble so
            // the back end drains -- but hold EX/MEM1 while MEM1 is mid-access
            // (i_stall_in) so its live work is not clobbered.
            advance = 1'b0;
            if (i_stall_in) next_valid = o_mem1_valid;
            else            next_valid = 1'b0;
        end else if (dm_stall) begin
            // divmul busy: hold the insn in EX. The held EX/MEM1 slot drains to
            // a bubble -- except when MEM1 is back-pressuring (i_stall_in): the
            // slot is then a live instruction MEM1 has not accepted yet.
            advance = 1'b0;
            if (i_stall_in) next_valid = o_mem1_valid;
            else            next_valid = 1'b0;
        end else if (i_stall_in) begin
            next_valid = o_mem1_valid;  // hold EX/MEM1 unchanged
            advance    = 1'b0;
        end else begin
            next_valid = i_valid;       // advance: bubble in if i_valid=0
            advance    = i_valid;
        end
    end

    // -- EX local back-pressure (downstream-independent) ----------
    // EX holds upstream for its own reasons: sequencing a drain-commit (held
    // until the back end drains; WRSYS holds one extra cycle for the device
    // latch) or waiting on the divmul unit. A fault flush (i_bubble) and the
    // post-commit-wait cycle release upstream. The spine ORs this with the
    // downstream stalls.
    logic ex_local_stall;
    always_comb begin
        if      (i_bubble)    ex_local_stall = 1'b0;
        else if (post_wait_q) ex_local_stall = 1'b0;
        else if (dc_here)     ex_local_stall = drained ? i_bundle.commit_wait : 1'b1;
        else if (dm_stall)    ex_local_stall = 1'b1;
        else                  ex_local_stall = 1'b0;
    end
    assign o_local_stall = ex_local_stall;

    // -- EX/MEM1 bubble cause -------------------------------------
    // Mirrors the next_valid priority. A fault flush, the WRSYS post-commit
    // hold, and a drain are front-end serialization (FLUSH); a divmul busy-wait
    // is the execution unit (FUNIT). Under MEM1 back-pressure the carried cause
    // persists; on a clean advance it forwards the incoming ID/EX cause.
    always_comb begin
        if      (i_bubble)    next_bcause = BCAUSE_FLUSH;
        else if (post_wait_q) next_bcause = BCAUSE_FLUSH;
        else if (dc_here)     next_bcause = i_stall_in ? o_mem1_bcause : BCAUSE_FLUSH;
        else if (dm_stall)    next_bcause = i_stall_in ? o_mem1_bcause : BCAUSE_FUNIT;
        else if (i_stall_in)  next_bcause = o_mem1_bcause;
        else                  next_bcause = i_bcause;
    end

    // ================================================================
    // EX/MEM1 register
    // ================================================================
    // Assemble the datapath payload, latched as a unit on advance. divmul drives
    // both writeback halves and N/Z (C=V=0); every other op uses the ALU result
    // and flags -- and a divmul only advances once its results are valid. EX
    // raises a DIV0 (VEC_ARITH) and a software trap (SYSCALL/BREAK), and is where
    // an eligible interrupt is injected as a synthetic fault; the slot's own
    // exception outranks the interrupt.
    dpath_payload_t mem1_payload_d;
    always_comb begin
        mem1_payload_d.value           = is_divmul ? dm_lo : result_value;
        mem1_payload_d.value_aux       = is_divmul ? dm_hi : 32'b0;
        mem1_payload_d.flags           = is_divmul ? {2'b00, dm_z, dm_n} : alu_flags;
        mem1_payload_d.phys_dst        = i_phys_dst;
        mem1_payload_d.phys_dst_we     = i_phys_dst_we;
        mem1_payload_d.phys_dst_aux    = i_phys_dst_aux;
        mem1_payload_d.phys_dst_aux_we = i_phys_dst_aux_we;
        mem1_payload_d.pc              = i_pc;
        mem1_payload_d.fault_pending   = i_fault_pending | (is_divmul & dm_fault)
                                       | i_bundle.is_trap | i_irq_inject;
        mem1_payload_d.fault_vec       = (is_divmul & dm_fault)               ? VEC_ARITH
                                       : (i_fault_pending | i_bundle.is_trap) ? i_fault_vec
                                       : i_irq_inject                         ? i_irq_vec
                                       :                                        i_fault_vec;
        mem1_payload_d.fault_vaddr     = i_pc;   // EX-born faults report the PC; MEM overwrites for EA faults
        mem1_payload_d.fault_status    = i_fault_status;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_mem1_valid  <= 1'b0;
            o_mem1_bcause <= BCAUSE_FLUSH;     // cold pipe: the fill bubbles are front-end
        end else begin
            o_mem1_valid  <= next_valid;
            o_mem1_bcause <= next_bcause;
            if (advance) begin
                o_mem1_bundle     <= i_bundle;
                o_mem1_payload    <= mem1_payload_d;
                o_mem1_store_data <= store_data;
            end
        end
    end

    // ================================================================
    // Assertions -- sim-only (Verilator --assert); stripped at synth.
    // ================================================================
    // Advance precondition: the EX/MEM1 register latches a real instruction
    // only on a clean accept.
    always_comb
        assert (!advance || (i_valid && !i_bubble && !i_stall_in))
            else $error("penumbra3_ex_stage: advance without a clean precondition");

    // A fault-commit flush always lands as a bubble in EX/MEM1.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_bubble |=> !o_mem1_valid)
        else $error("penumbra3_ex_stage: i_bubble did not flush the EX/MEM1 slot");

    // Back-pressure holds the EX/MEM1 slot intact -- no lost or duplicated insn.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble) |=> $stable(o_mem1_valid))
        else $error("penumbra3_ex_stage: back-pressure changed o_mem1_valid");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble && o_mem1_valid) |=> $stable(o_mem1_payload.phys_dst))
        else $error("penumbra3_ex_stage: back-pressure swapped the held EX/MEM1 slot");

    // EX steers the front end only for an actual control-transfer instruction.
    always_comb
        assert (!o_branch_taken ||
                i_bundle.op_class == OPC_BRANCH || i_bundle.op_class == OPC_JMP)
            else $error("penumbra3_ex_stage: branch redirect on a non-branch op_class");

    // A drain-commit instruction never advances into MEM1/WB (it would
    // double-commit: once via o_dc_commit, once at WB).
    always_comb
        assert (!(dc_here && advance))
            else $error("penumbra3_ex_stage: drain-commit insn advanced into EX/MEM1");

    // The post-commit hold never re-fires the commit and lasts exactly one cycle.
    always_comb
        assert (!(post_wait_q && o_dc_commit))
            else $error("penumbra3_ex_stage: drain-commit re-fired during post-commit hold");
    assert property (@(posedge i_clk) disable iff (i_rst)
        post_wait_q |=> !post_wait_q)
        else $error("penumbra3_ex_stage: post-commit hold exceeded one cycle");

    // A divmul never advances into EX/MEM1 while the unit is still busy.
    always_comb
        assert (!(dm_stall && advance))
            else $error("penumbra3_ex_stage: divmul advanced into EX/MEM1 while busy");

    // A forward must never source the youngest leg from a producer whose value
    // is not yet computed at MEM1 (a load / sysreg read) -- that leg carries an
    // address, not the result.
    always_comb
        assert (!(fwd_valid[0] && (o_mem1_bundle.op_class == OPC_LOAD ||
                                   o_mem1_bundle.op_class == OPC_RDSYS)))
            else $error("penumbra3_ex_stage: forwarded an unresolved MEM1 producer");

endmodule
