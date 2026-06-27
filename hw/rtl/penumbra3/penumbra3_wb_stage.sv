// penumbra3_wb_stage -- Penumbra/3 writeback stage (the commit point).
//
// Reads the MEM2/WB register -- the resolved ctrl_bundle_t + dpath_payload_t a
// retiring instruction carries -- and commits it. From that pair alone it:
//   - routes the destination write to exactly one storage by physical index:
//     a regfile entry (R1..R13 and the banked R14 USP/SSP) for indices up to
//     SB_SSP, or the SPR-file / scratch storage (EPC/ESR/SCRn) from SB_ESR up,
//   - drives the NZCV (-> SR[3:0]) flag write strobe,
//   - sequences a dual-destination write (divmul lo then hi) through the single
//     regfile port over two cycles, back-pressuring MEM2 for the second,
//   - takes a carried fault precisely, gating every architectural write off and
//     pulsing o_fault_commit for the flush + save-state in the spine.
//
// The split keys off the *physical index*, not the bundle's dst_is_spr bit:
// WRSPR USP is SPR-named yet regfile-backed (index SB_USP), so the index is the
// only oracle that routes it correctly. WRSPR SR never reaches here (the decoder
// raises it OPC_ILLEGAL); the SR S/I bits move only via exception entry, ERET,
// and EI/DI, all of which commit in EX and never advance into WB.
//
// WB is the downstream-most stage, so its only back-pressure is the dual-write
// second-write hold; the spine ORs o_local_stall into MEM2's freeze. The
// committing destinations are visible on the MEM2/WB register the spine already
// taps for forwarding, so WB does not re-export them or clear the scoreboard.

