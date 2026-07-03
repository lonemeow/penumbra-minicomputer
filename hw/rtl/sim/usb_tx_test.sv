// Penumbra USB transmit-chain integration wrapper (simulation DUT)
//
// Composes the full SIE transmit path so the testbench can feed bytes and
// observe the D+/D- waveform:
//
//   byte -> usb_serialize_tx -> usb_bit_stuff_tx -> usb_nrzi_encode
//        -> line mux (SYNC from framing, SE0/J EOP override) -> D+/D- pins
//
// usb_tx_framing paces the bit clock and brackets the packet: it feeds the
// SYNC bits to the encoder ahead of the payload, seeds the stuffer's run
// count on SYNC's last bit, appends the SE0/SE0/J EOP below the encoder, and
// gates the line drive. The pin stage is registered, as a real PHY's IOB
// flops would be. This wiring is the shape the transmit half of
// usb_phy_<target> takes.

module usb_tx_test (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic [1:0] i_speed,       // usb_speed_e
    input  logic [7:0] i_byte,        // next packet byte (per usb_serialize_tx)
    input  logic       i_byte_valid,
    output logic       o_byte_ready,
    output logic       o_dp,          // D+ drive (valid while o_oe)
    output logic       o_dn,          // D- drive
    output logic       o_oe,          // line drive enable
    output logic       o_bit_en       // bit-time strobe (testbench pacing)
);
    import usb_pkg::*;

    logic ser_bit;
    logic ser_active;
    logic stuff_bit;
    logic stuff;
    logic sync_sel;
    logic sync_bit;
    logic payload_en;
    logic stuff_init;
    logic nrzi_en;
    logic nrzi_init;
    logic nrzi_line;
    logic se0;
    logic drive_j;
    logic oe;

    usb_serialize_tx u_serialize (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_en           (payload_en),
        .i_hold         (stuff),
        .i_byte         (i_byte),
        .i_byte_valid   (i_byte_valid),
        .o_byte_ready   (o_byte_ready),
        .o_data_bit     (ser_bit),
        .o_active       (ser_active)
    );

    usb_bit_stuff_tx u_stuff (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_init         (stuff_init),
        .i_en           (payload_en),
        .i_data_bit     (ser_bit),
        .o_line_bit     (stuff_bit),
        .o_stuff        (stuff)
    );

    usb_tx_framing u_framing (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_speed        (i_speed),
        .i_ser_active   (ser_active),
        .i_stuff        (stuff),
        .o_bit_en       (o_bit_en),
        .o_sync_sel     (sync_sel),
        .o_sync_bit     (sync_bit),
        .o_payload_en   (payload_en),
        .o_stuff_init   (stuff_init),
        .o_nrzi_en      (nrzi_en),
        .o_nrzi_init    (nrzi_init),
        .o_se0          (se0),
        .o_drive_j      (drive_j),
        .o_oe           (oe)
    );

    usb_nrzi_encode u_nrzi (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_init         (nrzi_init),
        .i_en           (nrzi_en),
        .i_data_bit     (sync_sel ? sync_bit : stuff_bit),
        .o_line         (nrzi_line)
    );

    // Line mux and speed mapping: EOP overrides the encoder (SE0 and the J
    // tail are raw line states, not symbols); a differential symbol maps to
    // the pins by speed (J is D+ high at full-speed, D- high at low-speed).
    logic symbol;
    logic fs_polarity;
    logic dp_d, dn_d;
    assign symbol      = drive_j ? 1'b1 : nrzi_line;
    assign fs_polarity = (i_speed != USB_SPEED_LS);
    always_comb begin
        if (se0) begin
            dp_d = 1'b0;
            dn_d = 1'b0;
        end else begin
            dp_d = fs_polarity ? symbol : ~symbol;
            dn_d = fs_polarity ? ~symbol : symbol;
        end
    end

    // Registered pin stage, as the PHY's IOB flops would be.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_dp <= 1'b0;
            o_dn <= 1'b0;
            o_oe <= 1'b0;
        end else begin
            o_dp <= dp_d;
            o_dn <= dn_d;
            o_oe <= oe;
        end
    end
endmodule
