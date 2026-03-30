// Smoke-test module: trivial 32-bit adder
// Used only to verify the Verilator toolchain works end-to-end.
module smoke_adder (
    input  logic [31:0] i_a,
    input  logic [31:0] i_b,
    output logic [31:0] o_sum,
    output logic        o_carry
);

    assign {o_carry, o_sum} = i_a + i_b;

endmodule
