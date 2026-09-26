// bus_wait_meter — the longest wait one bus slave has made an access take
//
// Counts what bus_response_fabric's watchdog counts: the cycles an
// access waits between its presentation and its ack, here for the
// accesses of one slave. It holds the longest completed wait. The
// watchdog faults an access once it has waited TIMEOUT_CYCLES, so the
// reading against that is the slave's margin.
//
// Nothing clears the maximum but configuration: it survives the
// machine's resets, so a long wait that ended in a crash is still on
// display after the reset that recovers from it. The count saturates at
// its width's all-ones, which reads as "that long or longer".

module bus_wait_meter #(
    parameter int WIDTH = 8
) (
    input  logic             i_clk,
    input  logic             i_req,     // the slave's strobe: selected, and re or we
    input  logic             i_ack,     // the slave's ack
    output logic [WIDTH-1:0] o_max      // the longest wait, in cycles
);
    // Configuration sets both; the always_ff below is their only writer.
    /* verilator lint_off PROCASSINIT */
    logic [WIDTH-1:0] wait_q = '0;      // cycles the current access has waited
    logic [WIDTH-1:0] max_q  = '0;      // the longest completed wait
    /* verilator lint_on PROCASSINIT */

    always_ff @(posedge i_clk) begin
        if (i_req && !i_ack) begin
            if (wait_q != {WIDTH{1'b1}}) wait_q <= wait_q + 1;
            if (wait_q > max_q)           max_q <= wait_q;
        end else begin
            if (wait_q > max_q)           max_q <= wait_q;
            wait_q <= '0;
        end
    end

    assign o_max = max_q;

endmodule
