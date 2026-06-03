// penumbra2_mem_stage — Penumbra/2 memory-access stage.
//
// Specified by the MEM section of doc/internals/penumbra2/pipeline-stages.md.
// It reads the EX/MEM register, performs the data-side work, and latches the
// MEM/WB register for the writeback stage under the back-pressure handshake.
//
// This is the pass-through core: for ALU / branch / divmul / RDSPR / WRSPR
// results, MEM is a single-cycle register move — the value EX produced flows
// straight to WB, no stall. The data-memory and sysreg-sideband path (loads,
// stores, RDSYS) — D-cache access, MMU translation, alignment check, sub-word
// extract/replicate, the 1-cycle access STALL — is not wired here; an
// assertion catches any such instruction reaching the stage rather than
// letting it forward a stale ALU result as if it were loaded data.
//
// Handshake mirrors the EX stage: MEM has no stall source of its own yet, so
// it back-pressures EX only when WB back-pressures it (i_stall_in) — which WB
// does for the extra cycle a dual-destination writeback takes. i_bubble
// (the fault-commit flush from WB) forces the MEM/WB slot to a bubble and wins
// over everything.
//
// The writeback value reaching WB is a single datum (o_wb_value): the doc's
// gpr_value and spr_value collapse here because no instruction both writes a
// GPR and writes an SPR — WB routes the one value by the (mutually exclusive)
// gpr_we / spr_we bits. o_wb_value_aux carries a dual write's second value.

module penumbra2_mem_stage
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── EX/MEM input: the instruction leaving EX ─────────────────
    input  logic [OPC_W-1:0]      i_op_class,        // for the deferred-path guard (RDSYS)
    input  logic [MEM_OP_W-1:0]   i_mem_op,          // for the deferred-path guard (load/store)
    input  logic                  i_gpr_we,
    input  logic                  i_spr_we,
    input  logic                  i_flag_we,
    input  logic [3:0]            i_spr_sel,
    input  logic [31:0]           i_result,          // ALU result / link value / a dual write's primary value
    input  logic [31:0]           i_result_aux,      // a dual write's second (aux) value; don't-care otherwise
    input  logic [3:0]            i_flag_value,      // NZCV, packed as SR[3:0]
    input  logic [SB_IDX_W-1:0]   i_phys_dst,
    input  logic [SB_IDX_W-1:0]   i_phys_dst_aux,
    input  logic                  i_phys_dst_aux_en,
    input  logic [31:0]           i_pc,
    input  logic                  i_valid,           // 0 = bubble in
    input  logic                  i_fault_pending,
    input  logic [3:0]            i_fault_vec,

    // ── Pipeline handshake ───────────────────────────────────────
    input  logic                  i_stall_in,        // WB cannot accept this cycle
    input  logic                  i_bubble,          // force this insn to a bubble (fault flush from WB)
    output logic                  o_stall,            // back-pressure to EX

    // ── MEM/WB register (to WB) ──────────────────────────────────
    output logic                  o_gpr_we,
    output logic                  o_spr_we,
    output logic                  o_flag_we,
    output logic [3:0]            o_spr_sel,
    output logic [31:0]           o_wb_value,        // GPR-or-SPR writeback datum (a dual write's primary value)
    output logic [31:0]           o_wb_value_aux,    // a dual write's second (aux) value; don't-care otherwise
    output logic [3:0]            o_flag_value,
    output logic [SB_IDX_W-1:0]   o_phys_dst,
    output logic [SB_IDX_W-1:0]   o_phys_dst_aux,
    output logic                  o_phys_dst_aux_en,
    output logic [31:0]           o_pc,
    output logic                  o_valid,
    output logic                  o_fault_pending,
    output logic [3:0]            o_fault_vec
);

    // ── Writeback-value select + deferred-path guard ─────────────
    // The value that reaches WB as the GPR/SPR write datum. In the full
    // stage this muxes the ALU result against loaded data (LDx) and
    // sysreg-read data (RDSYS); with the D-cache / MMU / sysreg sideband
    // not yet wired, only the ALU-result source exists.
    logic [31:0] wb_value;

    assign wb_value = i_result;

    // No load/store/RDSYS may reach the skeleton — there is no D-cache,
    // MMU, or sysreg sideband yet, so wb_value (= i_result) is only the
    // correct writeback datum for ALU/branch/divmul/SPR results. Fire on
    // any deferred op: the guard holds on NOT-memory AND NOT-rdsys.
    always_comb begin
        assert (!i_valid || (i_mem_op == MEM_NONE && i_op_class != OPC_RDSYS))
            else $error("penumbra2_mem_stage: memory/RDSYS op reached the skeleton");
    end

    // ── Issue / back-pressure control ────────────────────────────
    // MEM accepts a new EX/MEM instruction every cycle unless WB
    // back-pressures it (i_stall_in). It has no stall source of its own.
    // i_bubble — the fault-flush from WB — forces the in-flight slot to a
    // bubble and wins over everything.
    logic advance, next_valid;

    always_comb begin
        if (i_bubble) begin
            next_valid = 1'b0;          // flush wins
            advance    = 1'b0;
            o_stall    = i_stall_in;
        end else if (i_stall_in) begin
            next_valid = o_valid;       // hold MEM/WB unchanged
            advance    = 1'b0;
            o_stall    = 1'b1;
        end else begin
            next_valid = i_valid;       // advance: bubble in if i_valid=0
            advance    = i_valid;
            o_stall    = 1'b0;
        end
    end

    // ── MEM/WB register ──────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_valid <= 1'b0;
        end else begin
            o_valid <= next_valid;
            if (advance) begin
                o_gpr_we          <= i_gpr_we;
                o_spr_we          <= i_spr_we;
                o_flag_we         <= i_flag_we;
                o_spr_sel         <= i_spr_sel;
                o_wb_value        <= wb_value;
                o_wb_value_aux    <= i_result_aux;
                o_flag_value      <= i_flag_value;
                o_phys_dst        <= i_phys_dst;
                o_phys_dst_aux    <= i_phys_dst_aux;
                o_phys_dst_aux_en <= i_phys_dst_aux_en;
                o_pc              <= i_pc;
                o_fault_pending   <= i_fault_pending;
                o_fault_vec       <= i_fault_vec;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // MEM-owned handshake invariants the structure does not enforce.
    // (The deferred-path guard belongs to the human-contribution block above.)
    // ══════════════════════════════════════════════════════════

    // Advance precondition: the MEM/WB register latches a real
    // instruction only on a clean accept — a valid input, not flushed,
    // not back-pressured.
    always_comb begin
        assert (!advance || (i_valid && !i_bubble && !i_stall_in))
            else $error("penumbra2_mem_stage: advance without a clean precondition");
    end

    // A fault-commit flush from WB always lands as a bubble in MEM/WB:
    // a wrong-path or faulting instruction must never slip through to WB
    // and commit.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_bubble |=> !o_valid)
        else $error("penumbra2_mem_stage: i_bubble did not flush the MEM/WB slot");

    // Back-pressure holds the MEM/WB slot intact: a stalled MEM neither
    // drops nor fabricates its valid bit (the lost- / duplicated-insn bug
    // at a stall boundary).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble) |=> $stable(o_valid))
        else $error("penumbra2_mem_stage: back-pressure changed o_valid");

    // ...and it does not swap the held instruction's identity under it.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_stall_in && !i_bubble && o_valid) |=> $stable(o_phys_dst))
        else $error("penumbra2_mem_stage: back-pressure swapped the held MEM/WB slot");

endmodule
