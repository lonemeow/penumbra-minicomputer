// Penumbra USB host controller (CLASS_USBHC device)
//
// The complete controller above the PHY seam: the register tier on the
// CPU clock, the MAC on the USB clock, and the crossing between them.
// An integration instantiates this behind an autoconfig_dev wrapper and
// mates the seam to its PHY — usb_phy_ecp5 on the board, usb_phy_sim in
// the simulator — exactly the swap the tier split exists for.
//
// The transaction request fields (TOKEN, LENGTH) cross the domains as
// plain wires: the register tier holds them stable from START until
// DONE, and the CDC's START toggle is what tells the MAC they are safe
// to sample — the data-before-toggle discipline, applied to a request.

module usbhc #(
    parameter int          BUF_BYTES     = 64,
    // Passed to the MAC; overridable so testbenches can run short frames
    // and fast connect detection.
    parameter int unsigned CLKS_PER_MS   = 60_000,
    parameter int unsigned DEBOUNCE_CLKS = 600
) (
    // ── CPU clock domain ─────────────────────────────────────────────
    input  logic        i_clk,
    input  logic        i_rst,
    // Bus interface (behind autoconfig_dev)
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,
    output logic        o_irq,
    // ── USB clock domain ─────────────────────────────────────────────
    input  logic        i_usb_clk,
    input  logic        i_usb_rst,
    // MAC-PHY seam
    output logic [7:0]  o_tx_data,
    output logic        o_tx_valid,
    input  logic        i_tx_ready,
    input  logic [7:0]  i_rx_data,
    input  logic        i_rx_valid,
    input  logic        i_rx_active,
    input  logic        i_rx_error,
    input  logic [1:0]  i_line_state,
    input  logic [2:0]  i_caps,
    output logic [1:0]  o_xcvr_sel,
    output logic        o_term_sel,
    output logic [1:0]  o_opmode,
    output logic        o_port_power
);

    // Register tier <-> CDC, CPU domain
    logic [6:0]  cbuf_addr;
    logic [7:0]  cbuf_wdata;
    logic        cbuf_we;
    logic [7:0]  cbuf_rdata;
    logic        start;
    logic        done;
    logic [2:0]  result;
    logic [6:0]  rxlen;
    logic        rxtoggle;
    logic        port_change, sof;
    logic        connect, enabled, reset_active, suspended;
    logic [1:0]  speed, line;
    logic [10:0] frame;
    logic        run, power, reset_port, suspend, resume;

    // Request fields, CPU domain outputs held stable through the
    // transaction; sampled USB-side on the crossed START.
    logic [1:0]  pid_sel;
    logic [6:0]  devaddr;
    logic [3:0]  endpoint;
    logic        toggle;
    logic [6:0]  length;

    // CDC <-> MAC, USB domain
    logic        u_start;
    logic        u_done;
    logic [2:0]  u_result;
    logic [6:0]  u_rxlen;
    logic        u_rxtoggle;
    logic        u_port_change, u_sof;
    logic        u_connect, u_enabled, u_reset_active, u_suspended;
    logic [1:0]  u_speed, u_line;
    logic [10:0] u_frame;
    logic        u_run, u_power, u_reset_port, u_suspend, u_resume;
    logic [6:0]  ubuf_addr;
    logic [7:0]  ubuf_wdata;
    logic        ubuf_we;
    logic [7:0]  ubuf_rdata;
    logic        u_busy;

    // The MAC's transaction busy stays USB-side: the register tier's
    // protocol is START -> DONE, never a busy poll.
    logic unused_u_busy;
    assign unused_u_busy = u_busy;

    usbhc_regs #(
        .BUF_BYTES      (BUF_BYTES)
    ) u_regs (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_addr         (i_addr),
        .i_wdata        (i_wdata),
        .i_we           (i_we),
        .i_re           (i_re),
        .o_rdata        (o_rdata),
        .o_busy         (o_busy),
        .o_irq          (o_irq),
        .i_caps         (i_caps),
        .o_cbuf_addr    (cbuf_addr),
        .o_cbuf_wdata   (cbuf_wdata),
        .o_cbuf_we      (cbuf_we),
        .i_cbuf_rdata   (cbuf_rdata),
        .o_start        (start),
        .i_done         (done),
        .i_result       (result),
        .i_rxlen        (rxlen),
        .i_rxtoggle     (rxtoggle),
        .i_port_change  (port_change),
        .i_sof          (sof),
        .i_connect      (connect),
        .i_enabled      (enabled),
        .i_reset_active (reset_active),
        .i_suspended    (suspended),
        .i_speed        (speed),
        .i_line         (line),
        .i_frame        (frame),
        .o_run          (run),
        .o_power        (power),
        .o_reset_port   (reset_port),
        .o_suspend      (suspend),
        .o_resume       (resume),
        .o_pid_sel      (pid_sel),
        .o_devaddr      (devaddr),
        .o_endpoint     (endpoint),
        .o_toggle       (toggle),
        .o_length       (length)
    );

    usbhc_cdc #(
        .BUF_BYTES        (BUF_BYTES)
    ) u_cdc (
        .i_clk            (i_clk),
        .i_rst            (i_rst),
        .i_cbuf_addr      (cbuf_addr),
        .i_cbuf_wdata     (cbuf_wdata),
        .i_cbuf_we        (cbuf_we),
        .o_cbuf_rdata     (cbuf_rdata),
        .i_start          (start),
        .o_done           (done),
        .o_result         (result),
        .o_rxlen          (rxlen),
        .o_rxtoggle       (rxtoggle),
        .o_port_change    (port_change),
        .o_sof            (sof),
        .o_connect        (connect),
        .o_enabled        (enabled),
        .o_reset_active   (reset_active),
        .o_suspended      (suspended),
        .o_speed          (speed),
        .o_line           (line),
        .o_frame          (frame),
        .i_run            (run),
        .i_power          (power),
        .i_reset_port     (reset_port),
        .i_suspend        (suspend),
        .i_resume         (resume),
        .i_usb_clk        (i_usb_clk),
        .i_usb_rst        (i_usb_rst),
        .i_ubuf_addr      (ubuf_addr),
        .i_ubuf_wdata     (ubuf_wdata),
        .i_ubuf_we        (ubuf_we),
        .o_ubuf_rdata     (ubuf_rdata),
        .o_u_start        (u_start),
        .i_u_done         (u_done),
        .i_u_result       (u_result),
        .i_u_rxlen        (u_rxlen),
        .i_u_rxtoggle     (u_rxtoggle),
        .i_u_port_change  (u_port_change),
        .i_u_sof          (u_sof),
        .i_u_connect      (u_connect),
        .i_u_enabled      (u_enabled),
        .i_u_reset_active (u_reset_active),
        .i_u_suspended    (u_suspended),
        .i_u_speed        (u_speed),
        .i_u_line         (u_line),
        .i_u_frame        (u_frame),
        .o_u_run          (u_run),
        .o_u_power        (u_power),
        .o_u_reset_port   (u_reset_port),
        .o_u_suspend      (u_suspend),
        .o_u_resume       (u_resume)
    );

    // The MAC's read and write buffer ports share the CDC's USB port:
    // transmit reads and receive stores never overlap within a
    // transaction, so the write leg simply wins the mux.
    logic [6:0] mac_raddr, mac_waddr;
    logic [7:0] mac_wdata;
    logic       mac_we;
    assign ubuf_addr  = mac_we ? mac_waddr : mac_raddr;
    assign ubuf_wdata = mac_wdata;
    assign ubuf_we    = mac_we;

    usbhc_mac #(
        .CLKS_PER_MS   (CLKS_PER_MS),
        .DEBOUNCE_CLKS (DEBOUNCE_CLKS),
        .BUF_BYTES     (BUF_BYTES)
    ) u_mac (
        .i_clk          (i_usb_clk),
        .i_rst          (i_usb_rst),
        .i_start        (u_start),
        .i_pid_sel      (pid_sel),
        .i_devaddr      (devaddr),
        .i_endpoint     (endpoint),
        .i_toggle       (toggle),
        .i_length       (length),
        .o_busy         (u_busy),
        .o_done         (u_done),
        .o_result       (u_result),
        .o_rxlen        (u_rxlen),
        .o_rxtoggle     (u_rxtoggle),
        .i_run          (u_run),
        .i_power        (u_power),
        .i_reset        (u_reset_port),
        .i_suspend      (u_suspend),
        .i_resume       (u_resume),
        .o_connect      (u_connect),
        .o_enabled      (u_enabled),
        .o_reset_active (u_reset_active),
        .o_suspended    (u_suspended),
        .o_port_speed   (u_speed),
        .o_port_line    (u_line),
        .o_port_change  (u_port_change),
        .o_sof_irq      (u_sof),
        .o_frame        (u_frame),
        .o_buf_raddr    (mac_raddr),
        .i_buf_rdata    (ubuf_rdata),
        .o_buf_waddr    (mac_waddr),
        .o_buf_wdata    (mac_wdata),
        .o_buf_we       (mac_we),
        .o_tx_data      (o_tx_data),
        .o_tx_valid     (o_tx_valid),
        .i_tx_ready     (i_tx_ready),
        .i_rx_data      (i_rx_data),
        .i_rx_valid     (i_rx_valid),
        .i_rx_active    (i_rx_active),
        .i_rx_error     (i_rx_error),
        .i_line_state   (i_line_state),
        .i_caps         (i_caps),
        .o_xcvr_sel     (o_xcvr_sel),
        .o_term_sel     (o_term_sel),
        .o_opmode       (o_opmode),
        .o_port_power   (o_port_power)
    );
endmodule
