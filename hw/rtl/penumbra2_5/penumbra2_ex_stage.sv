// penumbra2_ex_stage — Penumbra/2 execute stage.
//
// The integration point of the gen2 datapath spine, mirroring how
// penumbra2_id_stage assembles the front half. Specified by the EX
// sections of doc/internals/penumbra2/{pipeline-stages,control-decode,
// hazard-model}.md. Each cycle it:
//   - feeds the ID/EX operands to the combinational ALU (penumbra2_alu),
//   - forwards the youngest in-flight NZCV (penumbra2_flag_bypass) and
//     hands its carry bit to the ALU for ADC/SBC,
//   - resolves control transfers — cond_eval against the forwarded NZCV,
//     branch target from the ALU (PC + offset), JMP target from op_a —
//     and drives the front-end redirect,
//   - narrows the control bundle to the ctrl_mem + ctrl_wb subsets and
//     latches the EX/MEM register under the back-pressure handshake.
//
// Drain-commit instructions (ERET / WRSYS / WRSPR-SR / EI / DI) are
// sequenced here too: EX holds them, drains MEM/WB, and pulses o_dc_commit
// at the commit point (the architectural SR/PC/sysreg writes are routed by
// integration from the held ID/EX bundle — EX owns the *when*, not the
// *what*). MUL/MULU/DIV/DIVU run on the divmul peer unit, holding EX
// until it reports its two-half result (or a DIV0 fault → VEC_ARITH).
//
// Flag bypass boundary: the youngest producer ahead of the EX reader is
// the EX/MEM-stage instruction, which is exactly what this stage latched
// last cycle — so the bypass's MEM source feeds back from this stage's
// own registered o_flag_value/o_flag_we (the same self-feedback shape the
// ID scoreboard uses for its EX writer). The WB producer and the
// committed SR arrive as ports from downstream.

