// The Nexys Video's Ethernet: the Ethernet device on the board's PHY.
//
// The CLASS_ETHERNET device (eth) with what this board puts around it
// (doc/internals/nexysvideo/ethernet.md):
//
//   * the adapter that presents the RTL8211E's RGMII pins at the
//     device's seam, at 10 and 100 Mbit/s (eth_phy_rgmii_xc7)
//   * the PHY's hardware reset (eth_phy_reset)
//   * the station address, read from the board's EEPROM
//     (eth_addr_eeprom)
//   * the management data pin's pad
//
// What is left outside is the device's bus port, which a top puts behind
// an autoconfig_dev as it does any device's, a command port of the
// board's I2C master for the EEPROM's read, and the PHY's pins.
//
// The device's registers and PHY management run on i_clk, its receive
// side on the PHY's receive clock and its transmit side on the
// oscillator.
//
// Two resets come in. The machine's, i_rst, resets the device and starts
// the EEPROM's read. The board's, i_board_rst, alone resets the PHY, and
// o_phy_ready says when the PHY answers management frames after it. A
// top holds the machine's reset until then, so i_board_rst cannot be
// the machine's reset: each of the two would wait for the other.

module nexysvideo_eth #(
    // None has a default: the clock is the top's, and the top sizes the
    // device's window from the rings.
    parameter int unsigned CLK_HZ,      // i_clk's rate
    parameter int          RX_SLOTS,    // receive ring's slots, a power of two
    parameter int          TX_SLOTS,    // transmit ring's slots, a power of two
    parameter int          SLOT_BYTES   // bytes in a slot, a power of two
)(
    input  logic        i_clk,
    input  logic        i_rst,              // the machine's reset
    input  logic        i_clk_100mhz,       // transmit clock: the oscillator

    // ── The PHY's reset ──────────────────────────────────────
    input  logic        i_board_rst,        // the board's reset
    output logic        o_phy_ready,        // the PHY answers management frames

    // ── Bus (behind autoconfig_dev) ──────────────────────────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack,
    output logic        o_irq,

    // ── To the I2C master, for the EEPROM ────────────────────
    output logic        o_i2c_cmd_valid,
    input  logic        i_i2c_cmd_ready,
    output logic        o_i2c_cmd_start,
    output logic        o_i2c_cmd_stop,
    output logic        o_i2c_cmd_read,
    output logic [7:0]  o_i2c_cmd_wdata,
    output logic        o_i2c_cmd_nack,
    input  logic        i_i2c_rsp_valid,
    input  logic [7:0]  i_i2c_rsp_rdata,
    input  logic        i_i2c_rsp_nack,
    input  logic        i_i2c_rsp_error,

    // ── The PHY's pins ───────────────────────────────────────
    input  logic        i_rgmii_rxc,
    input  logic [3:0]  i_rgmii_rxd,
    input  logic        i_rgmii_rx_ctl,
    output logic        o_rgmii_txc,
    output logic [3:0]  o_rgmii_txd,
    output logic        o_rgmii_tx_ctl,
    output logic        o_mdc,
    inout  wire         io_mdio,
    output logic        o_phy_rst_n         // low resets the PHY
);

    // The oscillator's rate, a multiple of 100 MHz as the adapter's
    // transmit clock has to be.
    localparam int unsigned TX_CLK_HZ = 100_000_000;

    // ── The PHY's reset ──────────────────────────────────────
    // The RTL8211E asks for 10 ms on its reset pin, and answers no
    // management frame for 30 ms after (datasheet, "PHY Reset").
    eth_phy_reset #(
        .CLK_HZ    (CLK_HZ),
        .RESET_MS  (10),
        .SETTLE_MS (30)
    ) u_phy_reset (
        .i_clk       (i_clk),
        .i_rst       (i_board_rst),
        .o_phy_rst_n (o_phy_rst_n),
        .o_ready     (o_phy_ready)
    );

    // ── The station address ──────────────────────────────────
    logic [47:0] factory_addr;
    logic        factory_valid;

    // The EEPROM's address on the bus: the manual gives 1010111b.
    eth_addr_eeprom #(
        .DEV_ADDR (7'h57)
    ) u_addr (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .o_cmd_valid (o_i2c_cmd_valid),
        .i_cmd_ready (i_i2c_cmd_ready),
        .o_cmd_start (o_i2c_cmd_start),
        .o_cmd_stop  (o_i2c_cmd_stop),
        .o_cmd_read  (o_i2c_cmd_read),
        .o_cmd_wdata (o_i2c_cmd_wdata),
        .o_cmd_nack  (o_i2c_cmd_nack),
        .i_rsp_valid (i_i2c_rsp_valid),
        .i_rsp_rdata (i_i2c_rsp_rdata),
        .i_rsp_nack  (i_i2c_rsp_nack),
        .i_rsp_error (i_i2c_rsp_error),
        .o_addr      (factory_addr),
        .o_valid     (factory_valid)
    );

    // ── The device and the PHY's adapter ─────────────────────
    // The seam between the two.
    logic [2:0] speeds;
    logic       rx_clk, rx_ce, rx_dv, rx_er;
    logic [7:0] rxd;
    logic       tx_ce, tx_en;
    logic [7:0] txd;
    logic [1:0] tx_speed;

    logic mdio_out, mdio_oe;

    eth #(
        .CLK_HZ     (int'(CLK_HZ)),
        .RX_SLOTS   (RX_SLOTS),
        .TX_SLOTS   (TX_SLOTS),
        .SLOT_BYTES (SLOT_BYTES)
    ) u_dev (
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
        .i_factory_addr  (factory_addr),
        .i_factory_valid (factory_valid),
        .o_mdc           (o_mdc),
        .o_mdio          (mdio_out),
        .o_mdio_oe       (mdio_oe),
        .i_mdio          (io_mdio),
        .i_speeds        (speeds),
        .i_rx_clk        (rx_clk),
        .i_rx_ce         (rx_ce),
        .i_rxd           (rxd),
        .i_rx_dv         (rx_dv),
        .i_rx_er         (rx_er),
        // The adapter's receive side is the same at either speed.
        /* verilator lint_off PINCONNECTEMPTY */
        .o_rx_speed      (),
        /* verilator lint_on PINCONNECTEMPTY */
        .i_tx_clk        (i_clk_100mhz),
        .i_tx_ce         (tx_ce),
        .o_txd           (txd),
        .o_tx_en         (tx_en),
        .o_tx_speed      (tx_speed)
    );

    eth_phy_rgmii_xc7 #(
        .TX_CLK_HZ (TX_CLK_HZ)
    ) u_phy (
        .o_rx_clk       (rx_clk),
        .o_rx_ce        (rx_ce),
        .o_rxd          (rxd),
        .o_rx_dv        (rx_dv),
        .o_rx_er        (rx_er),
        .i_tx_clk       (i_clk_100mhz),
        .o_tx_ce        (tx_ce),
        .i_txd          (txd),
        .i_tx_en        (tx_en),
        .i_tx_speed     (tx_speed),
        .o_speeds       (speeds),
        .i_rgmii_rxc    (i_rgmii_rxc),
        .i_rgmii_rxd    (i_rgmii_rxd),
        .i_rgmii_rx_ctl (i_rgmii_rx_ctl),
        .o_rgmii_txc    (o_rgmii_txc),
        .o_rgmii_txd    (o_rgmii_txd),
        .o_rgmii_tx_ctl (o_rgmii_tx_ctl)
    );

    // The management data pin: driven while the device sends, let go
    // for the PHY's answer, with the board's pull-up behind it.
    assign io_mdio = mdio_oe ? mdio_out : 1'bz;

endmodule
