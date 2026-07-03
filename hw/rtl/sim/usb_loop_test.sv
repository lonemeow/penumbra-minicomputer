// Penumbra USB transmit->receive loopback (simulation DUT)
//
// Wires the composed transmit chain (usb_tx_test) into the composed receive
// chain (usb_rx_test) the way the shared bus would: while the transmitter
// drives (o_oe), the receiver sees its pins; while it doesn't, the receiver
// sees idle J — the level a connected device's pull-up holds the undriven bus
// at. Bytes fed to the transmit side must come back out of the receive side,
// closing the loop over every SIE cell at once.

module usb_loop_test (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic [1:0] i_speed,       // usb_speed_e
    input  logic [7:0] i_byte,        // transmit-side byte channel
    input  logic       i_byte_valid,
    output logic       o_byte_ready,
    output logic       o_bit_en,      // transmit bit-time strobe (testbench pacing)
    output logic [7:0] o_byte,        // receive-side packet byte
    output logic       o_byte_valid,
    output logic       o_eop,         // receive-side end of packet
    output logic       o_error       // receive-side bit-stuff violation
);
    import usb_pkg::*;

    logic tx_dp, tx_dn, tx_oe;
    logic rx_dp, rx_dn;
    logic idle_dp, idle_dn;
    logic rx_active;

    usb_tx_test u_tx (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_speed        (i_speed),
        .i_byte         (i_byte),
        .i_byte_valid   (i_byte_valid),
        .o_byte_ready   (o_byte_ready),
        .o_dp           (tx_dp),
        .o_dn           (tx_dn),
        .o_oe           (tx_oe),
        .o_bit_en       (o_bit_en)
    );

    // The undriven bus rests at idle J via the device pull-up: on D+ at
    // full-speed, on D- at low-speed.
    assign idle_dp = (i_speed != USB_SPEED_LS);
    assign idle_dn = ~idle_dp;
    assign rx_dp   = tx_oe ? tx_dp : idle_dp;
    assign rx_dn   = tx_oe ? tx_dn : idle_dn;

    usb_rx_test u_rx (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_speed        (i_speed),
        .i_dp           (rx_dp),
        .i_dn           (rx_dn),
        .o_byte         (o_byte),
        .o_byte_valid   (o_byte_valid),
        .o_active       (rx_active),
        .o_eop          (o_eop),
        .o_error        (o_error)
    );

    // The receive chain's packet-active flag isn't part of the loop check.
    logic unused_rx_active;
    assign unused_rx_active = rx_active;
endmodule
