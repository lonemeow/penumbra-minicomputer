// Penumbra PC Source Mux — selects next PC value
//
// Controlled by pc_src[2:0] from the micro-word:
//   000 = hold (PC unchanged — used during multi-cycle ops)
//   001 = PC + 4 (next sequential instruction)
//   010 = PC + offset (branch target, offset from PC adder)
//   011 = A-bus (indirect jump: JMP Rs)
//   100 = MDR (exception vector load)
//
// Purely combinational — a 5:1 multiplexer.

module pc_mux (
    input  logic [31:0] i_pc_current,  // Current PC value (for hold)
    input  logic [31:0] i_pc_plus4,    // PC + 4 (from PC adder)
    input  logic [31:0] i_pc_offset,   // PC + offset (from PC adder)
    input  logic [31:0] i_a_bus,       // A-bus value (for JMP Rs)
    input  logic [31:0] i_mdr,         // MDR value (for exception vector)
    input  logic [2:0]  i_sel,         // pc_src from micro-word

    output logic [31:0] o_pc_next      // Next PC value → PC register
);

    always_comb begin
        case (i_sel)
            3'b000:  o_pc_next = i_pc_current;
            3'b001:  o_pc_next = i_pc_plus4;
            3'b010:  o_pc_next = i_pc_offset;
            3'b011:  o_pc_next = i_a_bus;
            3'b100:  o_pc_next = i_mdr;
            default: o_pc_next = i_pc_current;  // Safe: hold on undefined
        endcase
    end

endmodule
