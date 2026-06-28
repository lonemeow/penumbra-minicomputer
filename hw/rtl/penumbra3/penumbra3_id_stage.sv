// penumbra3_id_stage -- Penumbra/3 issue stage.
//
// The integration point of the gen3 front half's tail. Unlike gen2's ID, it
// does NOT decode -- the word->bundle decode already ran on the fetch-FIFO
// enqueue path, so ID reads a *pre-decoded* ctrl_bundle_t off the FIFO head.
// Each cycle it:
//   - maps the bundle's architectural register refs to physical scoreboard
//     indices (penumbra3_regmap for GPRs; an inline map for SPR refs),
//   - reads the scoreboard pending bits for the two sources,
//   - decides issue (penumbra3_issue): block a source only when it is read,
//     pending, and not forwardable this cycle,
//   - drives the regfile read ports and selects the final ALU operands,
//   - finalizes the mode-dependent exception state (priv fault + vector),
//   - latches the bundle + operands + physical dst + PC into the ID/EX
//     register under the back-pressure handshake.
//
// Hazard model (diverges from gen2's unforwarded scoreboard). gen3 forwards,
// so the scoreboard tracks only the *non-forwardable* producers -- loads,
// divmul, and sysreg reads (is_pending_class) -- whose results are not
// available by the time a back-to-back dependent would forward them. ALU
// results never set a bit; EX forwards them. NZCV is not a scoreboard entry
// (it forwards in EX), so flag reads never stall here.
//
// Forwarding boundary. Whether a pending source can be supplied *this* cycle
// depends on where its producer sits in EX/MEM/WB -- so the spine broadcasts
// "physical register R's value is forwardable now" on the forward ports, and
// this stage only *matches* its source indices against them. The broadcast
// generation (which stages, what "ready" means) is co-designed with EX.

