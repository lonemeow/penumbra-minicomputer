// Penumbra MUL/DIV peer unit (divmul) — sequential multiply/divide
//
// A *peer* to the ALU (see doc/internals/divmul.md), not part of it: it shares
// the register-bus fabric but has its own start/done handshake, so the ALU
// stays single-cycle and off this unit's multi-cycle critical path.
//
// o_done, o_fault and the result/flag outputs form one bundle describing the
// most recent operation, valid from its completion cycle until the next
// accepted start. o_done and o_fault are registered so that nothing the
// consumer drives reaches its stall gate combinationally through this unit;
// the consequence is that the bundle still describes the *previous* operation
// on the cycle a start is issued, and the consumer covers that cycle itself.
// Cycle tables: doc/internals/divmul.md, "Completion Signalling".
//
// The datapath muxes the two result halves onto the R-bus during each of the
// two writeback cycles.
//
// Operand roles (per divmul.md):
//   i_a = multiplier   (MUL) / dividend (DIV)
//   i_b = multiplicand (MUL) / divisor  (DIV)
// Divides are 32/32; there is no dividend-high input. The third operand
// (Rdh) is write-only and is the high-half destination only — it is handled
// by the writeback µ-op, not as a hardware input here.
//
// Signedness is pure sign bookkeeping around one unsigned engine:
//   - Signed multiply iterates on |a|,|b| and negates the 64-bit product
//     when the operand signs differ.
//   - Signed divide iterates on |a|,|b| and negates quotient and remainder
//     *independently* — quotient sign = sign(a)^sign(b), remainder sign =
//     sign(a). 64-bit signed divide is software (sign-magnitude over the
//     unsigned 32-bit path).
//
// Op encoding reuses the gen1 core-internal ALU op field — the same
// MUL/MULU/DIV/DIVU values alu.sv decodes and uasm.py emits — so no
// new micro-word field is needed.  These are the microcode ALU field,
// not the ISA R-format opcode, so the constants are defined locally
// below (mirroring alu.sv's local OP_* params).
//
// The divide uses restoring division with an immediate select (keep the
// pre-subtract value, or the subtract result, based on a 33-bit borrow).
// Same 32-cycle count as non-restoring and no restore penalty — the choice
// is a combinational mux, not an extra cycle — and it sidesteps the
// partial-remainder sign bookkeeping that makes non-restoring error-prone.
//
// INT_MIN / -1 is C undefined behaviour; this unit returns INT_MIN (the
// unsigned engine yields 0x80000000, which the sign rules leave unnegated)
// and does not fault. Only DIV0 raises VEC_ARITH.

module divmul
    import penumbra_pkg::*;
