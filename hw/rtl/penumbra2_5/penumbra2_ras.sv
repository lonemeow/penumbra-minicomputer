// penumbra2_ras — Penumbra/2.5 return-address stack.
//
// A gen2.5-only leaf (like penumbra2_predict): it lives in hw/rtl/penumbra2_5/
// and is instantiated by the gen2.5 ID-stage fork. gen2 has no RAS — this file
// is simply not part of gen2's fileset.
//
// The BTFN predictor handles *direct* branches, whose target EX can recompute
// (PC + imm), so a direction guess suffices. A function return is a different
// animal: `JMP R13` is an *indirect* branch whose target — the caller's return
// address — is not in the instruction word. BTFN cannot touch it, and on real
// (kernel) code returns dominate the front-end flush bucket.
//
// The RAS predicts return targets the way returns actually behave: as the pop
// of a call/return stack. On a call (BL / JALR — anything that links the return
// address into R13) the ID stage pushes that return address. On a return
// (JMP R13) it pops, and the popped value is the predicted target; the spine
// steers fetch there at ID over the same 2-bubble path BTFN uses. EX stays the
// authority — it compares its resolved R13 target against the prediction and
// redirects on a miss — so a wrong guess (recursion past DEPTH, setjmp/longjmp,
// hand-rolled returns) costs an extra flush, never a wrong result.
//
// Coherence is the integration's job, not this leaf's: the stack mutates only
// on i_push/i_pop, which the ID stage asserts only for a true-path, non-faulting
// call or return that actually issues. A branch mispredict flushes its
// wrong-path successor combinationally before that successor can issue, so it
// never reaches the stack (the short ID→EX resolve distance buys this). The one
// residual is a precise fault, which squashes an already-issued shadow of up to
// two younger instructions: those may have moved the stack and will re-run after
// the handler, leaving a bounded, self-healing pointer drift. EX is the branch
// authority, so that drift only ever costs a few extra return flushes — never a
// wrong target — and faults are rare enough per instruction that no
// checkpoint/restore machinery earns its logic. See penumbra2_id_stage.
//
// See doc/internals/penumbra2/overview.md and doc/TODO.md (gen2.5 roadmap).

module penumbra2_ras #(
    // Call-nesting depth tracked. A nest deeper than DEPTH mispredicts on the
    // outermost returns; it never misbehaves. Kept small (flop-backed) — the
    // discrete-logic rebuild pays for every entry. Must be a power of two: the
    // wrapping `top` makes the physical ring 2**PTR_W deep, so a non-power-of-two
    // DEPTH would desync the overwrite-oldest accounting.
    parameter int DEPTH = 8
) (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── ID-stage events (asserted only when the ID slot issues) ──
    input  logic        i_push,       // a call issued — remember its return address
    input  logic [31:0] i_link_addr,  // the return address to push (= the call's next_pc)
    input  logic        i_pop,        // a return issued — consume the top of stack

    // ── Prediction for the return currently in ID ───────────────
    output logic        o_valid,      // the stack holds an entry to predict from
    output logic [31:0] o_target      // predicted return target (newest entry)
);

    localparam int PTR_W = $clog2(DEPTH);

    // Circular entry store. `top` indexes the most-recent push (what a pop
    // returns). `count` is the number of live entries, saturating at DEPTH so
    // o_valid never claims a prediction the stack does not actually hold.
    logic [31:0]      stack [DEPTH];
    logic [PTR_W-1:0] top;
    logic [PTR_W:0]   count;

    // A return is predictable only with at least one live entry; its target is
    // the newest one. Combinational — available the cycle the return is in ID.
    assign o_valid  = (count != 0);
    assign o_target = stack[top];

    // ── Stack update ─────────────────────────────────────────────
    // Convention the outputs above assume: `top` points at the most-recent
    // entry, so a push advances `top` (with wrap) and writes i_link_addr at the
    // new top. push and pop are mutually exclusive — an instruction is a call or
    // a return, never both (jalr r13 is classed as a call only). `top` resets to
    // all-ones so the first push (advance-then-write) lands at index 0.
    logic [PTR_W-1:0] next_top;
    assign next_top = top + 1'b1;
    
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            top   <= '1;
            count <= '0;
        end else begin
            // push and pop are mutually exclusive (a slot is a call or a
            // return, never both). A push past a full ring overwrites the
            // oldest entry — `next_top` wraps and `count` holds at DEPTH — so
            // deep nests lose their outermost frames, never the hot innermost
            // ones. An empty-ring pop is a no-op: o_valid is already 0, so the
            // return simply falls back to the EX redirect.
            if (i_push) begin
                top             <= next_top;
                stack[next_top] <= i_link_addr;
                if (count < DEPTH[PTR_W:0])
                    count <= count + 1'd1;
            end else if (i_pop) begin
                if (count > 0) begin
                    top   <= top - 1'd1;
                    count <= count - 1'd1;
                end
            end
        end
    end

    // ── Assertions (sim-only; stripped at synth) ─────────────────
    // A slot is a call or a return, never both — the ID detection must keep
    // i_push and i_pop disjoint.
    always_comb
        if (i_push & i_pop)
            $error("penumbra2_ras: simultaneous push and pop");

    // `count` must never exceed the stack depth — a miswired saturate would
    // make o_valid promise an entry that does not exist.
    always_ff @(posedge i_clk)
        if (!i_rst)
            assert (count <= DEPTH[PTR_W:0])
                else $error("penumbra2_ras: count exceeds DEPTH");

endmodule
