// Penumbra USB receive-chain integration wrapper (simulation DUT)
//
// Composes the full SIE receive path so the testbench can drive raw D+/D-
// waveforms and observe bytes:
//
//   D+/D- -> usb_line_state -> usb_oversample_rx -> usb_nrzi_decode
//         -> usb_rx_framing -> usb_bit_unstuff_rx -> usb_deserialize_rx -> byte
//
// The framing FSM brackets the packet: its SYNC-done strobe initializes both
// the unstuffer's run count and the deserializer's byte alignment, and its
// payload gate keeps SYNC bits out of the unstuffer (SYNC is not stuffed).
// This wiring is the shape the receive half of usb_phy_<target> takes.

module usb_rx_test (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic [1:0] i_speed,       // usb_speed_e
    input  logic       i_dp,          // D+ receiver
    input  logic       i_dn,          // D- receiver
    output logic [7:0] o_byte,        // received packet byte
    output logic       o_byte_valid,  // o_byte completed this cycle
    output logic       o_active,      // packet in progress
    output logic       o_eop,         // end of packet this cycle
    output logic       o_error       // bit-stuff violation in the payload
);
    import usb_pkg::*;

    logic [1:0] line_state;
    logic       j_level;        // J/K symbol level for the sampler (J=1)
    logic       bit_en;         // recovered bit strobe
    logic       sampled_line;   // line level at the sample point
    logic       data_bit;       // nrzi-decoded bit
    logic       sync_done;
    logic       payload_en;
    logic       unstuff_bit;
    logic       unstuff_valid;

    assign j_level = (line_state == USB_LINE_J);

    usb_line_state u_line_state (
        .i_speed        (i_speed),
        .i_dp           (i_dp),
        .i_dn           (i_dn),
        .o_state        (line_state)
    );

    usb_oversample_rx u_oversample (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_speed        (i_speed),
        .i_line         (j_level),
        .o_bit_en       (bit_en),
        .o_line         (sampled_line)
    );

    usb_nrzi_decode u_nrzi (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_en           (bit_en),
        .i_line         (sampled_line),
        .o_data_bit     (data_bit)
    );

    usb_rx_framing u_framing (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_line_state   (line_state),
        .i_bit_en       (bit_en),
        .i_data_bit     (data_bit),
        .o_active       (o_active),
        .o_sync_done    (sync_done),
        .o_payload_en   (payload_en),
        .o_eop          (o_eop)
    );

    usb_bit_unstuff_rx u_unstuff (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_init         (sync_done),
        .i_en           (payload_en),
        .i_line_bit     (data_bit),
        .o_data_bit     (unstuff_bit),
        .o_valid        (unstuff_valid),
        .o_error        (o_error)
    );

    usb_deserialize_rx u_deserialize (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_init         (sync_done),
        .i_valid        (unstuff_valid),
        .i_data_bit     (unstuff_bit),
        .o_byte         (o_byte),
        .o_byte_valid   (o_byte_valid)
    );
endmodule
