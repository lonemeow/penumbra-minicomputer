// Penumbra MAR — Memory Address Register
//
// Latches the D-cache/bus address from R-bus. I-cache address is
// permanently wired to PC (no MAR involvement).

module mar
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic        i_load,     // mar_load from micro-word
    input  logic [31:0] i_rbus,     // R-bus (ALU result)
    output logic [31:0] o_addr      // To D-cache / memory subsystem
);

    logic [31:0] addr;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            addr <= 32'b0;
        else if (i_load)
            addr <= i_rbus;
    end

    assign o_addr = addr;

endmodule
