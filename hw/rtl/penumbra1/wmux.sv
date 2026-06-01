// Penumbra W-Mux — selects the write-back source for the register file / SR
//
// Controlled by wb_src from the micro-word:
//   0 = RBUS   : R-bus (ALU result)
//   1 = MDR    : memory data register (load result)
//   2 = DML_LO : divmul low half  (product low / quotient)
//   3 = DML_HI : divmul high half (product high / remainder)
//
// Purely combinational — a 4:1 multiplexer. This is the single point where a
// value enters the write-back path, so each source stays an independent input
// rather than being wired into the R-bus.

module wmux (
    input  logic [31:0] i_r_bus,      // ALU result (R-bus)
    input  logic [31:0] i_mdr,        // Memory data register (load result)
    input  logic [31:0] i_divmul_lo,  // divmul low half
    input  logic [31:0] i_divmul_hi,  // divmul high half
    input  logic [1:0]  i_sel,        // wb_src from micro-word

    output logic [31:0] o_wr_data     // → register file write data
);

    always_comb begin
        case (i_sel)
            2'd0:    o_wr_data = i_r_bus;
            2'd1:    o_wr_data = i_mdr;
            2'd2:    o_wr_data = i_divmul_lo;
            default: o_wr_data = i_divmul_hi;  // 2'd3
        endcase
    end

endmodule
