// Penumbra B-Mux — selects the B-bus source for the ALU
//
// Controlled by b_mux_sel[1:0] from the micro-word:
//   00 = register port B output (normal register operand)
//   01 = immediate from IR (via immediate extractor)
//   10 = constant 4 (used for stack adjust: SSP - 4)
//   11 = constant 8 (used for stack adjust: SSP - 8)
//
// Purely combinational — a 4:1 multiplexer.
// In discrete: eight 74x153 chips (dual 4:1 mux × 32 bits = 16 muxes).

module bmux (
    input  logic [31:0] i_reg_b,    // Register file read port B output
    input  logic [31:0] i_imm32,    // Extended immediate (from imm_ext)
    input  logic [1:0]  i_sel,      // b_mux_sel from micro-word

    output logic [31:0] o_b_bus     // B-bus → ALU input B
);

    always_comb begin
        case (i_sel)
            2'b00:   o_b_bus = i_reg_b;
            2'b01:   o_b_bus = i_imm32;
            2'b10:   o_b_bus = 32'd4;
            2'b11:   o_b_bus = 32'd8;
            default: o_b_bus = 32'b0;
        endcase
    end

endmodule
