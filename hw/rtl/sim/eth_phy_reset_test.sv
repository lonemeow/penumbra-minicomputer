// eth_phy_reset_test — module-test harness for the PHY's hardware reset.
//
// The sequencer with the RTL8211E's two times, 10 ms and 30 ms, and a
// clock of 102.35 kHz (tb_eth_phy_reset). Neither time is a whole number
// of its cycles, so a count rounded down falls short, and the two waits
// come to 4095 cycles, one short of a power of two: the most a count
// sized for them can hold.

module eth_phy_reset_test (
    input  logic i_clk,
    input  logic i_rst,
    output logic o_phy_rst_n,
    output logic o_ready
);

    eth_phy_reset #(
        .CLK_HZ    (102_350),
        .RESET_MS  (10),
        .SETTLE_MS (30)
    ) u_reset (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .o_phy_rst_n (o_phy_rst_n),
        .o_ready     (o_ready)
    );

endmodule
