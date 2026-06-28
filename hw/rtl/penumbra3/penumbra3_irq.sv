// penumbra3_irq -- Penumbra/3 interrupt recognition + EX-frontier injection.
//
// An interrupt is asynchronous -- nobody's fault, tied to no instruction -- and
// need not be taken immediately, so it is taken at the cleanest boundary the
// pipeline offers: the EX frontier. When eligible and the EX slot is a clean,
// committable boundary, this unit pulses o_irq_inject; the EX stage then tags
// that instruction as a synthetic fault, which rides the precise-fault path
// (access suppressed in MEM, register write dropped at WB, save-state EPC <- its
// own PC) and re-executes after ERET. The interrupt thus reuses the whole fault
// machinery -- no drain FSM, no separately captured boundary PC.
//
// Why the EX frontier. EPC must be a real, resolved PC, and no instruction past
// the cut may have committed an irreversible memory side effect. EX is the one
// point that satisfies both: past branch resolution (real PC) and before MEM
// (no access issued). A data access issues only once it is the oldest in flight
// -- strictly older than the EX slot -- so an issued access is never squashed and
// a not-yet-issued one is squashed before its side effect. The non-speculative
// distinction is the EX/MEM boundary itself; injecting here respects it.
//
// ei_shadow. EI enables interrupts with a one-instruction delay: the instruction
// immediately after EI still runs masked. EI sets ei_shadow (and SR.I, in the
// SPR file); the shadow holds until that one next instruction *completes* --
// tracked by its retirement / drain-commit, not by raw cycles -- so a stalled
// shadow instruction still counts as exactly one. Because EI is itself a
// drain-commit, the pipeline is empty when it commits, so "the next instruction"
// is unambiguously the single next one.

module penumbra3_irq
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── IRQ lines (level) ────────────────────────────────────────
    input  logic        i_irq,             // external, wired-OR
    input  logic        i_timer_irq,       // timer (higher priority)

    // ── Pipeline state (from the spine / front end) ──────────────
    input  logic        i_sr_i,            // SR.I (interrupt enable)
    input  logic        i_ei_commit,       // EI committed this cycle (arm ei_shadow)
    input  logic        i_retire_valid,    // an instruction retired at WB
    input  logic        i_dc_commit,       // a drain-commit completed at EX
    input  logic        i_ex_valid,        // the EX slot holds a real instruction
    input  logic        i_ex_stall,        // EX is back-pressured (older access / divmul / drain-commit hold)
    input  logic        i_fault_commit,    // a fault committed this cycle (preempts the entry)
    input  logic        i_vecf_active,     // a vector fetch is already in progress

    // ── Outputs ──────────────────────────────────────────────────
    output logic        o_irq_inject,      // tag the EX instruction as an interrupt this cycle
    output logic [3:0]  o_irq_vec          // VEC_TIMER / VEC_EXT_IRQ
);

    // ── EI one-instruction shadow ────────────────────────────────
    // The shadow flip-flop:
    //   - sets when i_ei_commit (EI just committed);
    //   - clears once the single instruction after EI completes -- the next
    //     i_retire_valid (normal/trap) or i_dc_commit (a drain-commit shadowed
    //     instruction like ERET, which never retires at WB);
    //   - otherwise holds; resets to 0.
    // The set wins over the clear on EI's own commit cycle (EI's drain-commit
    // pulse coincides with i_ei_commit) -- the branch order arms the shadow
    // rather than immediately clearing it.
    logic ei_shadow;
    always_ff @(posedge i_clk) begin
        if (i_rst)
            ei_shadow <= 1'b0;
        else if (i_ei_commit)
            ei_shadow <= 1'b1;
        else if (i_retire_valid || i_dc_commit)
            ei_shadow <= 1'b0;
    end

    // ── Recognition ──────────────────────────────────────────────
    // An IRQ is eligible when a line is asserted, SR.I = 1, and ei_shadow = 0.
    // Timer outranks the external line.
    logic       irq_pending;
    logic       eligible;
    assign irq_pending = i_irq | i_timer_irq;
    assign eligible    = irq_pending & i_sr_i & ~ei_shadow;
    assign o_irq_vec   = i_timer_irq ? VEC_TIMER : VEC_EXT_IRQ;   // timer outranks external

    // ── EX-frontier inject ───────────────────────────────────────
    // Take the interrupt by tagging the EX instruction -- the EX stage turns this
    // into a synthetic fault -- when it is eligible and the EX slot is a clean
    // boundary to cut at:
    //   i_ex_valid      a real instruction to pin EPC to and squash;
    //   ~i_ex_stall     no older access still in flight, so older work commits
    //                   ahead and EPC / the saved SR land on a clean boundary;
    //   ~i_dc_commit    not across a committing EI/DI/ERET/WRSYS, keeping
    //                   SR.I / ei_shadow precise (a committing DI masks the IRQ);
    //   ~i_fault_commit a fault preempts -- it is flushing this slot;
    //   ~i_vecf_active  no entry already under way.
    assign o_irq_inject = eligible
                        & i_ex_valid
                        & ~i_ex_stall
                        & ~i_dc_commit
                        & ~i_fault_commit
                        & ~i_vecf_active;

    // ══════════════════════════════════════════════════════════
    // Assertions -- sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // The inject pulse only fires on a clean, committable EX boundary -- never
    // an empty slot, a stalled slot, or coincident with a fault commit.
    always_comb begin
        assert (!o_irq_inject || (i_ex_valid && !i_ex_stall && !i_fault_commit))
            else $error("penumbra3_irq: inject fired on an unclean EX boundary");
    end

endmodule
