// Penumbra USB transmit serializer (SIE line layer)
//
// Turns the MAC's byte-paced packet stream into the bit stream the rest of the
// transmit chain consumes: serializer -> usb_bit_stuff_tx -> usb_nrzi_encode ->
// line drive. USB transmits each byte LSB first, so this shifts the loaded byte
// out bit 0 through bit 7, matching usb_crc16's processing order.
//
// Two paced interfaces meet here. Upstream is a byte handshake (i_byte /
// i_byte_valid accepted when o_byte_ready): the serializer pulls a fresh byte
// only while empty, so the MAC can hold the next byte ready and the line never
// bubbles mid-packet. Downstream is the bit-time tick (i_en) with the stuffer's
// back-pressure folded in as i_hold: when the bit-stuffer inserts a stuff 0 it
// did not take our bit, so the serializer must hold it and present it again.
// The current bit is consumed only on i_en && !i_hold.
//
// The byte handshake runs on the 60 MHz domain clock, not the bit-time tick, so
// the registered state is not globally i_en-gated; bit advance is instead gated
// by the i_en && !i_hold "consumed" condition inside the next-state logic.

module usb_serialize_tx (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic       i_en,          // a line bit-time elapses this cycle
    input  logic       i_hold,        // downstream did not take the bit (stuff insertion); hold it
    input  logic [7:0] i_byte,        // next byte to transmit (sampled when o_byte_ready)
    input  logic       i_byte_valid,  // i_byte is valid this cycle
    output logic       o_byte_ready,  // serializer can accept a byte this cycle
    output logic       o_data_bit,    // current data bit (LSB first) for the bit-stuffer
    output logic       o_active       // a byte is loaded; o_data_bit is meaningful
);
    logic [7:0] sr_q, sr_d;        // shift register; bit 0 is the bit on the wire
    logic [2:0] cnt_q, cnt_d;      // index of the bit currently presented (0..7)
    logic       loaded_q, loaded_d; // a byte is loaded and being shifted out

    // Downstream takes the presented bit this cycle. The stuffer's stuff-bit
    // insertion (i_hold) steals a bit-time without consuming our data bit.
    logic advance;
    assign advance = i_en && !i_hold;

    // The final bit of the loaded byte is on the wire.
    logic last_bit;
    assign last_bit = (cnt_q == 3'd7);

    assign o_data_bit = sr_q[0];   // LSB first
    assign o_active   = loaded_q;

    always_comb begin
        sr_d         = sr_q;
        cnt_d        = cnt_q;
        loaded_d     = loaded_q;
        o_byte_ready = 1'b0;

        if (!loaded_q) begin
            o_byte_ready = 1'b1;
            if (i_byte_valid) begin
                sr_d     = i_byte;
                cnt_d    = '0;
                loaded_d = 1'b1;
            end
        end else if (advance) begin
            sr_d  = {1'b0, sr_q[7:1]};
            cnt_d = cnt_q + 1;
            if (last_bit) loaded_d = 1'b0;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            sr_q     <= 8'd0;
            cnt_q    <= 3'd0;
            loaded_q <= 1'b0;
        end else begin
            sr_q     <= sr_d;
            cnt_q    <= cnt_d;
            loaded_q <= loaded_d;
        end
    end
endmodule
