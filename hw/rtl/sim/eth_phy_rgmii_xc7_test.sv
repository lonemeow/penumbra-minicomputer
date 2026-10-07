// eth_phy_rgmii_xc7_test — module-test harness for the RGMII PHY adapter.
//
// The adapter with its transmit clock taken as 100 MHz, the rate of the
// Nexys Video's oscillator: a quarter of the pin clock's period is one
// cycle at 100 Mbit/s and ten at 10 (tb_eth_phy_rgmii_xc7). Its I/O cells
// are the 7-series stubs' behavioral ones.

module eth_phy_rgmii_xc7_test (
    output logic       o_rx_clk,
    output logic       o_rx_ce,
    output logic [7:0] o_rxd,
    output logic       o_rx_dv,
    output logic       o_rx_er,

    input  logic       i_tx_clk,
    output logic       o_tx_ce,
    input  logic [7:0] i_txd,
    input  logic       i_tx_en,
    input  logic [1:0] i_tx_speed,

    output logic [2:0] o_speeds,

    input  logic       i_rgmii_rxc,
    input  logic [3:0] i_rgmii_rxd,
    input  logic       i_rgmii_rx_ctl,
    output logic       o_rgmii_txc,
    output logic [3:0] o_rgmii_txd,
    output logic       o_rgmii_tx_ctl
);

    eth_phy_rgmii_xc7 #(
        .TX_CLK_HZ (100_000_000)
    ) u_phy (
        .o_rx_clk       (o_rx_clk),
        .o_rx_ce        (o_rx_ce),
        .o_rxd          (o_rxd),
        .o_rx_dv        (o_rx_dv),
        .o_rx_er        (o_rx_er),
        .i_tx_clk       (i_tx_clk),
        .o_tx_ce        (o_tx_ce),
        .i_txd          (i_txd),
        .i_tx_en        (i_tx_en),
        .i_tx_speed     (i_tx_speed),
        .o_speeds       (o_speeds),
        .i_rgmii_rxc    (i_rgmii_rxc),
        .i_rgmii_rxd    (i_rgmii_rxd),
        .i_rgmii_rx_ctl (i_rgmii_rx_ctl),
        .o_rgmii_txc    (o_rgmii_txc),
        .o_rgmii_txd    (o_rgmii_txd),
        .o_rgmii_tx_ctl (o_rgmii_tx_ctl)
    );

endmodule
