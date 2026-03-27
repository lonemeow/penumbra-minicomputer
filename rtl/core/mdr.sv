// Penumbra MDR — Memory Data Register
//
// Bridges the datapath and memory subsystem. Two load sources:
//   - mdr_load_mem: latch read data from D-cache/memory (for loads)
//   - mdr_load_a:   latch A-bus value (for stores — MDR drives write data)
//
// Output goes to W-mux (load writeback), PC mux (exception vector),
// and memory write-data bus (stores).

module mdr
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic        i_load_mem,   // mdr_load_mem from micro-word
    input  logic        i_load_a,     // mdr_load_a from micro-word
    input  logic [31:0] i_mem_data,   // Read data from D-cache/memory
    input  logic [31:0] i_a_bus,      // A-bus value (for stores)
    output logic [31:0] o_data        // To W-mux, PC mux, memory write bus
);

    logic [31:0] data;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            data <= 32'b0;
        else if (i_load_mem)
            data <= i_mem_data;
        else if (i_load_a)
            data <= i_a_bus;
    end

    assign o_data = data;

endmodule
