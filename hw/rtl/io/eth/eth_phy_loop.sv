// Penumbra Ethernet loopback PHY adapter
//
// Stands where a board's PHY adapter does, at the adapter's seam, and
// hands the receiver every byte time the transmitter puts up: a wire from
// the device back to itself, for simulation. One clock serves as both the
// receive clock and the transmit clock.
//
// It enables a byte time every CE_PERIOD cycles, in both directions, so a
// test can give the device the spacing a slower link would.

module eth_phy_loop #(
    parameter int CE_PERIOD = 1     // cycles to a byte time
)(
    input  logic       i_clk,

    // ── The seam, as a board's PHY adapter presents it ───────
    output logic       o_rx_ce,
    output logic [7:0] o_rxd,
    output logic       o_rx_dv,
    output logic       o_rx_er,
    output logic       o_tx_ce,
    input  logic [7:0] i_txd,
    input  logic       i_tx_en,
    output logic [2:0] o_speeds     // every speed: the rate is the test's to choose
);

    localparam int COUNT_W = (CE_PERIOD > 1) ? $clog2(CE_PERIOD) : 1;

    // No reset: a counter that runs from whatever it wakes with.
    logic [COUNT_W-1:0] count_q;
    logic               tick;
    assign tick = (count_q == COUNT_W'(CE_PERIOD - 1));

    always_ff @(posedge i_clk) begin
        count_q <= tick ? '0 : count_q + COUNT_W'(1);
    end

    // The byte time taken from the transmitter on one enable is the
    // receiver's on the next cycle.
    assign o_tx_ce = tick;

    always_ff @(posedge i_clk) begin
        o_rx_ce <= tick;
        if (tick) begin
            o_rxd   <= i_txd;
            o_rx_dv <= i_tx_en;
        end
    end

    assign o_rx_er  = 1'b0;
    assign o_speeds = 3'b111;

endmodule
