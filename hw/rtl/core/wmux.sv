// Penumbra W-Mux — selects write-back source for the register file
//
// Controlled by w_mux_sel from the micro-word:
//   0 = R-bus (ALU result)
//   1 = MDR (memory data register — load result)
//
// Purely combinational — a 2:1 multiplexer.

module wmux (
    input  logic [31:0] i_r_bus,    // ALU result (R-bus)
    input  logic [31:0] i_mdr,      // Memory data register
    input  logic        i_sel,      // w_mux_sel from micro-word

    output logic [31:0] o_wr_data   // → register file write data
);

    assign o_wr_data = i_sel ? i_mdr : i_r_bus;

endmodule
