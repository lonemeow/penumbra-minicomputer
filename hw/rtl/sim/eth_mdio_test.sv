// eth_mdio_test — module-test harness for the PHY management master.
//
// Runs the master on 25 MHz, the slowest bus clock a machine has. Half an
// MDC period is then five cycles and the period exactly the 400 ns
// minimum, so the read turnaround has the least room it ever gets, and
// one cycle is 40 ns, fine enough to hold the pins to the minimums a PHY
// asks for.

module eth_mdio_test (
    input  logic        i_clk,

    input  logic        i_start,
    input  logic        i_write,
    input  logic [4:0]  i_phy_addr,
    input  logic [4:0]  i_reg_addr,
    input  logic [15:0] i_wdata,
    output logic        o_busy,
    output logic        o_done,
    output logic [15:0] o_rdata,

    output logic        o_mdc,
    output logic        o_mdio,
    output logic        o_mdio_oe,
    input  logic        i_mdio
);

    eth_mdio #(
        .CLK_HZ (25_000_000)
    ) u_mdio (
        .i_clk      (i_clk),
        .i_start    (i_start),
        .i_write    (i_write),
        .i_phy_addr (i_phy_addr),
        .i_reg_addr (i_reg_addr),
        .i_wdata    (i_wdata),
        .o_busy     (o_busy),
        .o_done     (o_done),
        .o_rdata    (o_rdata),
        .o_mdc      (o_mdc),
        .o_mdio     (o_mdio),
        .o_mdio_oe  (o_mdio_oe),
        .i_mdio     (i_mdio)
    );

endmodule
