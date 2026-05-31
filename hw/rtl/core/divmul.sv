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
//   - o_fault asserts combinationally from the start cycle on a divide fault;
//     the unit does not iterate and the sequencer raises VEC_ARITH instead of
//     writing back. (Divide + faults arrive in a later increment.)
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
// Increment 1: signed + unsigned MULTIPLY only. Divide is stubbed (o_fault=0,
// the FSM simply ignores DIV ops) and lands in the next increment.

module divmul
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Operands and operation ───────────────────────────────────
    input  logic [31:0] i_a,        // multiplier / dividend low
    input  logic [31:0] i_b,        // multiplicand / divisor
    // i_rdh is the dividend high half — only meaningful for DIV, wired now so
    // the interface is stable for the divide increment.
    // verilator lint_off UNUSEDSIGNAL
    input  logic [31:0] i_rdh,
    // verilator lint_on UNUSEDSIGNAL
    input  logic [4:0]  i_op,       // ALU_MUL / ALU_MULU / ALU_DIV / ALU_DIVU
    input  logic        i_start,    // 1-cycle pulse: latch operands, begin

    // ── Status ───────────────────────────────────────────────────
    output logic        o_busy,     // high while iterating
    output logic        o_fault,    // divide fault → VEC_ARITH (div increment)

    // ── Results (both halves valid once o_busy falls) ────────────
    output logic [31:0] o_result_lo,// Rd  : product low  / quotient
    output logic [31:0] o_result_hi,// Rdh : product high / remainder
    output logic        o_flag_z,   // low half / quotient == 0
    output logic        o_flag_n    // low half / quotient [31]
);

    // ── Operation decode ─────────────────────────────────────────
    // is_signed selects the negate-in / negate-out wrapper around the
    // unsigned magnitude core.
    logic is_mul, is_signed;
    always_comb begin
        is_mul    = (i_op == ALU_MUL) || (i_op == ALU_MULU);
        is_signed = (i_op == ALU_MUL);   // ALU_DIV added in the signed-divide increment
    end

    // ── Sign-magnitude wrapper (load side) ───────────────────────
    // For signed multiply, iterate on absolute values and remember whether
    // the product must be negated (operands of differing sign).
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
    logic [63:0] accum;        // {accum_hi, accum_lo} — shared shift register
    logic [31:0] multiplicand; // held addend for the shift-add core
    logic [5:0]  iter;         // down-counter: 32 → 0
    logic        result_neg;   // latched: negate the 64-bit product on output

    // ── Per-iteration step (combinational) ───────────────────────
    // One shift-add multiply iteration over the magnitude operands.
    logic [63:0] accum_next;
    logic [31:0] accum_next_hi;
    logic        carry;

    always_comb begin
        // The ADD is conditional on the multiplier bit; the SHIFT is not —
        // every iteration shifts {carry, hi, lo} right by one.
        carry         = 1'b0;
        accum_next_hi = accum[63:32];
        if (accum[0]) begin
            {carry, accum_next_hi} = accum[63:32] + multiplicand;
            accum_next = {carry, accum_next_hi, accum[31:1]};
        end else begin
            accum_next = {1'b0, accum[63:1]};
        end
    end

    // ── Sequential core ──────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state        <= S_IDLE;
            accum        <= 64'd0;
            multiplicand <= 32'd0;
            iter         <= 6'd0;
            result_neg   <= 1'b0;
        end else begin
            case (state)
                S_IDLE: begin
                    if (i_start && is_mul) begin
                        // accum_lo holds the multiplier, accum_hi starts at 0;
                        // multiplicand is the held addend.
                        accum        <= {32'd0, a_mag};
                        multiplicand <= b_mag;
                        result_neg   <= a_neg ^ b_neg;
                        iter         <= 6'd32;
                        state        <= S_ITER;
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

    // Negate the full 64-bit product when the signed operands differed in sign.
    logic [63:0] result;
    assign result = result_neg ? (~accum + 64'd1) : accum;

    assign o_result_lo = result[31:0];
    assign o_result_hi = result[63:32];
    assign o_flag_z    = (result[31:0] == 32'd0);
    assign o_flag_n    = result[31];

    // Divide faults arrive with the divide increment.
    assign o_fault = 1'b0;

endmodule
