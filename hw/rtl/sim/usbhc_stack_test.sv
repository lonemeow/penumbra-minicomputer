// Penumbra USB MAC + sim-PHY stack (simulation DUT)
//
// usbhc_mac mated to usb_phy_sim at the UTMI-shaped seam — the first
// time the two tiers meet in RTL. The testbench plays the register tier
// above and the behavioural device below, so every check crosses the
// seam both ways; this is the pre-integration proof that the machine's
// USB-domain stack works end to end. Frame and debounce parameters are
// shortened as in usbhc_mac_test.

module usbhc_stack_test (
    input  logic        i_clk,
    input  logic        i_rst,
    // MAC register-tier surface
    input  logic        i_start,
    input  logic [1:0]  i_pid_sel,
    input  logic [6:0]  i_devaddr,
    input  logic [3:0]  i_endpoint,
    input  logic        i_toggle,
    input  logic [6:0]  i_length,
    output logic        o_busy,
    output logic        o_done,
    output logic [2:0]  o_result,
    output logic [6:0]  o_rxlen,
    output logic        o_rxtoggle,
    input  logic        i_run,
    input  logic        i_power,
    input  logic        i_reset,
    input  logic        i_suspend,
    input  logic        i_resume,
    output logic        o_connect,
    output logic        o_enabled,
    output logic        o_reset_active,
    output logic        o_suspended,
    output logic [1:0]  o_port_speed,
    output logic [1:0]  o_port_line,
    output logic        o_port_change,
    output logic        o_sof_irq,
    output logic [10:0] o_frame,
    output logic [6:0]  o_buf_raddr,
    input  logic [7:0]  i_buf_rdata,
    output logic [6:0]  o_buf_waddr,
    output logic [7:0]  o_buf_wdata,
    output logic        o_buf_we,
    // PHY device port
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
    // The seam between the tiers.
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

    usbhc_mac #(
        .CLKS_PER_MS   (4000),
        .DEBOUNCE_CLKS (30)
    ) u_mac (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_start        (i_start),
        .i_pid_sel      (i_pid_sel),
        .i_devaddr      (i_devaddr),
        .i_endpoint     (i_endpoint),
        .i_toggle       (i_toggle),
        .i_length       (i_length),
        .o_busy         (o_busy),
        .o_done         (o_done),
        .o_result       (o_result),
        .o_rxlen        (o_rxlen),
        .o_rxtoggle     (o_rxtoggle),
        .i_run          (i_run),
        .i_power        (i_power),
        .i_reset        (i_reset),
        .i_suspend      (i_suspend),
        .i_resume       (i_resume),
        .o_connect      (o_connect),
        .o_enabled      (o_enabled),
        .o_reset_active (o_reset_active),
        .o_suspended    (o_suspended),
        .o_port_speed   (o_port_speed),
        .o_port_line    (o_port_line),
        .o_port_change  (o_port_change),
        .o_sof_irq      (o_sof_irq),
        .o_frame        (o_frame),
        /* verilator lint_off PINCONNECTEMPTY */
        .o_sof_tx_cnt   (),
        /* verilator lint_on PINCONNECTEMPTY */
        .o_buf_raddr    (o_buf_raddr),
        .i_buf_rdata    (i_buf_rdata),
        .o_buf_waddr    (o_buf_waddr),
        .o_buf_wdata    (o_buf_wdata),
        .o_buf_we       (o_buf_we),
        .o_tx_data      (tx_data),
        .o_tx_valid     (tx_valid),
        .i_tx_ready     (tx_ready),
        .i_rx_data      (rx_data),
        .i_rx_valid     (rx_valid),
        .i_rx_active    (rx_active),
        .i_rx_error     (rx_error),
        .i_line_state   (line_state),
        .i_caps         (caps),
        .o_xcvr_sel     (xcvr_sel),
        .o_term_sel     (term_sel),
        .o_opmode       (opmode),
        /* verilator lint_off PINCONNECTEMPTY */
        .o_port_power   ()
        /* verilator lint_on PINCONNECTEMPTY */
    );

    usb_phy_sim u_phy (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
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
        /* verilator lint_off PINCONNECTEMPTY */
        .o_dbg          ()
        /* verilator lint_on PINCONNECTEMPTY */
    );
endmodule
