// Penumbra USB receive bit-unstuffer (SIE line layer)
//
// The RX-side inverse of usb_bit_stuff_tx: after six consecutive 1s the
// transmitter always inserted a 0, so the receiver removes that 0 from the
// recovered stream rather than passing it up as data. The removed bit is also
// a check — a 1 where the stuff 0 was due is a bit-stuff violation (seven 1s
// never occur legally in data), which raises o_error for the MAC to abort on
// (it feeds the seam's o_rx_error).
//
// Because it deletes bits, not every input bit yields an output: o_valid marks
// the cycles that carry real data. This mirrors the stuffer's o_stuff
// back-pressure, but the gap flows downstream instead of up.

module usb_bit_unstuff_rx (
    input  logic i_clk,
    input  logic i_rst,
    input  logic i_en,          // a received line bit is presented this cycle
    input  logic i_line_bit,    // post-NRZI received bit
    output logic o_data_bit,    // recovered data bit (when o_valid)
    output logic o_valid,       // o_data_bit is real data, not a removed stuff 0
    output logic o_error        // a 1 arrived where a stuff 0 was due
);
    // After six consecutive 1s, the next bit is a stuffed 0 to be removed.
    localparam logic [2:0] STUFF_AFTER = 3'd6;

    logic [2:0] ones_q;   // consecutive 1s passed up so far (0..6)
    logic [2:0] ones_d;

    always_comb begin
        if (ones_q == STUFF_AFTER) begin
            o_data_bit = 1'b1;
            o_valid    = 1'b0;
            o_error    = i_line_bit;
            ones_d     = 3'd0;
        end else begin
            o_data_bit = i_line_bit;
            o_valid    = 1'b1;
            o_error    = 1'b0;
            ones_d     = i_line_bit ? ones_q + 1 : 3'd0;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst)
            ones_q <= '0;
        else if (i_en)
            ones_q <= ones_d;
    end
endmodule
