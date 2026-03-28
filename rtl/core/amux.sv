// Penumbra A-Bus Source Mux — selects A-bus source
//
// Controlled by a_src[1:0] from the micro-word:
//   00 = register file read port A (normal operand)
//   01 = ESR (exception SR — saved at exception entry)
//   10 = EPC (exception PC — saved at exception entry)
//   11 = vector_addr (exception vector address)
//
// Purely combinational — a 4:1 multiplexer.

module amux (
    input  logic [31:0] i_reg_a,       // Register file read port A output
    input  logic [31:0] i_esr,         // Exception SR (ESR) — saved at exception entry
    input  logic [31:0] i_epc,         // Exception PC (EPC) — saved at exception entry
    input  logic [31:0] i_vector_addr, // Exception vector address
    input  logic [1:0]  i_sel,         // a_src from micro-word

    output logic [31:0] o_a_bus        // A-bus output
);

    always_comb begin
        case (i_sel)
            2'b00:   o_a_bus = i_reg_a;
            2'b01:   o_a_bus = i_esr;
            2'b10:   o_a_bus = i_epc;
            2'b11:   o_a_bus = i_vector_addr;
            default: o_a_bus = 32'b0;
        endcase
    end

endmodule
