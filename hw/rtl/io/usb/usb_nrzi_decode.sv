// Penumbra USB NRZI decoder (SIE line layer)
//
// The RX-side inverse of usb_nrzi_encode: it recovers each data bit from
// whether the received line level changed. The reference level is seeded at
// reset to match the encoder; in the full receiver the SYNC field
// re-establishes it per packet. Downstream, usb_bit_unstuff_rx (later) removes
// the stuffed 0s this layer hands up.

module usb_nrzi_decode (
    input  logic i_clk,
    input  logic i_rst,
    input  logic i_en,         // decode one received level this cycle
    input  logic i_line,       // received NRZI line level
    output logic o_data_bit    // recovered data bit
);
    logic prev_q;   // line level seen on the previous bit
    logic prev_d;

    assign o_data_bit = ~(i_line ^ prev_q);
    assign prev_d     = i_line;      // remember this level for the next bit

    always_ff @(posedge i_clk) begin
        if (i_rst)
            prev_q <= 1'b0;          // seeded to the encoder's reset level
        else if (i_en)
            prev_q <= prev_d;
    end
endmodule
