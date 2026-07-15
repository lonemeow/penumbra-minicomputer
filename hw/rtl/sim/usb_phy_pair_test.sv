// Penumbra USB PHY pair (simulation DUT)
//
// Two usb_phy_ecp5 instances on one resolved bus, host A and peer B: what
// A transmits arrives at B's seam as bytes and vice versa, proving the
// composed transmit and receive chains against each other at the wire
// level — the same closure usb_loop_test gave the bare SIE cells, now
// through the seam. The undriven bus idles at J via the attached device's
// pull-up (i_speed selects the polarity); i_force_en lets the testbench
// wiggle the wire directly for line-state scenarios.

module usb_phy_pair_test (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic [1:0] i_speed,        // usb_speed_e: idle polarity + both ports
    // Testbench wire override (models an external driver on the bus)
    input  logic       i_force_en,
    input  logic       i_force_dp,
    input  logic       i_force_dn,
    // Host A seam
    input  logic [7:0] i_a_tx_data,
    input  logic       i_a_tx_valid,
    output logic       o_a_tx_ready,
    output logic [7:0] o_a_rx_data,
    output logic       o_a_rx_valid,
    output logic       o_a_rx_active,
    output logic       o_a_rx_error,
    input  logic [1:0] i_a_opmode,
    input  logic [1:0] i_a_xcvr_sel,
    input  logic       i_a_term_sel,
    input  logic       i_a_port_power,
    output logic [1:0] o_a_line_state,
    output logic [2:0] o_a_caps,
    output logic       o_a_pull_dp,
    output logic       o_a_pull_dn,
    // Peer B seam (kept in normal traffic mode by the testbench)
    input  logic [7:0] i_b_tx_data,
    input  logic       i_b_tx_valid,
    output logic       o_b_tx_ready,
    output logic [7:0] o_b_rx_data,
    output logic       o_b_rx_valid,
    output logic       o_b_rx_active,
    output logic       o_b_rx_error,
    // The resolved bus, for waveform checks
    output logic       o_bus_dp,
    output logic       o_bus_dn,
    output logic       o_a_oe,
    output logic       o_b_oe
);
    import usb_pkg::*;

    logic a_dp, a_dn, a_oe;
    logic b_dp, b_dn, b_oe;
    logic idle_dp, idle_dn;
    logic bus_dp, bus_dn;

    assign idle_dp = (i_speed != USB_SPEED_LS);
    assign idle_dn = ~idle_dp;

    // One driver at a time: the testbench override, then whichever PHY
    // drives, then the pull-up's idle J.
    always_comb begin
        if (i_force_en) begin
            bus_dp = i_force_dp;
            bus_dn = i_force_dn;
        end else if (a_oe) begin
            bus_dp = a_dp;
            bus_dn = a_dn;
        end else if (b_oe) begin
            bus_dp = b_dp;
            bus_dn = b_dn;
        end else begin
            bus_dp = idle_dp;
            bus_dn = idle_dn;
        end
    end

    assign o_bus_dp = bus_dp;
    assign o_bus_dn = bus_dn;
    assign o_a_oe   = a_oe;
    assign o_b_oe   = b_oe;

    logic unused_a_clk, unused_b_clk;
    logic [2:0] unused_b_caps;
    logic [1:0] unused_b_line_state;
    logic unused_b_pull_dp, unused_b_pull_dn;

    usb_phy_ecp5 u_a (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .o_clk          (unused_a_clk),
        .i_tx_data      (i_a_tx_data),
        .i_tx_valid     (i_a_tx_valid),
        .o_tx_ready     (o_a_tx_ready),
        .o_rx_data      (o_a_rx_data),
        .o_rx_valid     (o_a_rx_valid),
        .o_rx_active    (o_a_rx_active),
        .o_rx_error     (o_a_rx_error),
        .i_opmode       (i_a_opmode),
        .i_xcvr_sel     (i_a_xcvr_sel),
        .i_term_sel     (i_a_term_sel),
        .i_port_power   (i_a_port_power),
        .o_line_state   (o_a_line_state),
        .o_caps         (o_a_caps),
        .i_dp           (bus_dp),
        .i_dn           (bus_dn),
        .o_tx_dp        (a_dp),
        .o_tx_dn        (a_dn),
        .o_tx_oe        (a_oe),
        .o_pull_dp      (o_a_pull_dp),
        .o_pull_dn      (o_a_pull_dn)
    );

    usb_phy_ecp5 u_b (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .o_clk          (unused_b_clk),
        .i_tx_data      (i_b_tx_data),
        .i_tx_valid     (i_b_tx_valid),
        .o_tx_ready     (o_b_tx_ready),
        .o_rx_data      (o_b_rx_data),
        .o_rx_valid     (o_b_rx_valid),
        .o_rx_active    (o_b_rx_active),
        .o_rx_error     (o_b_rx_error),
        .i_opmode       (2'b00),
        .i_xcvr_sel     (i_speed),
        .i_term_sel     (1'b1),
        .i_port_power   (1'b1),
        .o_line_state   (unused_b_line_state),
        .o_caps         (unused_b_caps),
        .i_dp           (bus_dp),
        .i_dn           (bus_dn),
        .o_tx_dp        (b_dp),
        .o_tx_dn        (b_dn),
        .o_tx_oe        (b_oe),
        .o_pull_dp      (unused_b_pull_dp),
        .o_pull_dn      (unused_b_pull_dn)
    );
endmodule
