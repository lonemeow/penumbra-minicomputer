// eth_loop_test — module-test harness for the Ethernet adapter.
//
// The whole adapter with the loopback PHY adapter at its seam, so that
// every frame it sends comes back to it: registers, both crossings, both
// slot memories, transmitter and receiver, driven from the bus alone
// (tb_eth_loop). One clock stands for both PHY-side clocks, unrelated to
// the bus clock, and a byte time is every other cycle of it, as a PHY
// adapter pairing nibbles presents them.
//
// The bus clock is taken as 25 MHz, which only sets how many cycles a
// management frame lasts.

module eth_loop_test (
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack,
    output logic        o_irq,

    input  logic [47:0] i_factory_addr,
    input  logic        i_factory_valid,

    output logic        o_mdc,
    output logic        o_mdio,
    output logic        o_mdio_oe,
    input  logic        i_mdio,

    input  logic        i_phy_clk,
    output logic [1:0]  o_rx_speed,
    output logic [1:0]  o_tx_speed
);

    logic       rx_ce, rx_dv, rx_er, tx_ce, tx_en;
    logic [7:0] rxd, txd;
    logic [2:0] speeds;

    eth_phy_loop #(
        .CE_PERIOD (2)
    ) u_phy (
        .i_clk    (i_phy_clk),
        .o_rx_ce  (rx_ce),
        .o_rxd    (rxd),
        .o_rx_dv  (rx_dv),
        .o_rx_er  (rx_er),
        .o_tx_ce  (tx_ce),
        .i_txd    (txd),
        .i_tx_en  (tx_en),
        .o_speeds (speeds)
    );

    eth #(
        .CLK_HZ (25_000_000)
    ) u_eth (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_addr          (i_addr),
        .i_wdata         (i_wdata),
        .i_byte_en       (i_byte_en),
        .i_we            (i_we),
        .i_re            (i_re),
        .o_rdata         (o_rdata),
        .o_ack           (o_ack),
        .o_irq           (o_irq),
        .i_factory_addr  (i_factory_addr),
        .i_factory_valid (i_factory_valid),
        .o_mdc           (o_mdc),
        .o_mdio          (o_mdio),
        .o_mdio_oe       (o_mdio_oe),
        .i_mdio          (i_mdio),
        .i_speeds        (speeds),
        .i_rx_clk        (i_phy_clk),
        .i_rx_ce         (rx_ce),
        .i_rxd           (rxd),
        .i_rx_dv         (rx_dv),
        .i_rx_er         (rx_er),
        .o_rx_speed      (o_rx_speed),
        .i_tx_clk        (i_phy_clk),
        .i_tx_ce         (tx_ce),
        .o_txd           (txd),
        .o_tx_en         (tx_en),
        .o_tx_speed      (o_tx_speed)
    );

endmodule
