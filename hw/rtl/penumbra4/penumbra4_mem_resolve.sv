// penumbra4_mem_resolve — MEM2: takes the access's verdicts and finishes
// the access or hands it to the L1-D.
//
// MEM1 launches no access, so MEM2 has none to resolve: every op passes
// through.
//
// The output register is the pair commit takes.

module penumbra4_mem_resolve
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic  i_clk,
    input  logic  i_rst,

    input  pair_t i_pair,
    input  logic  i_kill,
    input  logic  i_hold,
    output logic  o_hold,

    output pair_t o_pair
);

    pair_t pair_q, pair_d;

    always_comb begin
        pair_d = pair_q;
        if (i_kill) begin
            pair_d = '0;
        end else if (!i_hold) begin
            pair_d = i_pair;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            pair_q <= '0;
        end else begin
            pair_q <= pair_d;
        end
    end

    assign o_hold = i_hold;
    assign o_pair = pair_q;

endmodule
