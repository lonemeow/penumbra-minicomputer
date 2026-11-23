// penumbra4_alu — one op's result and flags.
//
// Combinational. C is the adder's carry out, NOT borrow on a subtract, and
// the last bit a shift moves out (clear for a shift by zero); the logic
// ops, the passes and NOT clear C and V (doc/system/instruction-set.md).

module penumbra4_alu
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  alu_fn_e i_fn,
    input  word_t   i_a,
    input  word_t   i_b,
    input  logic    i_carry,        // the C flag, for ADC and SBC
    output word_t   o_result,
    output flags_t  o_flags
);

    logic        subtract;
    word_t       b_in;
    logic        carry_in;
    logic [32:0] sum;
    logic [4:0]  shift;
    logic [4:0]  last_out_left;     // the last bit a left shift moves out
    logic        shift_carry_left;
    logic        shift_carry_right;
    logic        carry;
    logic        overflow;

    assign subtract = (i_fn == ALU_SUB) || (i_fn == ALU_SBC);
    assign b_in     = i_b ^ {32{subtract}};
    assign shift    = i_b[4:0];

    always_comb begin
        if (i_fn == ALU_ADC || i_fn == ALU_SBC) begin
            carry_in = i_carry;
        end else begin
            carry_in = subtract;
        end
    end

    assign sum = {1'b0, i_a} + {1'b0, b_in} + {32'b0, carry_in};

    assign last_out_left     = 5'd0 - shift;
    assign shift_carry_left  = (shift != 5'd0) && i_a[last_out_left];
    assign shift_carry_right = (shift != 5'd0) && i_a[shift - 5'd1];

    always_comb begin
        o_result = '0;
        carry    = 1'b0;
        overflow = 1'b0;
        case (i_fn)
            ALU_ADD, ALU_SUB, ALU_ADC, ALU_SBC: begin
                o_result = sum[31:0];
                carry    = sum[32];
                overflow = (i_a[31] == b_in[31]) && (sum[31] != i_a[31]);
            end
            ALU_AND:  o_result = i_a & i_b;
            ALU_OR:   o_result = i_a | i_b;
            ALU_XOR:  o_result = i_a ^ i_b;
            ALU_SHL: begin
                o_result = i_a << shift;
                carry    = shift_carry_left;
            end
            ALU_SHR: begin
                o_result = i_a >> shift;
                carry    = shift_carry_right;
            end
            ALU_SAR: begin
                o_result = word_t'($signed(i_a) >>> shift);
                carry    = shift_carry_right;
            end
            ALU_PASS: o_result = i_b;
            ALU_NOT:  o_result = ~i_b;
            default: begin
            end
        endcase
    end

    assign o_flags.n = o_result[31];
    assign o_flags.z = (o_result == '0);
    assign o_flags.c = carry;
    assign o_flags.v = overflow;

endmodule
