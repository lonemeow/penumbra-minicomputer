// Penumbra USB NRZI encoder (SIE line layer)
//
// NRZI (non-return-to-zero inverted) carries the bit clock in the data: a 0
// bit flips the line level, a 1 bit holds it. Combined with bit-stuffing —
// which forces a 0 at least every seventh bit — this guarantees the receiver a
// transition to lock its oversampling clock to. The SYNC field at packet start
// establishes the level the first data bit is measured against.
//
// This is the TX-side transcoder, sitting between the bit-stuffer and the
// serializer's pin drive. The RX-side inverse is usb_nrzi_decode.
//
// The reference level is per packet: i_init (the framing layer's packet-start
// strobe) reloads it to idle J, the level the bus rests at between packets and
// the one the receiver measures the first SYNC bit against. Without it, a
// second packet would encode its SYNC relative to wherever the previous
// payload happened to end.

module usb_nrzi_encode (
    input  logic i_clk,
    input  logic i_rst,
    input  logic i_init,       // packet start: reload the reference to idle J
    input  logic i_en,         // encode one bit this cycle
    input  logic i_data_bit,   // bit-stuffed data bit in
    output logic o_line        // NRZI line level for this bit (J = 1)
);
    // The J/K symbol level convention across the SIE cells: J is 1.
    localparam logic LEVEL_J = 1'b1;

    logic level_q;   // line level held from the previous bit
    logic level_d;   // line level for the current bit

    // A 1 holds the level, a 0 toggles it.
    assign level_d = i_data_bit ? level_q : ~level_q;

    // The line output is the registered level: it drives a continuously-sampled
    // line, and upstream (the serializer/framing muxes) may change i_data_bit
    // between bit strobes, so a combinational output would wobble mid-bit. A
    // bit's symbol appears the clock after its i_en and holds a full bit time.
    assign o_line = level_q;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            level_q <= LEVEL_J;    // idle J at reset; i_init re-establishes it per packet
        else if (i_init)
            level_q <= LEVEL_J;
        else if (i_en)
            level_q <= level_d;
    end
endmodule
