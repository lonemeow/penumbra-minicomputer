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

module usb_nrzi_encode (
    input  logic i_clk,
    input  logic i_rst,
    input  logic i_en,         // encode one bit this cycle
    input  logic i_data_bit,   // bit-stuffed data bit in
    output logic o_line        // NRZI line level for this bit
);
    logic level_q;   // line level held from the previous bit
    logic level_d;   // line level for the current bit

    // A 1 holds the level, a 0 toggles it.
    assign level_d = i_data_bit ? level_q : ~level_q;
    assign o_line  = level_d;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            level_q <= 1'b0;   // SYNC re-establishes the real reference per packet
        else if (i_en)
            level_q <= level_d;
    end
endmodule
