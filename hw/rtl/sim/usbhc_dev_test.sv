// Penumbra USB host controller device test (simulation DUT)
//
// The full CLASS_USBHC device as an integration will see it — usbhc
// (register tier + CDC + MAC) mated to usb_phy_sim — minus only the
// autoconfig wrapper. The testbench plays the bus master against the
// register contract on the CPU clock and the behavioural device on the
// USB clock, so every scenario crosses the clock domains for real.
// Frame and debounce are shortened as in the MAC-level tests.

module usbhc_dev_test (
    // CPU clock domain: the bus
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,
    output logic        o_irq,
    // USB clock domain: the device port
    input  logic        i_usb_clk,
    input  logic        i_usb_rst,
    output logic [7:0]  o_pkt_data,
    output logic        o_pkt_valid,
    output logic        o_pkt_end,
    output logic        o_keepalive,
    output logic        o_bus_reset,
    output logic        o_dev_resume,
    output logic        o_rx_ready,
    input  logic        i_rx_valid,
    input  logic [7:0]  i_rx_data,
    input  logic        i_rx_last,
    input  logic        i_dev_connect,
    input  logic [1:0]  i_dev_speed
);
    logic [7:0] tx_data;
    logic       tx_valid, tx_ready;
    logic [7:0] rx_data;
    logic       rx_valid, rx_active, rx_error;
    logic [1:0] opmode, xcvr_sel;
    logic       term_sel;
    logic [1:0] line_state;
    logic [2:0] caps;
    logic       phy_clk;

    logic unused_phy_clk;
    assign unused_phy_clk = phy_clk;

    logic [15:0] phy_dbg;

    usbhc #(
        .CLKS_PER_MS   (4000),
        .DEBOUNCE_CLKS (30)
    ) u_usbhc (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_addr       (i_addr),
        .i_wdata      (i_wdata),
        .i_we         (i_we),
        .i_re         (i_re),
        .o_rdata      (o_rdata),
        .o_busy       (o_busy),
        .o_irq        (o_irq),
        .i_usb_clk    (i_usb_clk),
        .i_usb_rst    (i_usb_rst),
        .o_tx_data    (tx_data),
        .o_tx_valid   (tx_valid),
        .i_tx_ready   (tx_ready),
        .i_rx_data    (rx_data),
        .i_rx_valid   (rx_valid),
        .i_rx_active  (rx_active),
        .i_rx_error   (rx_error),
        .i_line_state (line_state),
        .i_caps       (caps),
        .o_xcvr_sel   (xcvr_sel),
        .o_term_sel   (term_sel),
        .o_opmode     (opmode),
        /* verilator lint_off PINCONNECTEMPTY */
        .o_port_power (),
        /* verilator lint_on PINCONNECTEMPTY */
        .i_dbg        (phy_dbg)
    );

    usb_phy_sim u_phy (
        .i_clk          (i_usb_clk),
        .i_rst          (i_usb_rst),
        .o_clk          (phy_clk),
        .i_tx_data      (tx_data),
        .i_tx_valid     (tx_valid),
        .o_tx_ready     (tx_ready),
        .o_rx_data      (rx_data),
        .o_rx_valid     (rx_valid),
        .o_rx_active    (rx_active),
        .o_rx_error     (rx_error),
        .i_opmode       (opmode),
        .i_xcvr_sel     (xcvr_sel),
        .i_term_sel     (term_sel),
        .o_line_state   (line_state),
        .o_caps         (caps),
        .o_pkt_data     (o_pkt_data),
        .o_pkt_valid    (o_pkt_valid),
        .o_pkt_end      (o_pkt_end),
        .o_keepalive    (o_keepalive),
        .o_bus_reset    (o_bus_reset),
        .o_resume       (o_dev_resume),
        .o_rx_ready     (o_rx_ready),
        .i_rx_valid     (i_rx_valid),
        .i_rx_data      (i_rx_data),
        .i_rx_last      (i_rx_last),
        .i_dev_connect  (i_dev_connect),
        .i_dev_speed    (i_dev_speed),
        .o_dbg          (phy_dbg)
    );
endmodule
