// Penumbra MUL/DIV peer unit (divmul) — sequential 32-cycle multiply/divide
//
// A *peer* to the ALU (see doc/internals/divmul.md), not part of it: it shares
// the register-bus fabric but has its own start/busy handshake, so the ALU
// stays single-cycle and off this unit's multi-cycle critical path.
//
// Protocol (matches the ALU's existing i_start/o_busy multi-cycle contract,
// so the sequencer drives it through the same alu_start / alu_busy plumbing):
//   - Pulse i_start for one cycle with i_op and operands valid. The unit
//     latches them and begins iterating; o_busy asserts the next cycle.
//   - o_busy holds high while iterating; its falling edge means the results
//     are valid. The sequencer STALLs on o_busy.
//   - o_fault asserts on a divide fault (DIV0 or narrowing overflow). The unit
//     does not iterate; the sequencer raises VEC_ARITH instead of writing back.
//
// Both result halves are exposed continuously once o_busy falls; the datapath
// muxes which one onto the R-bus during each of the two writeback cycles.
//
// Operand roles (per divmul.md):
//   i_a   = multiplier    (MUL) / dividend low  (DIV)
//   i_b   = multiplicand  (MUL) / divisor       (DIV)
//   i_rdh = dividend high (DIV; 0 for plain 32/32). Unused by MUL.
//
// Op encoding reuses the ALU op field (penumbra_pkg::ALU_MUL/MULU/DIV/DIVU),
// so no new micro-word field is needed.
//
// Coverage: signed + unsigned MULTIPLY, and UNSIGNED DIVIDE (DIVU) with DIV0 /
// narrowing-overflow faults. Signed divide (ALU_DIV) is the next increment —
// it needs the dividend-sign convention for the plain 32/32 case resolved.
//
// The divide uses restoring division with an immediate select (keep the
// subtract result, or the pre-subtract value, based on a 33-bit compare). Same
// 32-cycle count as non-restoring and no restore penalty — the choice is a
// combinational mux, not an extra cycle — and it sidesteps the partial-
// remainder sign bookkeeping that makes non-restoring error-prone. (divmul.md
// sketches non-restoring; this is the deviation, equivalent in cycles.)

module divmul
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Operands and operation ───────────────────────────────────
    input  logic [31:0] i_a,        // multiplier / dividend low
    input  logic [31:0] i_b,        // multiplicand / divisor
    input  logic [31:0] i_rdh,      // dividend high half (DIV; 0 for 32/32)
    input  logic [4:0]  i_op,       // ALU_MUL / ALU_MULU / ALU_DIV / ALU_DIVU
    input  logic        i_start,    // 1-cycle pulse: latch operands, begin

    // ── Status ───────────────────────────────────────────────────
    output logic        o_busy,     // high while iterating
    output logic        o_fault,    // divide fault → VEC_ARITH

    // ── Results (both halves valid once o_busy falls) ────────────
    output logic [31:0] o_result_lo,// Rd  : product low  / quotient
    output logic [31:0] o_result_hi,// Rdh : product high / remainder
    output logic        o_flag_z,   // low half / quotient == 0
    output logic        o_flag_n    // low half / quotient [31]
);

    // ── Operation decode (combinational, sampled at i_start) ─────
    logic is_mul, is_divu, is_signed_mul;
    always_comb begin
        is_mul        = (i_op == ALU_MUL) || (i_op == ALU_MULU);
        is_divu       = (i_op == ALU_DIVU);    // ALU_DIV (signed) → next increment
        is_signed_mul = (i_op == ALU_MUL);
    end

    // ── Divide fault detect (combinational, sampled at i_start) ──
    //   DIV0     : divisor == 0
    //   overflow : dividend high half >= divisor — the 32-bit quotient won't
    //              fit. Always false for plain 32/32 (i_rdh == 0, i_b != 0).
    logic div_fault;
    always_comb begin
        div_fault = is_divu && ((i_b == 32'd0) || (i_rdh >= i_b));
    end

    // ── Sign-magnitude wrapper for signed multiply (load side) ───
    // Iterate on absolute values; remember whether the product is negative.
    logic        a_neg, b_neg;
    logic [31:0] a_mag, b_mag;
    always_comb begin
        a_neg = is_signed_mul && i_a[31];
        b_neg = is_signed_mul && i_b[31];
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
    logic        op_is_div;   // latched: selects the iteration step
    logic        result_neg;  // latched: negate 64-bit product (signed MUL)
    logic        fault_q;     // latched: divide fault, no writeback

    // ── Iteration step candidates (combinational) ───────────────
    // Both are computed every cycle; op_is_div selects which feeds accum.

    // Multiply: conditional add of the multiplicand into the high half, then
    // shift {carry, hi, lo} right by one (LSB-first partial-product walk). The
    // 33-bit sum keeps the carry so it lands in the product MSB after the shift.
    logic [31:0] mul_addend;
    logic [32:0] mul_sum;
    logic [63:0] mul_next;
    always_comb begin
        mul_addend = accum[0] ? op_b : 32'd0;
        mul_sum    = {1'b0, accum[63:32]} + {1'b0, mul_addend};
        mul_next   = {mul_sum, accum[31:1]};   // {carry,hi}(33) ++ lo[31:1](31), == >>1
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
            state      <= S_IDLE;
            accum      <= 64'd0;
            op_b       <= 32'd0;
            iter       <= 6'd0;
            op_is_div  <= 1'b0;
            result_neg <= 1'b0;
            fault_q    <= 1'b0;
        end else begin
            case (state)
                S_IDLE: begin
                    if (i_start && (is_mul || is_divu)) begin
                        if (is_divu && div_fault) begin
                            // Faulting divide: don't iterate. o_busy stays low,
                            // o_fault asserts → sequencer raises VEC_ARITH.
                            fault_q <= 1'b1;
                        end else begin
                            fault_q    <= 1'b0;
                            op_is_div  <= is_divu;
                            op_b       <= is_divu ? i_b : b_mag;
                            // MUL: accum_lo = multiplier magnitude, hi = 0.
                            // DIV: accum = {dividend high, dividend low}.
                            accum      <= is_divu ? {i_rdh, i_a} : {32'd0, a_mag};
                            result_neg <= is_divu ? 1'b0 : (a_neg ^ b_neg);
                            iter       <= 6'd32;
                            state      <= S_ITER;
                        end
                    end
                end
                S_ITER: begin
                    accum <= accum_next;
                    iter  <= iter - 6'd1;
                    if (iter == 6'd1)
                        state <= S_IDLE;   // 32nd step completes this cycle
                end
                default: state <= S_IDLE;
            endcase
        end
    end

    // ── Outputs ──────────────────────────────────────────────────
    // o_busy falls the cycle after the final step writes accum.
    assign o_busy = (state != S_IDLE);

    // Signed multiply negates the 64-bit product when operand signs differ.
    // Unsigned multiply and divide leave result_neg == 0.
    logic [63:0] result;
    assign result = result_neg ? (~accum + 64'd1) : accum;

    assign o_result_lo = result[31:0];   // product low  / quotient
    assign o_result_hi = result[63:32];  // product high / remainder
    assign o_flag_z    = (result[31:0] == 32'd0);
    assign o_flag_n    = result[31];

    assign o_fault = fault_q;

endmodule