module penumbra3_wb_stage
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // -- MEM2/WB input: the instruction reaching commit ------------
    // WB consumes only dst_sel + flags_updater from the bundle (the rest resolved
    // upstream into the payload), but the bundle still rides through as a unit.
    /* verilator lint_off UNUSEDSIGNAL */
    input  ctrl_bundle_t          i_bundle,
    /* verilator lint_on UNUSEDSIGNAL */
    input  dpath_payload_t        i_payload,
    input  logic                  i_valid,            // 0 = bubble
    input  bcause_e               i_bcause,           // stall cause carried by a bubble in this slot

    // -- Back-pressure to MEM2 (the dual-write second-write hold) --
    output logic                  o_local_stall,

    // -- Retire + stall-cause reporting (perfctr) -----------------
    output logic                  o_insn_committed,   // one pulse per distinct retiring instruction
    output bcause_e               o_bcause,           // cause charged on a non-retiring cycle

    // -- Regfile write port (regfile is external) -----------------
    output logic [SB_IDX_W-1:0]   o_regfile_idx,
    output logic [31:0]           o_regfile_data,
    output logic                  o_regfile_we,

    // -- SPR-file / scratch write strobe (spine routes by SPR#) ---
    output logic                  o_spr_we,
    output logic [3:0]            o_spr_sel,
    output logic [31:0]           o_spr_value,

    // -- Flag (NZCV -> SR[3:0]) write strobe ----------------------
    output logic                  o_flag_we,
    output logic [3:0]            o_flag_value,

    // -- Fault commit (to the exception unit: flush + save-state) -
    output logic                  o_fault_commit,
    output logic [3:0]            o_fault_vec,
    output logic [31:0]           o_fault_pc,         // -> EPC
    output logic [31:0]           o_fault_vaddr,      // -> FADDR
    output logic [31:0]           o_fault_status      // -> FSTAT
);

    // -- Fault commit: take the fault at the oldest in-flight slot -
    // In-order completion makes the WB slot the oldest in flight, so a fault
    // carried here is the oldest faulting one and taking it is precise. A
    // faulting instruction is inert: can_commit gates every architectural write
    // off, and o_fault_commit launches the flush + save-state instead.
    logic can_commit;
    assign can_commit     = i_valid & ~i_payload.fault_pending;
    assign o_fault_commit = i_valid & i_payload.fault_pending;

    assign o_fault_vec    = i_payload.fault_vec;
    assign o_fault_pc     = i_payload.pc;
    assign o_fault_vaddr  = i_payload.fault_vaddr;
    assign o_fault_status = i_payload.fault_status;

    // -- Destination storage routing (by physical index) ----------
    // The regmap already folded GPR/SPR/bank selection into the index, so the
    // index range alone names the storage. The aux destination (divmul hi half)
    // is a GPR by construction, so it always targets the regfile.
    logic dst_regfile, dst_sprfile;
    assign dst_regfile = i_payload.phys_dst_we & (i_payload.phys_dst <= SB_SSP);
    assign dst_sprfile = i_payload.phys_dst_we & (i_payload.phys_dst >= SB_ESR);

    // -- Flag + SPR write strobes ---------------------------------
    // Passive strobes to the external SR / SPR storage. flags_updater doubles as
    // the WB flag write-enable (EX computed the NZCV in the payload). A dual
    // write re-asserts flag_we across both cycles, re-writing the same NZCV
    // (idempotent), and has dst_sprfile = 0 (its dests are GPRs), so the SPR
    // strobe stays quiet throughout.
    assign o_flag_value = i_payload.flags;
    assign o_flag_we    = can_commit & i_bundle.flags_updater;
    assign o_spr_sel    = i_bundle.dst_sel;          // SPR number for a WRSPR
    assign o_spr_value  = i_payload.value;
    assign o_spr_we     = can_commit & dst_sprfile;

    // -- Regfile write port + dual-write sequencing ---------------
    // A normal commit is one regfile write. A divmul is a dual-destination write:
    // its lo half (phys_dst / value) and hi half (phys_dst_aux / value_aux) share
    // the single write port over two consecutive cycles, holding MEM2 for the
    // second. writing_aux is the flop that tells the two cycles apart; a faulting
    // slot is inert (can_commit = 0), so it never enters the sequence.
    logic wb_dual_write;
    assign wb_dual_write = can_commit & i_payload.phys_dst_aux_we;

    logic writing_aux, next_writing_aux;
    always_ff @(posedge i_clk) begin
        if (i_rst) writing_aux <= 1'b0;
        else       writing_aux <= next_writing_aux;
    end

    logic [SB_IDX_W-1:0] regfile_idx;
    logic [31:0]         regfile_data;
    logic                regfile_we;

    // The primary cycle handles both a normal single commit and a dual write's
    // lo half (same phys_dst / value); it stalls MEM2 and arms the aux cycle only
    // for a dual write. The aux cycle writes the hi half; it is reached only from
    // a dual write, so its write fires unconditionally (the aux dst is a GPR by
    // construction). next_writing_aux is the hold *and* the FSM advance -- the
    // same event -- so o_local_stall reuses it directly.
    always_comb begin
        if (!writing_aux) begin
            regfile_idx      = i_payload.phys_dst;
            regfile_data     = i_payload.value;
            regfile_we       = can_commit & dst_regfile;
            next_writing_aux = wb_dual_write;
        end else begin
            regfile_idx      = i_payload.phys_dst_aux;
            regfile_data     = i_payload.value_aux;
            regfile_we       = 1'b1;
            next_writing_aux = 1'b0;
        end
    end

    assign o_local_stall = next_writing_aux;

    assign o_regfile_idx  = regfile_idx;
    assign o_regfile_data = regfile_data;
    assign o_regfile_we   = regfile_we;

    // -- Retire + stall-cause reporting ---------------------------
    // A distinct instruction commits when a valid slot is at WB and this is not a
    // dual write's aux continuation (already counted on its primary cycle).
    // Faulting slots still count -- they complete via the fault path. The aux
    // cycle is the only valid slot that retires nothing; it is the divmul's
    // second register write, so it is charged to the execution unit. Every other
    // non-retiring cycle is a bubble forwarding its carried cause.
    assign o_insn_committed = i_valid & ~writing_aux;
    assign o_bcause         = writing_aux ? BCAUSE_FUNIT : i_bcause;

    // ==============================================================
    // Assertions -- sim-only (Verilator --assert); stripped at synth.
    // ==============================================================

    // A faulting commit takes the fault and writes nothing: the fault-commit
    // pulse is mutually exclusive with every architectural write.
    always_comb
        assert (!(o_fault_commit && (o_regfile_we || o_spr_we || o_flag_we)))
            else $error("penumbra3_wb_stage: faulting instruction also wrote architectural state");

    // The regfile and SPR-file writes are mutually exclusive -- a destination
    // routes to exactly one storage.
    always_comb
        assert (!(o_regfile_we && o_spr_we))
            else $error("penumbra3_wb_stage: regfile and SPR write fired in the same cycle");

    // o_local_stall is the dual-write second-write hold only; nothing else
    // back-pressures MEM2 from WB.
    always_comb
        assert (!o_local_stall || wb_dual_write)
            else $error("penumbra3_wb_stage: stall asserted outside a dual-destination writeback");

    // The aux (second-write) cycle only happens while a dual write occupies WB;
    // otherwise the aux write would land on another instruction's destination.
    always_comb
        assert (!writing_aux || wb_dual_write)
            else $error("penumbra3_wb_stage: aux write without a dual write in WB");

endmodule
