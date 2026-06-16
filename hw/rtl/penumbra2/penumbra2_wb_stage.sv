// penumbra2_wb_stage — Penumbra/2 writeback stage.
//
// The pipeline's commit point. Specified by the WB section of
// doc/internals/penumbra2/pipeline-stages.md plus the writeback rules in
// regfile.md and hazard-model.md. From the MEM/WB register it:
//   - drives the single regfile write port (GPR / SP commit),
//   - sequences a dual-destination write's two registers through that one
//     port over two consecutive cycles, holding MEM one extra cycle (the
//     file has no second write port),
//   - drives the flag (NZCV->SR) and SPR write strobes to their owning
//     modules.
//
// WB's pending writeback destinations are observable directly from the
// MEM/WB register, so the scoreboard's view of them is derived by the
// integration rather than exported from here.
//
// The regfile, status_reg (SR/ESR), EPC, and SPR-scratch storage are
// external modules: WB drives their write strobes, the same split by which
// ID drives the regfile read ports. There is no write-through — a strobe
// lands synchronously and a same-cycle ID read still sees the old value
// (the no-forwarding baseline; the matching extra ID stall is in the
// scoreboard).
//
// Fault commit: when the instruction reaching commit carries a fault, WB
// takes it here (the oldest in-flight slot, so precise) — every architectural
// write is gated off and o_fault_commit pulses, driving the younger-instruction
// flush and the save-state pulse (EPC/ESR/SR/bank) in the integration. The
// IF-side vector-fetch FSM that consumes o_fault_commit is a later milestone.

