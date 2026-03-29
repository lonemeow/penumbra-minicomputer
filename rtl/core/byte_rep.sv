// Penumbra Byte Replicator — sub-word store data lane positioning
//
// Replicates the low byte or halfword of the input across all lanes
// so that byte_en can select the correct position for the write.
// For word stores, passes through unchanged.
//
// Purely combinational — wiring and mux only.
//
// mem_size encoding (matches microcode):
//   00 = byte:     data[7:0] → all 4 byte lanes
//   01 = halfword: data[15:0] → both halfword lanes
//   10 = word:     pass-through

module byte_rep (
    input  logic [31:0] i_data,    // Raw store data (from MDR)
    input  logic [1:0]  i_size,    // Access size: 00=byte, 01=half, 10=word

    output logic [31:0] o_data     // Lane-replicated data for memory write
);

    always_comb begin
        case (i_size)
            2'b00:   o_data = {4{i_data[7:0]}};    // Byte: replicate to all lanes
            2'b01:   o_data = {2{i_data[15:0]}};    // Half: replicate to both lanes
            default: o_data = i_data;                // Word: pass-through
        endcase
    end

endmodule