#(
    // Multiplier bits retired per iteration step; must divide 32. The divide
    // is unaffected — restoring division retires one quotient bit regardless.
    //
    // Each extra bit costs a gated row and a carry-save level: cheap in LUTs,
    // expensive in chips. One is what discrete logic affords; FPGA cores raise it.
    parameter int MUL_BITS_PER_STEP = 1
) (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Operands and operation ───────────────────────────────────
    input  logic [31:0] i_a,        // multiplier / dividend
    input  logic [31:0] i_b,        // multiplicand / divisor
    input  logic [4:0]  i_op,       // OP_MUL / OP_MULU / OP_DIV / OP_DIVU (ALU field)
    input  logic        i_start,    // 1-cycle pulse: latch operands, begin

    // ── Status ───────────────────────────────────────────────────
    output logic        o_done,     // low while iterating; high from completion
                                    // until the next start (result bundle valid)
    output logic        o_fault,    // valid with o_done: DIV0 → VEC_ARITH

    // ── Results (both halves valid once o_busy falls) ────────────
    output logic [31:0] o_result_lo,// Rd  : product low  / quotient
    output logic [31:0] o_result_hi,// Rdh : product high / remainder
    output logic        o_flag_z,   // low half / quotient == 0
    output logic        o_flag_n    // low half / quotient [31]
);

    localparam int MUL_STEPS = 32 / MUL_BITS_PER_STEP;

    build_check #(
        .COND (32 % MUL_BITS_PER_STEP == 0),
        .MSG  ("divmul: MUL_BITS_PER_STEP must divide 32")
    ) u_check_step_divides_32 ();

    // ── ALU op-field encodings (gen1 core-internal contract) ─────
    // Mirror alu.sv's local OP_* params and uasm.py's alu map.  This is
    // the microcode ALU field, NOT the ISA R-format opcode (OP_R_*).
    localparam logic [4:0] OP_MUL  = 5'b01101;
    localparam logic [4:0] OP_MULU = 5'b01110;
    localparam logic [4:0] OP_DIV  = 5'b01111;
    localparam logic [4:0] OP_DIVU = 5'b10000;

    // ── Operation decode (combinational, sampled at i_start) ─────
    logic is_mul, is_div, is_signed;
    always_comb begin
        is_mul    = (i_op == OP_MUL) || (i_op == OP_MULU);
        is_div    = (i_op == OP_DIV) || (i_op == OP_DIVU);
        is_signed = (i_op == OP_MUL) || (i_op == OP_DIV);
    end

    // ── Divide fault detect: DIV0 only (no narrowing form) ───────
    logic div_fault;
    assign div_fault = is_div && (i_b == 32'd0);

    // ── Sign-magnitude wrapper (load side) ───────────────────────
    // Signed multiply and signed divide both iterate on absolute values.
    logic        a_neg, b_neg;
    logic [31:0] a_mag, b_mag;
    always_comb begin
        a_neg = is_signed && i_a[31];
        b_neg = is_signed && i_b[31];
        a_mag = a_neg ? (~i_a + 32'd1) : i_a;
        b_mag = b_neg ? (~i_b + 32'd1) : i_b;
    end

    // ── State ────────────────────────────────────────────────────
    localparam logic [1:0] S_IDLE = 2'd0;
    localparam logic [1:0] S_ITER = 2'd1;

    logic [1:0]  state;
    logic [63:0] accum;       // {hi, lo}: product (MUL) | {remainder, quotient} (DIV)
    logic [31:0] op_b;        // held: multiplicand magnitude (MUL) | divisor (DIV)
    logic [5:0]  iter;        // down-counter: 32 → 0
    logic        op_is_div;   // latched: selects the iteration step + output mode
    logic        neg_xor;     // latched: sign(a)^sign(b) — product / quotient sign
    logic        neg_a;       // latched: sign(a)         — remainder sign
    logic        done_q;      // drives o_done: low while an operation is in
                              // flight, high from the cycle it completes
    logic        fault_q;     // drives o_fault: set with done_q when the
                              // completed operation was a divide by zero

    // Start on the *rising edge* of i_start. The integrated micro-routine holds
    // i_start high across the whole STALL (it can't pulse for one cycle inside a
    // stalled micro-op), so edge detection keeps that from re-triggering on the
    // cycle the unit finishes.
    logic        i_start_q;
    logic        start_pulse;
    assign start_pulse = i_start & ~i_start_q;

    // final_step is the cycle the last iteration step writes accum, so the
    // results are valid on the cycle *after* it.
    logic        start_accepted, start_faulting, final_step;
    assign start_accepted = start_pulse && (is_mul || is_div) && !div_fault;
    assign start_faulting = start_pulse && div_fault;
    assign final_step     = (state == S_ITER) && (iter == 6'd1);

    // ── Iteration step candidates (combinational) ───────────────
    // Both are computed every cycle; op_is_div selects which feeds accum.

    // Multiply: LSB-first partial-product walk, MUL_BITS_PER_STEP rows per step.
    //
    // The high half plus every row is bounded by (2**32-1)*2**MUL_BITS_PER_STEP,
    // so mul_next's concatenation *is* the right shift — nothing to truncate.
    //
    // Rows reduce in carry-save and resolve through one adder per step. A
    // carry-propagate add per row instead costs an adder delay per bit retired,
    // spending what the extra bits buy. A `*` on the multiplier slice would let
    // the synthesiser build the tree, but it infers DSP blocks — which the
    // discrete build has no counterpart for.
    logic [31+MUL_BITS_PER_STEP:0] mul_ps, mul_pc;    // carry-save partial product
    logic [31+MUL_BITS_PER_STEP:0] mul_sum;
    logic [63:0] mul_next;
    always_comb begin
        // The high half and row 0 are just two addends, so seeding the pair with
        // them costs no reduction level — and one bit per step none at all.
        mul_ps = {{MUL_BITS_PER_STEP{1'b0}}, accum[63:32]};
        mul_pc = accum[0] ? {{MUL_BITS_PER_STEP{1'b0}}, op_b} : '0;
        for (int j = 1; j < MUL_BITS_PER_STEP; j++) begin
            logic [31+MUL_BITS_PER_STEP:0] row, csa_sum, csa_carry;
            row       = accum[j] ? ({{MUL_BITS_PER_STEP{1'b0}}, op_b} << j) : '0;
            csa_sum   = mul_ps ^ mul_pc ^ row;
            csa_carry = ((mul_ps & mul_pc) | (mul_pc & row)
                                           | (mul_ps & row)) << 1;
            mul_ps    = csa_sum;
            mul_pc    = csa_carry;
        end
        mul_sum  = mul_ps + mul_pc;
        mul_next = {mul_sum, accum[31:MUL_BITS_PER_STEP]};
    end

    // Divide (restoring): shift {hi, lo} left by one to form the next partial
    // remainder, then compare against the divisor. If it fits, subtract and set
    // the quotient bit; otherwise keep the pre-subtract value. The compare is
    // 33-bit so a full-width remainder can't alias.
    logic [32:0] rem_shifted;
    logic [32:0] divisor_ext;
    logic        q_bit;
    logic [32:0] rem_diff;
    logic [31:0] new_rem;
    logic [63:0] div_next;
    always_comb begin
        rem_shifted = {accum[63:32], accum[31]};       // (hi << 1) | lo[31]
        divisor_ext = {1'b0, op_b};
        rem_diff    = rem_shifted - divisor_ext;       // 33-bit; bit 32 = borrow
        q_bit       = ~rem_diff[32];                   // no borrow → remainder >= divisor
        new_rem     = q_bit ? rem_diff[31:0] : rem_shifted[31:0];
        div_next    = {new_rem, accum[30:0], q_bit};   // new remainder : (lo << 1 | q)
    end

    logic [63:0] accum_next;
    assign accum_next = op_is_div ? div_next : mul_next;

    // ── Sequential core ──────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state     <= S_IDLE;
            accum     <= 64'd0;
            op_b      <= 32'd0;
            iter      <= 6'd0;
            op_is_div <= 1'b0;
            neg_xor   <= 1'b0;
            neg_a     <= 1'b0;
            done_q    <= 1'b1;   // nothing in flight
            fault_q   <= 1'b0;
            i_start_q <= 1'b0;
        end else begin
            i_start_q <= i_start;

            // A faulting divide takes the same shape as any other operation,
            // differing only in completing at once.
            if (start_accepted) begin
                done_q  <= 1'b0;     // in flight
                fault_q <= 1'b0;
            end else if (start_faulting) begin
                done_q  <= 1'b1;     // nothing to compute — complete at once
                fault_q <= 1'b1;
            end else if (final_step) begin
                done_q  <= 1'b1;
            end

            case (state)
                S_IDLE: begin
                    // A faulting divide loads nothing and stays IDLE.
                    if (start_accepted) begin
                        op_is_div <= is_div;
                        op_b      <= b_mag;            // == i_b when unsigned
                        // accum_lo holds the multiplier/dividend magnitude; hi
                        // starts at 0 (divides are 32/32, no high-half input).
                        accum     <= {32'd0, a_mag};
                        neg_xor   <= a_neg ^ b_neg;    // product / quotient sign
                        neg_a     <= a_neg;            // remainder sign
                        iter      <= is_div ? 6'd32 : 6'(MUL_STEPS);
                        state     <= S_ITER;
                    end
                end
                S_ITER: begin
                    accum <= accum_next;
                    iter  <= iter - 6'd1;
                    if (final_step)
                        state <= S_IDLE;   // 32nd step completes this cycle
                end
                default: state <= S_IDLE;
            endcase
        end
    end

    // ── Outputs ──────────────────────────────────────────────────
    assign o_done = done_q;

    // Multiply negates the whole 64-bit product (borrow propagates lo→hi).
    // Signed divide negates quotient and remainder *independently* — they are
    // two separate numbers, not halves of one. Unsigned ops leave neg_* == 0.
    logic [63:0] prod;
    logic [31:0] quo, rem;
    assign prod = neg_xor ? (~accum + 64'd1)         : accum;
    assign quo  = neg_xor ? (~accum[31:0]  + 32'd1)  : accum[31:0];
    assign rem  = neg_a   ? (~accum[63:32] + 32'd1)  : accum[63:32];

    assign o_result_lo = op_is_div ? quo : prod[31:0];   // quotient / product low
    assign o_result_hi = op_is_div ? rem : prod[63:32];  // remainder / product high
    assign o_flag_z    = (o_result_lo == 32'd0);
    assign o_flag_n    = o_result_lo[31];

    // Held, not pulsed, so a consumer must qualify it with which instruction it
    // is completing — exactly as it does the result halves.
    assign o_fault = fault_q;

    // A completed operation stays completed, so a back-pressured consumer can
    // commit later than the completion cycle.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (done_q && !start_accepted && !start_faulting) |=> done_q)
        else $error("divmul: o_done fell without a start");

    // The fault flag belongs to the operation that set it.
    assert property (@(posedge i_clk) disable iff (i_rst)
        start_accepted |=> !fault_q)
        else $error("divmul: o_fault survived a non-faulting start");

endmodule