module penumbra3_id_stage
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── Fetch-FIFO head (pre-decoded) ────────────────────────────
    input  ctrl_bundle_t          i_bundle,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,
    input  logic                  i_valid,          // FIFO head present (o_deq_valid)
    input  logic                  i_if_fault_pending, // IF-stage fault carried in the slot
    input  logic [3:0]            i_if_fault_vec,
    input  logic [31:0]           i_if_fault_status,
    output logic                  o_deq_ready,      // consume the FIFO head this cycle

    input  logic                  i_supervisor,     // SR.S (live)

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic                  i_stall_in,       // EX cannot accept this cycle
    input  logic                  i_bubble,         // redirect / fault flush
    input  logic                  i_fetch_busy,     // front end memory-bound (bcause split)
    input  logic                  i_pipe_hold,      // registered back-end hold (load_pending, ...)
    output logic                  o_local_stall,    // scoreboard interlock (spine back-pressure + perfctr)

    // ── Regfile read (external) ──────────────────────────────────
    output logic [SB_IDX_W-1:0]   o_rd_idx_a,
    output logic [SB_IDX_W-1:0]   o_rd_idx_b,
    input  logic [31:0]           i_rd_data_a,
    input  logic [31:0]           i_rd_data_b,

    // ── SPR-file source read (EPC/ESR/SCRn have no regfile entry) ─
    output logic [3:0]            o_spr_rd_sel,
    input  logic [31:0]           i_spr_src_value,

    // ── Scoreboard clear (from a downstream completion) ──────────
    input  logic                  i_clr_en,
    input  logic [SB_IDX_W-1:0]   i_clr_idx,

    // ── Forward broadcasts (driven by the spine from EX/MEM2/WB) ─
    // Each names a physical register whose value is forwardable to EX this
    // cycle. A pending source matching one may issue (EX supplies the value).
    input  logic                  i_fwd0_en,
    input  logic [SB_IDX_W-1:0]   i_fwd0_idx,
    input  logic                  i_fwd1_en,
    input  logic [SB_IDX_W-1:0]   i_fwd1_idx,

    // ── ID/EX register (to EX) ───────────────────────────────────
    output ctrl_bundle_t          o_ex_bundle,
    output logic [31:0]           o_ex_op_a,
    output logic [31:0]           o_ex_op_b,
    output logic [31:0]           o_ex_store_data,
    output logic [SB_IDX_W-1:0]   o_ex_phys_dst,
    output logic                  o_ex_phys_dst_we,    // dst occupies a scoreboard entry / regfile write
    output logic [SB_IDX_W-1:0]   o_ex_phys_dst_aux,
    output logic                  o_ex_phys_dst_aux_we,
    // Operand-forward match qualifiers (EX matches these against the in-flight
    // producers; fwdable = the final operand is a GPR regfile read).
    output logic [SB_IDX_W-1:0]   o_ex_phys_src_a,
    output logic                  o_ex_src_a_fwdable,
    output logic [SB_IDX_W-1:0]   o_ex_phys_src_b,
    output logic                  o_ex_src_b_fwdable,
    output logic [31:0]           o_ex_pc,
    output logic [31:0]           o_ex_next_pc,
    output logic                  o_ex_valid,
    output logic                  o_ex_fault_pending,
    output logic [3:0]            o_ex_fault_vec,
    output logic [31:0]           o_ex_fault_status,
    output bcause_e               o_ex_bcause
);

    // ── Architectural -> physical mapping ────────────────────────
    // src_a is always a GPR; src_b and dst may name an SPR. GPRs go through
    // penumbra3_regmap (R14 banks on live supervisor); SPR refs map inline to
    // their scoreboard entry (SR is untracked -- it forwards / serializes).
    logic [SB_IDX_W-1:0] gpr_src_a, gpr_src_b, gpr_dst, gpr_dst_aux;
    logic                gpr_src_a_tr, gpr_src_b_tr, gpr_dst_tr, gpr_dst_aux_tr;

    penumbra3_regmap #(.IDX_BITS (SB_IDX_W)) u_rm_a (
        .i_areg       (i_bundle.src_a_sel),
        .i_supervisor (i_supervisor),
        .o_pidx       (gpr_src_a),
        .o_tracked    (gpr_src_a_tr)
    );
    penumbra3_regmap #(.IDX_BITS (SB_IDX_W)) u_rm_b (
        .i_areg       (i_bundle.src_b_sel),
        .i_supervisor (i_supervisor),
        .o_pidx       (gpr_src_b),
        .o_tracked    (gpr_src_b_tr)
    );
    penumbra3_regmap #(.IDX_BITS (SB_IDX_W)) u_rm_d (
        .i_areg       (i_bundle.dst_sel),
        .i_supervisor (i_supervisor),
        .o_pidx       (gpr_dst),
        .o_tracked    (gpr_dst_tr)
    );
    // The aux (Rdh) destination is a GPR field distinct from the primary Rd, so
    // it needs its own arch->phys map -- the divmul high half writes a different
    // register than the low half.
    penumbra3_regmap #(.IDX_BITS (SB_IDX_W)) u_rm_aux (
        .i_areg       (i_bundle.dst_aux_sel),
        .i_supervisor (i_supervisor),
        .o_pidx       (gpr_dst_aux),
        .o_tracked    (gpr_dst_aux_tr)
    );

    // SPR# -> scoreboard entry. SR has no entry (returns 0, untracked).
    function automatic logic [SB_IDX_W-1:0] spr_to_sb(input logic [3:0] spr);
        case (spr)
            SPR_ESR:  spr_to_sb = SB_ESR;
            SPR_EPC:  spr_to_sb = SB_EPC;
            SPR_USP:  spr_to_sb = SB_USP;
            SPR_SCR0: spr_to_sb = SB_SCR0;
            SPR_SCR1: spr_to_sb = SB_SCR1;
            SPR_SCR2: spr_to_sb = SB_SCR2;
            SPR_SCR3: spr_to_sb = SB_SCR3;
            default:  spr_to_sb = '0;          // SR or undefined: no entry
        endcase
    endfunction

    logic [SB_IDX_W-1:0] phys_src_a, phys_src_b, phys_dst;
    logic                phys_src_a_used, phys_src_b_used, phys_dst_we;
    assign phys_src_a = gpr_src_a;                              // src_a is always a GPR
    assign phys_src_b = i_bundle.src_b_is_spr ? spr_to_sb(i_bundle.src_b_sel) : gpr_src_b;
    assign phys_dst   = i_bundle.dst_is_spr   ? spr_to_sb(i_bundle.dst_sel)   : gpr_dst;

    // A reference occupies a scoreboard entry when it is enabled/written and
    // maps to a tracked entry (GPR: not R0/PC; SPR: not SR).
    assign phys_src_a_used = i_bundle.src_a_en & gpr_src_a_tr;  // src_a is GPR-only
    assign phys_src_b_used = i_bundle.src_b_en &
                             (i_bundle.src_b_is_spr ? (i_bundle.src_b_sel != SPR_SR) : gpr_src_b_tr);
    assign phys_dst_we     = i_bundle.dst_we &
                             (i_bundle.dst_is_spr   ? (i_bundle.dst_sel   != SPR_SR) : gpr_dst_tr);

    // A source is forwardable when its final operand is a GPR regfile read --
    // not the PC, an immediate, or an SPR-file value. (a_from_pc / src_*_is_pc
    // imply the matching enable is low, so they are already excluded; b_from_imm
    // is the store case where src_b is live but op_b carries the EA immediate.)
    logic src_a_fwdable, src_b_fwdable;
    assign src_a_fwdable = i_bundle.src_a_en & ~i_bundle.a_from_pc & gpr_src_a_tr;
    assign src_b_fwdable = i_bundle.src_b_en & ~i_bundle.src_b_is_spr
                         & ~i_bundle.src_b_is_pc & ~i_bundle.b_from_imm & gpr_src_b_tr;

    // ── Regfile + SPR-file read ──────────────────────────────────
    assign o_rd_idx_a   = phys_src_a;
    assign o_rd_idx_b   = phys_src_b;
    assign o_spr_rd_sel = i_bundle.src_b_sel;

    // EPC/ESR/SCRn are SPR-file-backed (no regfile entry): a RDSPR of one reads
    // its value here as operand B (ALU_PASS carries it). USP reads the regfile
    // R14 bank normally.
    logic src_b_spr_file;
    assign src_b_spr_file = i_bundle.src_b_is_spr
                          & (i_bundle.src_b_sel == SPR_EPC | i_bundle.src_b_sel == SPR_ESR
                             | (i_bundle.src_b_sel >= SPR_SCR0 & i_bundle.src_b_sel <= SPR_SCR3));

    // ── Operand select ───────────────────────────────────────────
    // A register source reads as the live PC when decode flagged it R15/PC.
    logic [31:0] src_a_val, src_b_val;
    assign src_a_val = i_bundle.src_a_is_pc ? i_pc : i_rd_data_a;
    assign src_b_val = i_bundle.src_b_is_pc ? i_pc : i_rd_data_b;

    logic [31:0] op_a_sel, op_b_sel, store_data_sel;
    assign op_a_sel = i_bundle.a_from_pc ? i_pc : src_a_val;
    always_comb begin
        if      (i_bundle.b_from_imm) op_b_sel = i_bundle.imm;
        else if (src_b_spr_file)      op_b_sel = i_spr_src_value;
        else                          op_b_sel = src_b_val;
    end
    assign store_data_sel = src_b_val;

    // ── Mode-dependent exception finalize ────────────────────────
    // Decode emitted priv_op (mode-independent); the actual privilege fault and
    // the fault vector are composed here against live mode. IF-stage faults
    // outrank decode faults; SYSCALL/BREAK are traps, not faults.
    logic priv_fault, insn_fault_pending;
    logic [3:0] insn_fault_vec;
    assign priv_fault = i_bundle.priv_op & ~i_supervisor;
    assign insn_fault_pending = i_if_fault_pending | i_bundle.illegal | priv_fault;
    always_comb begin
        if      (i_if_fault_pending)            insn_fault_vec = i_if_fault_vec;
        else if (i_bundle.illegal)              insn_fault_vec = VEC_ILLEGAL;
        else if (priv_fault)                    insn_fault_vec = VEC_PRIV;
        else if (i_bundle.op_class == OPC_SYSCALL) insn_fault_vec = VEC_SYSCALL;
        else if (i_bundle.op_class == OPC_BREAK)   insn_fault_vec = VEC_BREAK;
        else                                    insn_fault_vec = 4'd0;
    end

    // A faulting / bubble slot is inert to the scoreboard.
    logic sb_eligible;
    assign sb_eligible = i_valid & ~insn_fault_pending;

    // ── Pending-class: producers the scoreboard must track ───────
    // Results not available by EX-forward time -- loads, divmul, sysreg reads.
    // Everything else issues freely and is forwarded in EX, so it sets no bit.
    logic is_pending_class;
    assign is_pending_class = (i_bundle.op_class == OPC_LOAD)
                            | (i_bundle.op_class == OPC_DIVMUL)
                            | (i_bundle.op_class == OPC_RDSYS);

    // ── Scoreboard ───────────────────────────────────────────────
    logic src_a_pending, src_b_pending;
    logic issue;   // forward-declared; driven by the issue/handshake block

    penumbra3_scoreboard #(.NREGS (SB_NUM_ENTRIES)) u_scoreboard (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        // Set both destination bits a pending-class op occupies on issue: the
        // primary (Rd) always, and the aux (Rdh) for the dual-destination
        // divmul -- the aux is not EX-forwarded, so a dependent must stall on it
        // until it writes back.
        .i_set_en       (issue & sb_eligible & is_pending_class & phys_dst_we),
        .i_set_idx      (phys_dst),
        .i_set2_en      (issue & sb_eligible & i_bundle.dst_aux_we & gpr_dst_aux_tr),
        .i_set2_idx     (gpr_dst_aux),
        .i_clr_en       (i_clr_en),
        .i_clr_idx      (i_clr_idx),
        .i_src0_idx     (phys_src_a),
        .i_src1_idx     (phys_src_b),
        .o_src0_pending (src_a_pending),
        .o_src1_pending (src_b_pending)
    );

    // ── Forward availability ─────────────────────────────────────
    // A pending source may issue this cycle if a forward broadcast names its
    // physical register (EX will supply the value). See the forwarding-boundary
    // note in the header: ID matches, the spine generates the broadcasts.
    logic src_a_fwd, src_b_fwd;
    assign src_a_fwd = (i_fwd0_en && phys_src_a == i_fwd0_idx)
                     | (i_fwd1_en && phys_src_a == i_fwd1_idx);
    assign src_b_fwd = (i_fwd0_en && phys_src_b == i_fwd0_idx)
                     | (i_fwd1_en && phys_src_b == i_fwd1_idx);

    // ── Issue decision ───────────────────────────────────────────
    logic can_issue;
    penumbra3_issue u_issue (
        .i_valid        (i_valid),
        .i_pipe_hold    (i_pipe_hold),
        .i_src0_used    (phys_src_a_used),
        .i_src0_pending (src_a_pending),
        .i_src0_fwd     (src_a_fwd),
        .i_src1_used    (phys_src_b_used),
        .i_src1_pending (src_b_pending),
        .i_src1_fwd     (src_b_fwd),
        .o_can_issue    (can_issue)
    );

    // A fault-tagged slot always issues (it never scoreboard-stalls --
    // sb_eligible gates its sources off -- and faults at WB).
    logic eligible;
    assign eligible = can_issue | (i_valid & insn_fault_pending & ~i_pipe_hold);

    // The local (downstream-independent) stall: a live slot that cannot issue
    // for a hazard reason. Surfaced for the spine's back-pressure and perfctr.
    assign o_local_stall = i_valid & ~eligible;

    // ── Issue / back-pressure control ────────────────────────────
    logic next_valid;
    always_comb begin
        if (i_bubble) begin
            next_valid = 1'b0;
            issue      = 1'b0;
        end else if (i_stall_in) begin
            next_valid = o_ex_valid;     // hold ID/EX unchanged
            issue      = 1'b0;
        end else begin
            next_valid = eligible;
            issue      = eligible;
        end
    end

    // The FIFO head is consumed exactly when ID advances it into EX.
    assign o_deq_ready = issue;

    // ── ID/EX bubble cause ───────────────────────────────────────
    bcause_e next_bcause;
    always_comb begin
        if      (i_bubble)        next_bcause = BCAUSE_FLUSH;
        else if (i_stall_in)      next_bcause = o_ex_bcause;     // hold the carried cause
        else if (~i_valid)        next_bcause = i_fetch_busy ? BCAUSE_IFETCH : BCAUSE_FLUSH;
        else if (~eligible)       next_bcause = BCAUSE_HAZARD;
        else                      next_bcause = BCAUSE_NONE;     // issuing
    end

    // ── ID/EX register ───────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_ex_valid  <= 1'b0;
            o_ex_bcause <= BCAUSE_FLUSH;
        end else begin
            o_ex_valid  <= next_valid;
            o_ex_bcause <= next_bcause;
            if (issue) begin
                o_ex_bundle          <= i_bundle;
                o_ex_op_a            <= op_a_sel;
                o_ex_op_b            <= op_b_sel;
                o_ex_store_data      <= store_data_sel;
                o_ex_phys_dst        <= phys_dst;
                o_ex_phys_dst_we     <= phys_dst_we & ~insn_fault_pending;
                o_ex_phys_dst_aux    <= gpr_dst_aux;   // aux (divmul Rdh): its own GPR, distinct from Rd
                o_ex_phys_dst_aux_we <= i_bundle.dst_aux_we & ~insn_fault_pending;
                o_ex_phys_src_a      <= phys_src_a;
                o_ex_src_a_fwdable   <= src_a_fwdable;
                o_ex_phys_src_b      <= phys_src_b;
                o_ex_src_b_fwdable   <= src_b_fwdable;
                o_ex_pc              <= i_pc;
                o_ex_next_pc         <= i_next_pc;
                o_ex_fault_pending   <= insn_fault_pending;
                o_ex_fault_vec       <= insn_fault_vec;
                o_ex_fault_status    <= i_if_fault_status;
            end
        end
    end

    // ── Assertions (sim-only; stripped at synth) ─────────────────
    // Issue precondition: advance into EX only as a clean, eligible slot.
    always_comb
        assert (!issue || (eligible && !i_bubble && !i_stall_in))
            else $error("penumbra3_id_stage: issue without a clean precondition");
    // A committed ID/EX slot is never both faulting and an aux-write op.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_ex_valid |-> !(o_ex_fault_pending && o_ex_phys_dst_aux_we))
        else $error("penumbra3_id_stage: faulting slot carries an aux write");

endmodule