module penumbra2_ex_stage
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── ID/EX input: the issued instruction ──────────────────────
    input  logic [OPC_W-1:0]      i_op_class,
    input  logic [ALU_OP_W-1:0]   i_alu_op,
    input  logic [1:0]            i_divmul_op,      // divmul variant (ISA op[1:0])
    input  logic [31:0]           i_op_a,
    input  logic [31:0]           i_op_b,
    input  logic [31:0]           i_store_data,
    input  logic [3:0]            i_cond,           // branch condition (Format B)
    input  logic                  i_predicted_taken,// ID's direction guess (BTFN or RAS)
    input  logic [31:0]           i_predicted_target,// the predicted target (RAS-predicted returns; verified here)
    input  logic [MEM_OP_W-1:0]   i_mem_op,
    input  logic [1:0]            i_mem_size,
    input  logic                  i_sign_ext,
    input  logic [3:0]            i_sys_dev,
    input  logic [3:0]            i_sys_reg,
    input  logic [3:0]            i_spr_sel,
    input  logic                  i_drain_commit,     // ERET/WRSYS/WRSPR-SR/EI/DI
    input  logic                  i_post_commit_wait, // WRSYS: hold 1 cyc for the device latch
    input  logic                  i_gpr_we,
    input  logic                  i_spr_we,
    input  logic                  i_flag_we,
    input  logic [SB_IDX_W-1:0]   i_phys_dst,
    input  logic [SB_IDX_W-1:0]   i_phys_dst_aux,
    input  logic                  i_phys_dst_aux_en,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,        // PC + 4: BL/JALR link value
    input  logic                  i_is_trap,        // SYSCALL/BREAK: raise the trap here
    input  logic                  i_valid,          // 0 = bubble in
    input  logic                  i_fault_pending,
    input  logic [3:0]            i_fault_vec,
    input  logic [31:0]           i_fault_status,   // carried payload; FAULT_NONE when none

    // ── Interrupt injection (from the interrupt unit) ────────────
    // An eligible interrupt is taken by tagging this instruction as a synthetic
    // fault here, so it rides the precise-fault path: its access is suppressed
    // in MEM (~i_fault_pending), its register write is dropped at WB, and the
    // save-state captures EPC ← its own PC after the older in-flight slots
    // commit ahead of it. The instruction is inert and re-executes after ERET.
    // Its own exception (upstream fault / DIV0 / trap) outranks the interrupt,
    // leaving the IRQ pending to retake after that handler.
    input  logic                  i_irq_inject,
    input  logic [3:0]            i_irq_vec,

    // ── Flag bypass external sources (MEM producer is internal) ──
    input  logic [3:0]            i_sr_flags,        // committed SR NZCV
    input  logic [3:0]            i_wb_flags,        // MEM/WB in-flight producer
    input  logic                  i_wb_writes_flags,

    // ── Committed SR word (RDSPR SR returns S/I from here + bypassed NZCV) ──
    // Only [31:4] (S/I/reserved) is consumed; [3:0] (committed NZCV) is
    // replaced by the flag bypass, so the low nibble is intentionally unused.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0]           i_sr_committed,
    /* verilator lint_on UNUSEDSIGNAL */

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic                  i_stall_in,        // MEM cannot accept this cycle
    input  logic                  i_wb_active,       // WB holds a live (non-bubble) insn
    input  logic                  i_bubble,          // force this insn to a bubble (fault flush from WB)
    input  logic [BCAUSE_W-1:0]   i_bcause,          // cause carried by an incoming ID/EX bubble
    output logic                  o_local_stall,     // back-pressure to ID (downstream-independent)
    output logic                  o_dc_commit,       // drain-commit insn commits this cycle
    output logic                  o_funit_stall,     // stall cause: waiting on the divmul unit (perfctr)

    // ── Branch resolution (to IF: redirect + flush IF1/IF2/ID) ──
    output logic                  o_branch_taken,    // redirect the front-end this cycle
    output logic [31:0]           o_branch_target,   // PC to redirect to

    // ── EX/MEM register (to MEM) ─────────────────────────────────
    output logic [OPC_W-1:0]      o_op_class,
    output logic [MEM_OP_W-1:0]   o_mem_op,
    output logic [1:0]            o_mem_size,
    output logic                  o_sign_ext,
    output logic [3:0]            o_sys_dev,
    output logic [3:0]            o_sys_reg,
    output logic [3:0]            o_spr_sel,
    output logic                  o_gpr_we,
    output logic                  o_spr_we,
    output logic                  o_flag_we,
    output logic [31:0]           o_result,
    output logic [31:0]           o_result_aux,      // a dual write's second (aux) result; don't-care otherwise
    output logic [31:0]           o_store_data,
    output logic [3:0]            o_flag_value,      // NZCV, packed as SR[3:0]
    output logic [SB_IDX_W-1:0]   o_phys_dst,
    output logic [SB_IDX_W-1:0]   o_phys_dst_aux,
    output logic                  o_phys_dst_aux_en,
    output logic [31:0]           o_pc,
    output logic                  o_valid,
    output logic [BCAUSE_W-1:0]   o_bcause,          // stall cause carried by this slot when it is a bubble
    output logic                  o_fault_pending,
    output logic [3:0]            o_fault_vec,
    output logic [31:0]           o_fault_status
);

    // ── Flag bypass: youngest in-flight NZCV for the EX reader ───
    // The MEM producer is this stage's own registered output: the
    // EX/MEM slot is the instruction now in MEM. A bubble or faulting
    // slot is not a committed flag writer, so it is gated off here.
    // The carry bit feeds the ALU (ADC/SBC); all four bits feed the
    // branch-condition evaluation below.
    logic [3:0] fwd_flags;
    logic       mem_writes_flags;
    assign mem_writes_flags = o_flag_we & o_valid & ~o_fault_pending;

    penumbra2_flag_bypass u_flag_bypass (
        .i_sr_flags(i_sr_flags),
        .i_mem_flags(o_flag_value),
        .i_mem_writes_flags(mem_writes_flags),
        .i_wb_flags(i_wb_flags),
        .i_wb_writes_flags(i_wb_writes_flags),
        .o_flags(fwd_flags)
    );

    // ── ALU compute ──────────────────────────────────────────────
    // NZCV bundle packs as SR[3:0]: N=0 Z=1 C=2 V=3. ADC/SBC take the
    // forwarded carry (bundle bit 2); other ops ignore the carry-in.
    logic        carry_in;
    logic [31:0] alu_result;
    logic        alu_n, alu_z, alu_c, alu_v;
    logic [3:0]  alu_flags;

    assign carry_in = fwd_flags[2];

    penumbra2_alu u_alu (
        .i_a(i_op_a),
        .i_b(i_op_b),
        .i_op(i_alu_op),
        .i_carry_in(carry_in),
        .o_result(alu_result),
        .o_flag_z(alu_z),
        .o_flag_n(alu_n),
        .o_flag_c(alu_c),
        .o_flag_v(alu_v)
    );

    assign alu_flags = {alu_v, alu_c, alu_z, alu_n};

    // ── Branch resolution ────────────────────────────────────────
    // EX resolves every control transfer. The condition is evaluated
    // against the forwarded NZCV by gen1's cond_eval (reused unchanged —
    // the cond encoding is shared ISA). For Format B the ALU already
    // computed the target (PC + offset) as its result; JMP/JALR carry
    // the target register in op_a. BL and JALR additionally link PC+4
    // into R13 — that link value, not the ALU result, must reach WB.
    logic cond_taken;
    cond_eval u_cond_eval (
        .i_flag_n(fwd_flags[0]),
        .i_flag_z(fwd_flags[1]),
        .i_flag_c(fwd_flags[2]),
        .i_flag_v(fwd_flags[3]),
        .i_cond(i_cond),
        .o_taken(cond_taken)
    );

    // Link-value mux: a linking control transfer (BL / JALR — the only
    // branch/JMP forms with gpr_we) writes PC+4 to R13, so its EX/MEM
    // result is next_pc; everything else keeps the ALU result (which is
    // the load/store EA for memory ops and the value for ALU ops).
    logic        is_link;
    logic [31:0] result_value;
    assign is_link      = (i_op_class == OPC_BRANCH || i_op_class == OPC_JMP) & i_gpr_we;

    // RDSPR SR composes the live status word: committed S/I (and reserved)
    // from i_sr_committed, NZCV from the flag bypass (the youngest in-flight
    // writer) so it reads identically to any other flag reader. SR has no
    // scoreboard entry, so it cannot ride the operand-B SPR path the value
    // SPRs use — it is built here instead. i_spr_sel distinguishes it from a
    // value-SPR RDSPR (whose result is ALU_PASS of the operand-B read).
    logic is_rdspr_sr;
    assign is_rdspr_sr  = (i_op_class == OPC_RDSPR) & (i_spr_sel == SPR_SR);

    assign result_value = is_link     ? i_next_pc
                        : is_rdspr_sr ? {i_sr_committed[31:4], fwd_flags}
                        :               alu_result;

    // branch_redirect / branch_target are the resolution proper.
    logic        branch_redirect;
    logic [31:0] branch_target;

    always_comb begin
        case (i_op_class)
            OPC_JMP: begin
                branch_redirect = 1'b1;
                branch_target   = i_op_a;
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

    // EX is the branch authority: it confirms or corrects the ID-stage
    // prediction. It redirects only on a *misprediction* — its resolution
    // disagreeing with the ID guess — so a correctly-predicted branch leaves
    // the speculative stream already in flight untouched. The fall-through on
    // a not-taken misprediction is i_next_pc.
    //
    // Two ways the guess can be wrong. Direction: EX's taken/not-taken
    // resolution disagrees with the ID guess — the only failure mode for a
    // direct branch, since EX recomputes its identical PC+imm target. Target:
    // an *indirect* prediction (a RAS-predicted return) can match in direction
    // yet jump elsewhere, so its resolved target must be checked too.
    logic        mispredict;
    logic        direction_wrong;
    logic        target_wrong;
    logic [31:0] redirect_target;
    assign direction_wrong = branch_redirect ^ i_predicted_taken;
    // A return (the only RAS-predicted kind) can resolve to a target other than
    // the RAS guess. Sourcing the jump target as i_op_a — not branch_target —
    // keeps this 32-bit compare off the ALU-result path. i_predicted_taken drops
    // an unpredicted JMP (direction_wrong already covers it); the OPC_JMP gate
    // keeps a direct branch from ever tripping this.
    assign target_wrong    = (i_op_class == OPC_JMP) & i_predicted_taken & (i_op_a != i_predicted_target);
    assign mispredict      = direction_wrong | target_wrong;
    assign redirect_target = branch_redirect ? branch_target : i_next_pc;

    // A taken redirect counts only for a real, non-faulting, un-flushed
    // branch slot; a bubble or a slot the fault flush is killing must
    // not steer the front-end.
    assign o_branch_taken  = mispredict & i_valid & ~i_fault_pending & ~i_bubble;
    assign o_branch_target = redirect_target;

    // ── Drain-commit FSM ─────────────────────────────────────────
    // ERET/WRSYS/WRSPR-SR/EI/DI must order their commit against older
    // in-flight instructions (which observe the pre-commit SR) and
    // younger ones (which observe the post-commit state). The insn holds
    // in EX — it never advances into MEM/WB — while MEM and WB drain;
    // once drained it commits (o_dc_commit pulse) and, for the WRSYS
    // variant, EX holds upstream one extra cycle (post_wait_q) so the
    // sysreg device latches before the next insn can observe it.
    //
    // MEM occupancy is EX's own EX/MEM register (o_valid) — MEM holds an
    // insn by back-pressuring, never by buffering past the register — so
    // only WB needs an occupancy feedback (i_wb_active).
    logic dc_here;          // a drain-commit insn occupies EX (held in ID/EX)
    logic drained;          // MEM and WB hold no live instruction
    logic dc_commit_now;    // commit fires this cycle
    logic post_wait_q;      // the WRSYS post-commit extra-hold cycle

    assign dc_here       = i_valid & i_drain_commit & ~i_fault_pending & ~i_bubble;
    assign drained       = ~o_valid & ~i_wb_active;
    assign dc_commit_now = dc_here & drained & ~post_wait_q;
    assign o_dc_commit   = dc_commit_now;

    always_ff @(posedge i_clk) begin
        if (i_rst) post_wait_q <= 1'b0;
        else       post_wait_q <= dc_commit_now & i_post_commit_wait;
    end

    // ── divmul peer unit ─────────────────────────────────────────
    // MUL/MULU/DIV/DIVU run on the multi-cycle divmul unit (a peer to
    // the ALU). EX holds i_start high while the divmul is the EX insn —
    // the unit edge-detects it, so it triggers once — and stalls while
    // o_busy. The cycle o_busy falls, both result halves are valid to
    // latch (low → result, high → result_aux). A DIV0 never iterates:
    // o_fault pulses on the start cycle with o_busy staying low, so it
    // completes in one cycle carrying VEC_ARITH.
    //
    // The unit's 5-bit op is the gen1 core-internal ALU-field encoding,
    // kept local here (as in divmul.sv / alu.sv) rather than in the
    // shared ISA package. divmul_op (ISA op[1:0]) selects it.
    localparam logic [4:0] DM_OP_MUL  = 5'b01101;
    localparam logic [4:0] DM_OP_MULU = 5'b01110;
    localparam logic [4:0] DM_OP_DIV  = 5'b01111;
    localparam logic [4:0] DM_OP_DIVU = 5'b10000;

    logic        is_divmul;
    logic        divmul_in_ex;   // a valid divmul occupies EX (the level)
    logic        dm_started;     // the divmul in EX has already been launched
    logic [4:0]  dm_op;
    logic        dm_start, dm_busy, dm_fault, dm_z, dm_n;
    logic [31:0] dm_lo, dm_hi;

    // Issue/back-pressure control outputs, declared here because the divmul
    // launch FF below references `advance`; both are assigned further down.
    logic        advance, next_valid;
    logic [BCAUSE_W-1:0] next_bcause;   // cause tag for the EX/MEM slot next edge

    assign is_divmul    = (i_op_class == OPC_DIVMUL);
    assign divmul_in_ex = is_divmul & i_valid & ~i_fault_pending & ~i_bubble;

    always_comb begin
        case (i_divmul_op)
            2'b00:   dm_op = DM_OP_MUL;
            2'b01:   dm_op = DM_OP_MULU;
            2'b10:   dm_op = DM_OP_DIV;
            default: dm_op = DM_OP_DIVU;
        endcase
    end

    // The unit's protocol wants a one-cycle i_start pulse, and it triggers on
    // a rising edge. A held level would launch only the FIRST of two adjacent
    // divmuls — the level never falls between back-to-back divmuls, so the
    // second's edge is swallowed and it silently reuses the first's result.
    // So pulse start exactly on a divmul's first EX cycle: dm_started latches
    // once we launch and holds until the instruction advances out of EX (the
    // next divmul re-arms). Holding until advance — not until the unit goes
    // idle — keeps a finished-but-back-pressured divmul from re-launching.
    assign dm_start = divmul_in_ex & ~dm_started;

    always_ff @(posedge i_clk) begin
        if (i_rst)         dm_started <= 1'b0;
        else if (advance)  dm_started <= 1'b0;   // left EX → re-arm for the next divmul
        else if (dm_start) dm_started <= 1'b1;   // launched this divmul
    end

    divmul u_divmul (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_a(i_op_a), .i_b(i_op_b),
        .i_op(dm_op), .i_start(dm_start),
        .o_busy(dm_busy), .o_fault(dm_fault),
        .o_result_lo(dm_lo), .o_result_hi(dm_hi),
        .o_flag_z(dm_z), .o_flag_n(dm_n)
    );

    // While the unit is busy EX holds the insn; it advances the cycle the unit
    // is no longer busy (with results, or with a DIV0 fault). The hold tracks
    // the level (divmul-in-EX), not the launch pulse — the pulse is high only
    // on the first cycle, but the hold must persist for the whole iteration.
    logic dm_stall;
    assign dm_stall  = divmul_in_ex & dm_busy;
    assign o_funit_stall = dm_stall;

    // ── Issue / back-pressure control ────────────────────────────
    // EX accepts a new ID/EX instruction every cycle except when it is
    // back-pressured (MEM via i_stall_in), sequencing a drain-commit, or
    // waiting on the divmul unit. i_bubble — the fault-flush from WB —
    // forces the in-flight slot to a bubble and wins over everything.
    // (advance/next_valid are declared above the divmul block.)
    always_comb begin
        if (i_bubble) begin
            next_valid = 1'b0;          // flush wins
            advance    = 1'b0;
        end else if (post_wait_q) begin
            next_valid = 1'b0;          // WRSYS already committed; the insn leaves as a bubble
            advance    = 1'b0;
        end else if (dc_here) begin
            // Drain-commit insn: never advances into MEM/WB. Inject a
            // bubble so MEM/WB drain — but only when MEM can accept;
            // while MEM is mid-access (i_stall_in) hold EX/MEM so its
            // live work is not clobbered.
            advance = 1'b0;
            if (i_stall_in) next_valid = o_valid;
            else            next_valid = 1'b0;
        end else if (dm_stall) begin
            // divmul busy: hold the insn in EX; it advances the cycle busy
            // clears. The held EX/MEM slot drains to a bubble — except when
            // MEM is back-pressuring EX (i_stall_in): the slot is then a live
            // instruction MEM has not accepted yet and must be preserved
            // intact (the same i_stall_in split the drain-commit branch makes).
            advance = 1'b0;
            if (i_stall_in) next_valid = o_valid;   // hold the un-accepted EX/MEM slot
            else            next_valid = 1'b0;       // drain: the divmul produced nothing
        end else if (i_stall_in) begin
            next_valid = o_valid;       // hold EX/MEM unchanged
            advance    = 1'b0;
        end else begin
            next_valid = i_valid;       // advance: bubble in if i_valid=0
            advance    = i_valid;
        end
    end

    // ── EX local back-pressure (downstream-independent) ──────────
    // EX holds upstream for its own reasons: sequencing a drain-commit (held
    // until MEM/WB drain; the WRSYS variant holds one extra cycle for the
    // device latch) or waiting on the divmul unit. A fault flush (i_bubble)
    // and the post-commit-wait cycle release upstream — the latter only ever
    // occurs with the pipe already drained (asserted below), so the downstream
    // stall it would otherwise mask is guaranteed absent. The spine ORs this
    // with the downstream stalls; the pre-refactor o_stall was exactly
    // ex_local_stall | i_stall_in.
    logic ex_local_stall;
    always_comb begin
        if      (i_bubble)    ex_local_stall = 1'b0;
        else if (post_wait_q) ex_local_stall = 1'b0;
        else if (dc_here)     ex_local_stall = drained ? i_post_commit_wait : 1'b1;
        else if (dm_stall)    ex_local_stall = 1'b1;
        else                  ex_local_stall = 1'b0;
    end
    assign o_local_stall = ex_local_stall;

    // ── EX/MEM bubble cause ───────────────────────────────────────
    // Mirrors the next_valid priority above (same idiom as ex_local_stall): the
    // cause stamped on the EX/MEM slot whenever EX injects or forwards a bubble.
    // A fault flush, the WRSYS post-commit hold, and a drain-commit drain are
    // front-end serialization (FLUSH); a divmul busy-wait is the execution unit
    // (FUNIT). When EX instead holds the slot under MEM back-pressure the carried
    // cause persists; on a clean advance it forwards the incoming ID/EX cause
    // (meaningful only if that slot is itself a bubble).
    always_comb begin
        if      (i_bubble)    next_bcause = BCAUSE_FLUSH;
        else if (post_wait_q) next_bcause = BCAUSE_FLUSH;
        else if (dc_here)     next_bcause = i_stall_in ? o_bcause : BCAUSE_FLUSH;
        else if (dm_stall)    next_bcause = i_stall_in ? o_bcause : BCAUSE_FUNIT;
        else if (i_stall_in)  next_bcause = o_bcause;
        else                  next_bcause = i_bcause;
    end

    // ── EX/MEM register ──────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid  <= 1'b0;
            o_bcause <= BCAUSE_FLUSH;       // cold pipe: the fill bubbles are front-end
        end else begin
            o_valid  <= next_valid;
            o_bcause <= next_bcause;        // travels with the slot, valid or bubble
            if (advance) begin
                o_op_class        <= i_op_class;
                o_mem_op          <= i_mem_op;
                o_mem_size        <= i_mem_size;
                o_sign_ext        <= i_sign_ext;
                o_sys_dev         <= i_sys_dev;
                o_sys_reg         <= i_sys_reg;
                o_spr_sel         <= i_spr_sel;
                o_gpr_we          <= i_gpr_we;
                o_spr_we          <= i_spr_we;
                o_flag_we         <= i_flag_we;
                // divmul drives both writeback halves and N/Z (C=V=0);
                // every other op uses the ALU result + flags. A divmul
                // only ever advances once its results are valid.
                o_result          <= is_divmul ? dm_lo : result_value;
                o_result_aux      <= is_divmul ? dm_hi : 32'b0;
                o_store_data      <= i_store_data;
                o_flag_value      <= is_divmul ? {2'b00, dm_z, dm_n} : alu_flags;
                o_phys_dst        <= i_phys_dst;
                o_phys_dst_aux    <= i_phys_dst_aux;
                o_phys_dst_aux_en <= i_phys_dst_aux_en;
                o_pc              <= i_pc;
                // EX raises two synchronous exceptions — a DIV0 (VEC_ARITH) and
                // a software trap (SYSCALL/BREAK, vector from decode) — and is
                // also where an eligible interrupt is injected as a synthetic
                // fault on this instruction. The slot's own exception outranks
                // the interrupt: an upstream fault, DIV0, or trap keeps its
                // vector, so the IRQ stays pending and retakes after that
                // handler. The interrupt's vector applies only on an otherwise
                // clean slot.
                o_fault_pending   <= i_fault_pending | (is_divmul & dm_fault) | i_is_trap | i_irq_inject;
                o_fault_vec       <= (is_divmul & dm_fault)         ? VEC_ARITH
                                   : (i_fault_pending | i_is_trap)  ? i_fault_vec
                                   : i_irq_inject                   ? i_irq_vec
                                   :                                  i_fault_vec;
                // DIV0, traps, and the injected interrupt carry no data address;
                // i_fault_status is FAULT_NONE for all of them (self-qualifying).
                o_fault_status    <= i_fault_status;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // EX-owned behavioral invariants the structure does not enforce.
    // Control-bundle consistency (gpr_we needs a dst, is_trap only on
    // SYSCALL/BREAK, dst_aux only on a dual-destination op, …) is owned and asserted in
    // penumbra2_decode, so it is not re-checked here.
    // ══════════════════════════════════════════════════════════
    always_comb begin
        // Advance precondition: the EX/MEM register latches a real
        // instruction only on a clean accept — a valid input, not
        // flushed, not back-pressured. A mis-gated latch (advancing
        // on a stall/bubble, or latching a bubble) violates this.
        assert (!advance || (i_valid && !i_bubble && !i_stall_in))
            else $error("penumbra2_ex_stage: advance without a clean precondition");
    end

    // A fault-commit flush from WB always lands as a bubble in EX/MEM:
    // a wrong-path or faulting instruction must never slip through to
    // MEM and commit.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_bubble |=> !o_valid)
        else $error("penumbra2_ex_stage: i_bubble did not flush the EX/MEM slot");

    // Back-pressure holds the EX/MEM slot intact: a stalled EX neither
    // drops nor fabricates its valid bit — the classic lost- or
    // duplicated-instruction bug at a stall boundary.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble) |=> $stable(o_valid))
        else $error("penumbra2_ex_stage: back-pressure changed o_valid");

    // ...and it does not swap the held instruction's identity under it
    // (the EX/MEM payload an upstream stall is waiting on must persist).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble && o_valid) |=> $stable(o_phys_dst))
        else $error("penumbra2_ex_stage: back-pressure swapped the held EX/MEM slot");

    // EX steers the front-end only for an actual control-transfer
    // instruction; a redirect resolved from any other op_class is a
    // branch-resolution bug.
    always_comb begin
        assert (!o_branch_taken ||
                i_op_class == OPC_BRANCH || i_op_class == OPC_JMP)
            else $error("penumbra2_ex_stage: branch redirect on a non-branch op_class");
    end

    // A drain-commit instruction never advances into MEM/WB — its effect
    // is the o_dc_commit pulse, not a downstream pipeline-register slot.
    // If one ever flowed past EX it would double-commit (once via
    // o_dc_commit, once at WB).
    always_comb begin
        assert (!(dc_here && advance))
            else $error("penumbra2_ex_stage: drain-commit insn advanced into EX/MEM");
    end

    // The post-commit hold never re-fires the commit (it is the cycle
    // *after* the WRSYS commit, holding for the device latch).
    always_comb begin
        assert (!(post_wait_q && o_dc_commit))
            else $error("penumbra2_ex_stage: drain-commit re-fired during post-commit hold");
    end

    // The post-commit hold lasts exactly one cycle — never two in a row.
    assert property (@(posedge i_clk) disable iff (i_rst)
        post_wait_q |=> !post_wait_q)
        else $error("penumbra2_ex_stage: post-commit hold exceeded one cycle");

    // The local-stall split (the spine ORs ex_local_stall with the downstream stalls,
    // reconstructing the old o_stall = ex_local_stall | i_stall_in) relies on
    // post_wait_q never coinciding with downstream back-pressure: post_wait_q
    // follows a drained commit, so MEM/WB hold nothing and i_stall_in is 0.
    // If this fires, ex_local_stall=0 here would wrongly drop a real downstream stall.
    assert property (@(posedge i_clk) disable iff (i_rst)
        post_wait_q |-> !i_stall_in)
        else $error("penumbra2_ex_stage: post-commit-wait coincided with downstream back-pressure");

    // A divmul never advances into EX/MEM while the unit is still busy —
    // it would carry stale/garbage result halves to WB.
    always_comb begin
        assert (!(dm_stall && advance))
            else $error("penumbra2_ex_stage: divmul advanced into EX/MEM while busy");
    end

endmodule