module penumbra2_wb_stage
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── MEM/WB input: the instruction reaching commit ────────────
    input  logic                  i_gpr_we,
    input  logic                  i_spr_we,
    input  logic                  i_flag_we,
    input  logic [3:0]            i_spr_sel,
    input  logic [31:0]           i_wb_value,        // GPR/SPR write datum (a dual write's primary value)
    input  logic [31:0]           i_wb_value_aux,    // a dual write's second (aux) value
    input  logic [3:0]            i_flag_value,      // NZCV, packed as SR[3:0]
    input  logic [SB_IDX_W-1:0]   i_phys_dst,
    input  logic [SB_IDX_W-1:0]   i_phys_dst_aux,
    input  logic                  i_phys_dst_aux_en, // set only for a dual-destination write (aux dst present)
    input  logic [31:0]           i_pc,              // committing instruction's PC (EPC source on a fault)
    input  logic                  i_valid,           // 0 = bubble
    input  logic                  i_fault_pending,
    input  logic [3:0]            i_fault_vec,

    // ── Back-pressure (to MEM): the dual-write second-write hold ──
    output logic                  o_stall,

    // ── Retire pulse (perfctr): one per distinct instruction ─────
    // High when an instruction reaches its commit this cycle, counted exactly
    // once. Distinct from "a valid slot occupies WB" (o_retire_valid): a dual
    // write's second (aux) cycle holds the same slot but commits no new
    // instruction — it was already pulsed on its primary cycle — so it is
    // excluded here.
    output logic                  o_insn_committed,

    // ── Regfile write port (regfile is external) ─────────────────
    output logic [SB_IDX_W-1:0]   o_wr_idx,
    output logic [31:0]           o_wr_data,
    output logic                  o_wr_en,

    // ── Flag + SPR write strobes (status_reg / SPR modules) ──────
    output logic                  o_flag_we,
    output logic [3:0]            o_flag_value,
    output logic                  o_spr_we,
    output logic [3:0]            o_spr_sel,
    output logic [31:0]           o_spr_value,

    // ── Fault commit (to the exception unit: flush + save-state) ─
    output logic                  o_fault_commit,    // a faulting instruction is committing this cycle
    output logic [3:0]            o_fault_vec,       // its vector number
    output logic [31:0]           o_fault_pc         // its PC (→ EPC)
);

    // ── Fault commit: take the fault at the commit point ─────────
    // WB is the commit point, and in-order completion makes the WB slot the
    // oldest in flight — so a faulting instruction here is the oldest faulting
    // one, and taking its fault is automatically precise. The faulting
    // instruction must be *inert*: every architectural write is gated off, and
    // o_fault_commit launches the flush + save-state instead. can_commit is the
    // "this instruction is allowed to write" qualifier the strobes below use.
    logic can_commit;
    assign can_commit     = i_valid && !i_fault_pending;
    assign o_fault_commit = i_valid && i_fault_pending;

    assign o_fault_vec = i_fault_vec;
    assign o_fault_pc  = i_pc;

    // A dual-destination write — an instruction that commits two registers
    // through the single write port over two cycles. Its aux-dst enable
    // marks it (and implies gpr_we); it occupies WB for both cycles. A
    // faulting slot is inert (can_commit = 0), so it never enters the sequence.
    logic wb_dual_write;
    assign wb_dual_write = can_commit & i_phys_dst_aux_en;

    // ── Flag + SPR write strobes ─────────────────────────────────
    // Passive strobes to the external SR / SPR storage — no local state.
    // (a dual write holds flag_we across both its WB cycles, re-writing the
    // same NZCV; idempotent — and it has spr_we = 0, so the SPR strobe is
    // quiet across both.)
    assign o_flag_value = i_flag_value;
    assign o_flag_we    = can_commit & i_flag_we;
    assign o_spr_sel    = i_spr_sel;
    assign o_spr_value  = i_wb_value;
    assign o_spr_we     = can_commit & i_spr_we;

    // ── Regfile write port + dual-write sequencing ───────────────
    // A normal commit is one GPR write. A dual write replaces it with a
    // two-cycle primary-then-aux sequence through the same port, holding MEM
    // one extra cycle. writing_aux is the FF that tells the two cycles apart.
    logic [SB_IDX_W-1:0] wr_idx;
    logic [31:0]         wr_data;
    logic                wr_en;
    logic                writing_aux;
    logic                next_writing_aux;

    always_ff @(posedge i_clk) begin
        if (i_rst) writing_aux <= 1'b0;
        else       writing_aux <= next_writing_aux;
    end

    always_comb begin
        // Normal single GPR commit (also a dual write's first/primary cycle:
        // its primary dst / value are i_phys_dst / i_wb_value, the defaults
        // below).
        if (!writing_aux) begin
            wr_idx          = i_phys_dst;
            wr_data         = i_wb_value;
            wr_en           = can_commit & i_gpr_we;
            o_stall         = wb_dual_write;
            next_writing_aux = wb_dual_write;
        end else begin
            wr_idx          = i_phys_dst_aux;
            wr_data         = i_wb_value_aux;
            wr_en           = can_commit & i_gpr_we;
            o_stall         = 1'b0;
            next_writing_aux = 1'b0;
        end
    end

    assign o_wr_idx  = wr_idx;
    assign o_wr_data = wr_data;
    assign o_wr_en   = wr_en;

    // A distinct instruction commits when a valid slot is at WB and this is not
    // the dual-write continuation (writing_aux) — the second register write of
    // an instruction already counted on its primary cycle. Faulting slots still
    // count (they complete via the fault path and never dual-write).
    assign o_insn_committed = i_valid & ~writing_aux;

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // A faulting commit takes the fault and writes nothing: the fault-commit
    // pulse and a regfile write are mutually exclusive in the same cycle.
    always_comb begin
        assert (!(o_fault_commit && o_wr_en))
            else $error("penumbra2_wb_stage: faulting instruction also wrote a register");
    end

    // o_stall is the dual-write second-write hold only; nothing else back-
    // pressures MEM from WB.
    always_comb begin
        assert (!o_stall || wb_dual_write)
            else $error("penumbra2_wb_stage: stall asserted outside a dual-destination writeback");
    end

    // The second-write cycle only happens while a dual write occupies WB; if
    // MEM were not held, the aux write would land on some other
    // instruction's destination.
    always_comb begin
        assert (!writing_aux || wb_dual_write)
            else $error("penumbra2_wb_stage: aux write without a dual write in WB");
    end

endmodule
