// penumbra4_mem_launch — MEM1: launches the access's translation and
// lookup.
//
// MEM1 launches nothing: a load, a store, RDSYS or WRSYS that reaches it
// stops the simulation. Every other op passes through.
//
// The output register is the pair MEM2 resolves.

module penumbra4_mem_launch
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

    // ── Contract ─────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst && i_pair.y.valid) begin
            assert (!(i_pair.y.op.op_class inside {OPC_MEM, OPC_RDSYS, OPC_WRSYS}))
                else $fatal(1, "penumbra4_mem_launch: an access or sysreg op at 0x%08x, which MEM1 does not launch",
                            i_pair.y.op.pc);
        end
    end

endmodule
