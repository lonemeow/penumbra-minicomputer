// penumbra3_alu -- Penumbra/3 EX-stage compute unit.
//
// Combinational 32-bit ALU: result = f(a, b, op), with ARM-style NZCV flag
// generation. Purely combinational -- no clock, no state. MUL/DIV are not
// here; they run on the shared divmul peer unit and reach the regfile
// through the writeback mux.
//
// The twelve single-cycle functions are selected by alu_op (penumbra3_pkg,
// ALU_*). SUB and SBC share the adder by inverting B and adding a carry-in
// (A + ~B + cin = A - B for cin=1). ADC/SBC take the carry-in from the
// forwarded SR carry flag; ADD/SUB force it. MOV is realised as ALU_PASS
// (pass operand B; the operand mux puts the moved value there).

module penumbra3_alu
    import penumbra3_pkg::*;
(
    input  logic [31:0] i_a,
    input  logic [31:0] i_b,
    input  alu_op_e     i_op,
    input  logic        i_carry_in,   // SR carry, used by ADC/SBC

    output logic [31:0] o_result,
    output logic        o_flag_z,     // result == 0
    output logic        o_flag_n,     // result[31]
    output logic        o_flag_c,     // carry-out (ARM: C = NOT borrow on SUB)
    output logic        o_flag_v      // signed overflow
);

    // -- Adder with subtract support --------------------------------
    // SUB/SBC invert B and add a carry-in; ADD/ADC do not.
    //   sub_mode ? A + ~B + cin  :  A + B + cin
    logic        sub_mode;
    logic [31:0] b_eff;        // B after conditional inversion
    logic        cin;
    logic [32:0] adder_full;   // 33-bit to capture carry-out
    logic [31:0] adder_result;
    logic        adder_cout;

    assign sub_mode = (i_op == ALU_SUB) || (i_op == ALU_SBC);
    assign b_eff    = i_b ^ {32{sub_mode}};   // XOR with all-1s = bitwise NOT

    // Carry-in: ADD=0, SUB=1 (completes two's complement), ADC/SBC take the
    // forwarded SR carry (ARM: A + ~B + C = A - B - !C for SBC).
    always_comb begin
        case (i_op)
            ALU_ADC: cin = i_carry_in;
            ALU_SBC: cin = i_carry_in;
            default: cin = sub_mode;
        endcase
    end

    assign adder_full   = {1'b0, i_a} + {1'b0, b_eff} + {32'b0, cin};
    assign adder_result = adder_full[31:0];
    assign adder_cout   = adder_full[32];

    // -- Shifts -----------------------------------------------------
    // B[4:0] is the shift amount; the carry is the last bit shifted out
    // (0 when the shift amount is 0).
    logic [4:0]  shamt;
    logic [31:0] shl_result;
    logic [31:0] shr_result;
    logic [31:0] sar_result;
    logic        shl_carry;
    logic        shr_carry;

    assign shamt      = i_b[4:0];
    assign shl_result = i_a << shamt;
    assign shr_result = i_a >> shamt;
    assign sar_result = $signed(i_a) >>> shamt;
    assign shl_carry  = (shamt == 0) ? 1'b0 : i_a[32 - shamt];
    assign shr_carry  = (shamt == 0) ? 1'b0 : i_a[shamt - 1];

    // -- Result + carry select --------------------------------------
    logic [31:0] result_mux;
    logic        carry_mux;

    always_comb begin
        case (i_op)
            ALU_ADD, ALU_SUB,
            ALU_ADC, ALU_SBC: begin result_mux = adder_result; carry_mux = adder_cout; end
            ALU_AND:  begin result_mux = i_a & i_b;   carry_mux = 1'b0; end
            ALU_OR:   begin result_mux = i_a | i_b;   carry_mux = 1'b0; end
            ALU_XOR:  begin result_mux = i_a ^ i_b;   carry_mux = 1'b0; end
            ALU_SHL:  begin result_mux = shl_result;  carry_mux = shl_carry; end
            ALU_SHR:  begin result_mux = shr_result;  carry_mux = shr_carry; end
            ALU_SAR:  begin result_mux = sar_result;  carry_mux = shr_carry; end
            ALU_PASS: begin result_mux = i_b;         carry_mux = 1'b0; end
            ALU_NOT:  begin result_mux = ~i_b;        carry_mux = 1'b0; end
            default:  begin result_mux = 32'b0;       carry_mux = 1'b0; end
        endcase
    end

    // -- Outputs and flags ------------------------------------------
    assign o_result = result_mux;
    assign o_flag_z = ~|o_result;
    assign o_flag_n = o_result[31];
    assign o_flag_c = carry_mux;
    // V -- signed overflow. Only the add/subtract family produces a meaningful
    // overflow; every other op clears V.
    assign o_flag_v = (i_op == ALU_ADD || i_op == ALU_SUB ||
                       i_op == ALU_ADC || i_op == ALU_SBC)
                    ? (i_a[31] == b_eff[31]) && (o_result[31] != i_a[31])
                    : 1'b0;

endmodule
