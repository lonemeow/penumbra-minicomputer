// Penumbra A-Bus Source Mux — selects A-bus source
//
// Controlled by a_src[1:0] from the micro-word:
//   00 = register file read port A (normal operand)
//   01 = shadow_SR (saved SR during exception entry)
//   10 = shadow_PC (saved PC during exception entry)
//   11 = vector_addr (exception vector address)
//
// Purely combinational — a 4:1 multiplexer.

module amux (
    input  logic [31:0] i_reg_a,       // Register file read port A output
    input  logic [31:0] i_shadow_sr,   // Latched SR from exception entry
    input  logic [31:0] i_shadow_pc,   // Latched PC from exception entry
    input  logic [31:0] i_vector_addr, // Exception vector address
    input  logic [1:0]  i_sel,         // a_src from micro-word

    output logic [31:0] o_a_bus        // A-bus output
);

    always_comb begin
        case (i_sel)
            2'b00:   o_a_bus = i_reg_a;
            2'b01:   o_a_bus = i_shadow_sr;
            2'b10:   o_a_bus = i_shadow_pc;
            2'b11:   o_a_bus = i_vector_addr;
            default: o_a_bus = 32'b0;
        endcase
    end

endmodule
