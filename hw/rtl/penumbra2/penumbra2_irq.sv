// penumbra2_irq — Penumbra/2 interrupt recognition + drain-and-take.
//
// Interrupts are asynchronous: not tied to any instruction, recognized at the
// fetch boundary, and taken by *draining* rather than flushing (the in-flight
// stream completes — an interrupt is nobody's fault, so nothing is discarded).
// This unit owns that path, plus the EI one-instruction enable delay.
//
// Recognition. An IRQ is eligible when a line is asserted, SR.I = 1, and
// ei_shadow = 0. Timer outranks the external line. On recognition the unit
// stops the front end issuing (o_fetch_stop freezes IF1's PC at the boundary
// and bubbles its output) and latches that boundary PC as the EPC and the
// vector. It then waits for the pipeline to drain (no in-flight instruction),
// and pulses o_irq_entry — which the integration turns into a save-state
// (EPC <- boundary, no flush) and a vector-fetch launch, reusing the same
// machinery a fault uses for entry. A fault committing during the drain
// preempts the interrupt: it flushes and vectors itself, and the IRQ stays
// pending (its line is still asserted) to be recognized after the handler.
//
// ei_shadow. EI enables interrupts with a one-instruction delay: the
// instruction immediately after EI still runs masked. EI sets ei_shadow (and
// SR.I, in the SPR file); the shadow holds until that one next instruction
// *completes* — tracked by its retirement / drain-commit, not by raw cycles,
// so a stalled shadow instruction still counts as exactly one. Because EI is
// itself drain-commit, the pipeline is empty when it commits, so "the next
// instruction" is unambiguously the single next one.

module penumbra2_irq
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
    input  logic        i_pipe_busy,       // any instruction in flight (IF2..WB)
    input  logic [31:0] i_boundary_pc,     // next-fetch PC (EPC for the interrupt)
    input  logic        i_fault_commit,    // a fault committed (preempts the entry)
    input  logic        i_vecf_active,     // a vector fetch is already in progress

    // ── Outputs ──────────────────────────────────────────────────
    output logic        o_fetch_stop,      // freeze IF1 (PC + bubble) during recognition/drain
    output logic        o_irq_entry,       // pulse: take the interrupt now
    output logic [3:0]  o_irq_vec,         // VEC_TIMER / VEC_EXT_IRQ
    output logic [31:0] o_irq_epc          // boundary PC to save
);

    // ── EI one-instruction shadow ────────────────────────────────
    logic ei_shadow;
    // The shadow flip-flop:
    //   - sets when i_ei_commit (EI just committed);
    //   - clears once the single instruction after EI completes — the next
    //     i_retire_valid (normal/trap) or i_dc_commit (a drain-commit shadowed
    //     instruction like ERET, which never retires at WB);
    //   - otherwise holds; resets to 0.
    // The set wins over the clear on EI's own commit cycle (EI's drain-commit
    // pulse coincides with i_ei_commit) — the branch order arms the shadow
    // rather than immediately clearing it.
    always_ff @(posedge i_clk) begin
        if (i_rst)
            ei_shadow <= 1'b0;
        else if (i_ei_commit)
            ei_shadow <= 1'b1;
        else if (i_retire_valid || i_dc_commit)
            ei_shadow <= 1'b0;
    end

    // ── Recognition ──────────────────────────────────────────────
    logic       irq_pending;
    logic       eligible;
    logic [3:0] irq_vec_now;
    assign irq_pending = i_irq | i_timer_irq;
    assign eligible    = irq_pending & i_sr_i & ~ei_shadow;
    assign irq_vec_now = i_timer_irq ? VEC_TIMER : VEC_EXT_IRQ;   // timer outranks external

    // ── Drain-and-take FSM ───────────────────────────────────────
    localparam logic S_IDLE  = 1'b0;
    localparam logic S_DRAIN = 1'b1;

    logic       state, next_state;
    logic [3:0] vec_q;
    logic [31:0] epc_q;

    // Start an entry: eligible, the pipeline isn't already taking an exception,
    // and no fault is committing this cycle. Latched into the drain below.
    logic start;
    assign start = (state == S_IDLE) & eligible & ~i_vecf_active & ~i_fault_commit;

    // Drained = nothing left in flight. The boundary instruction was never
    // issued (fetch stopped at recognition), so this is the clean boundary.
    logic drained;
    assign drained = ~i_pipe_busy;

    always_comb begin
        next_state = state;
        case (state)
            S_IDLE:  if (start) next_state = S_DRAIN;
            // A fault preempts (it flushes + vectors itself); otherwise take
            // the interrupt the cycle the pipe empties.
            S_DRAIN: if (i_fault_commit || drained) next_state = S_IDLE;
            default: next_state = S_IDLE;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state <= S_IDLE;
        end else begin
            state <= next_state;
            if (start) begin
                vec_q <= irq_vec_now;
                epc_q <= i_boundary_pc;     // freeze the boundary as EPC
            end
        end
    end

    // Stop fetching from recognition (start) through the whole drain, so no
    // instruction past the boundary is issued.
    assign o_fetch_stop = start | (state == S_DRAIN);
    // Take it the cycle the pipe is empty and no fault preempted.
    assign o_irq_entry  = (state == S_DRAIN) & drained & ~i_fault_commit;
    assign o_irq_vec    = vec_q;
    assign o_irq_epc    = epc_q;

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // The entry pulse only fires from a drained DRAIN — never with the pipe
    // still busy (that would save a racing in-flight instruction's boundary).
    always_comb begin
        assert (!o_irq_entry || (state == S_DRAIN && !i_pipe_busy))
            else $error("penumbra2_irq: entry fired with the pipeline not drained");
    end

    // A fault and an interrupt entry are mutually exclusive — the fault
    // preempts, so the entry must not coincide with a fault commit.
    always_comb begin
        assert (!(o_irq_entry && i_fault_commit))
            else $error("penumbra2_irq: interrupt entry coincided with a fault commit");
    end

endmodule
