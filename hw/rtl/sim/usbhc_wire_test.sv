// Penumbra USBHC wire-level enumeration DUT (simulation)
//
// The full host stack against a device transceiver on a resolved wire:
// usbhc (register tier + CDC + MAC) drives usb_phy_ecp5 A exactly as a
// board top does; a second usb_phy_ecp5 B is the *device's* transceiver,
// its byte seam exported for the testbench's device model.  Unlike the
// byte-level device tests, everything the device learns here it learns
// from the wire: bus reset is 50 ms of measured SE0 on its line tap,
// not a friendly event — which is what lets a spec-strict device model
// (deaf until reset) verify the port bring-up end to end.
//
// The undriven wire idles at the polarity of i_speed, playing the
// attached device's pull-up, as in usb_phy_pair_test.

module usbhc_wire_test (
    // ── CPU clock domain: the usbhc bus interface ────────────────────
    input  logic        i_clk,
    input  logic        i_rst,
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
    input  logic [1:0]  i_speed,          // attached device speed (idle polarity)
    input  logic        i_dev_present,    // the device pull-up is on the line
    // Device-side seam (PHY B), driven by the testbench's device model
    input  logic [7:0]  i_dev_tx_data,
    input  logic        i_dev_tx_valid,
    output logic        o_dev_tx_ready,
    output logic [7:0]  o_dev_rx_data,
    output logic        o_dev_rx_valid,
    output logic        o_dev_rx_active,
    output logic        o_dev_rx_error,
    output logic [1:0]  o_dev_line_state, // the device's own line view
    // The resolved wire, for waveform-level checks
    output logic        o_bus_dp,
    output logic        o_bus_dn,
    output logic        o_a_oe,
    output logic        o_b_oe
);
    import usb_pkg::*;

    // ── Host stack: usbhc + PHY A ────────────────────────────────────
    logic [7:0] tx_data;
    logic       tx_valid, tx_ready;
    logic [7:0] rx_data;
    logic       rx_valid, rx_active, rx_error;
    logic [1:0] line_state;
    logic [2:0] caps;
    logic [1:0] xcvr_sel, opmode;
    logic       term_sel;
    logic [15:0] phy_dbg;
    logic       a_clk_unused, b_clk_unused;

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

    logic a_dp, a_dn, a_oe;
    logic b_dp, b_dn, b_oe;
    logic bus_dp, bus_dn;

    usb_phy_ecp5 u_phy_a (
        .i_clk        (i_usb_clk),
        .i_rst        (i_usb_rst),
        .o_clk        (a_clk_unused),
        .i_tx_data    (tx_data),
        .i_tx_valid   (tx_valid),
        .o_tx_ready   (tx_ready),
        .o_rx_data    (rx_data),
        .o_rx_valid   (rx_valid),
        .o_rx_active  (rx_active),
        .o_rx_error   (rx_error),
        .i_opmode     (opmode),
        .i_xcvr_sel   (xcvr_sel),
        .i_term_sel   (term_sel),
        .o_line_state (line_state),
        .o_caps       (caps),
        .i_dp         (bus_dp),
        .i_dn         (bus_dn),
        .o_tx_dp      (a_dp),
        .o_tx_dn      (a_dn),
        .o_tx_oe      (a_oe),
        /* verilator lint_off PINCONNECTEMPTY */
        .o_pull_dp    (),
        .o_pull_dn    (),
        /* verilator lint_on PINCONNECTEMPTY */
        .o_dbg        (phy_dbg)
    );

    // ── Device transceiver: PHY B on its seam ────────────────────────
    //
    // Normal traffic mode at the device's own speed; its host-side pull
    // outputs are ignored (the wire's idle polarity plays the pull-up).
    logic [2:0] b_caps_unused;

    usb_phy_ecp5 u_phy_b (
        .i_clk        (i_usb_clk),
        .i_rst        (i_usb_rst),
        .o_clk        (b_clk_unused),
        .i_tx_data    (i_dev_tx_data),
        .i_tx_valid   (i_dev_tx_valid),
        .o_tx_ready   (o_dev_tx_ready),
        .o_rx_data    (o_dev_rx_data),
        .o_rx_valid   (o_dev_rx_valid),
        .o_rx_active  (o_dev_rx_active),
        .o_rx_error   (o_dev_rx_error),
        .i_opmode     (2'b00),
        .i_xcvr_sel   (i_speed),
        .i_term_sel   (1'b1),
        .o_line_state (o_dev_line_state),
        .o_caps       (b_caps_unused),
        .i_dp         (bus_dp),
        .i_dn         (bus_dn),
        .o_tx_dp      (b_dp),
        .o_tx_dn      (b_dn),
        .o_tx_oe      (b_oe),
        /* verilator lint_off PINCONNECTEMPTY */
        .o_pull_dp    (),
        .o_pull_dn    (),
        .o_dbg        ()
        /* verilator lint_on PINCONNECTEMPTY */
    );

    // ── The wire: one driver at a time, idle from the pull-up ────────
    logic idle_dp, idle_dn;
    assign idle_dp = (i_speed != USB_SPEED_LS);
    assign idle_dn = ~idle_dp;

    always_comb begin
        if (a_oe) begin
            bus_dp = a_dp;
            bus_dn = a_dn;
        end else if (b_oe) begin
            bus_dp = b_dp;
            bus_dn = b_dn;
        end else if (i_dev_present) begin
            bus_dp = idle_dp;
            bus_dn = idle_dn;
        end else begin
            // No pull-up: the undriven bus reads SE0 (disconnect).
            bus_dp = 1'b0;
            bus_dn = 1'b0;
        end
    end

    assign o_bus_dp = bus_dp;
    assign o_bus_dn = bus_dn;
    assign o_a_oe   = a_oe;
    assign o_b_oe   = b_oe;
endmodule
