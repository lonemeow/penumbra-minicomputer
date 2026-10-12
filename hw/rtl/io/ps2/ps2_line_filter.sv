// Penumbra PS/2 port: one line in
//
// A PS/2 line onto the bus clock: two synchroniser flops, then o_level
// follows only once the line has stood at a new level for FILTER_CYCLES
// cycles. A PS/2 line rises through a pull-up, slowly enough to cross
// an input's threshold more than once.
//
// The delay from i_line to o_level is two cycles and FILTER_CYCLES, the
// same for every instance. The link depends on that to keep the three
// signals it filters aligned.
//
// No reset: the level is the line's own, and configures to high, a
// line let go.

module ps2_line_filter #(
    parameter int FILTER_CYCLES = 1     // cycles a new level stands before it counts
)(
    input  logic i_clk,
    input  logic i_line,    // the line as it stands, on no clock
    output logic o_level    // the level it last kept for FILTER_CYCLES cycles
);

    localparam int RUN_W = (FILTER_CYCLES > 1) ? $clog2(FILTER_CYCLES) : 1;

    typedef logic [RUN_W-1:0] run_t;
    localparam run_t RUN_LAST = run_t'(FILTER_CYCLES - 1);

    // ASYNC_REG on both synchroniser flops: Vivado places the pair
    // together and its clock-crossing check recognises it.
    (* ASYNC_REG = "TRUE" *) logic line_s1_q, line_s2_q;

    always_ff @(posedge i_clk) begin
        {line_s2_q, line_s1_q} <= {line_s1_q, i_line};
    end

    /* verilator lint_off PROCASSINIT */
    logic level_q = 1'b1;
    /* verilator lint_on PROCASSINIT */

    // Cycles the line has now stood at the level o_level does not show.
    run_t run_q;

    always_ff @(posedge i_clk) begin
        if (line_s2_q == level_q) begin
            run_q <= '0;
        end else if (run_q == RUN_LAST) begin
            level_q <= line_s2_q;
            run_q   <= '0;
        end else begin
            run_q <= run_q + run_t'(1);
        end
    end

    assign o_level = level_q;

endmodule
