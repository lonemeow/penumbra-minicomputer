// Penumbra USB transmit bit-stuffer (SIE line layer)
//
// USB sends no separate clock; the receiver recovers bit timing by
// oversampling the line and locking to NRZI edges. A long run of data 1s is,
// in NRZI, a stretch with no transitions — the sampler would drift off the bit
// boundary. To bound that, USB inserts a 0 into the transmitted stream after
// every six consecutive 1s, guaranteeing a transition at least every seventh
// bit; the receiver removes the stuffed 0 again.
//
// This module sits in the SIE between the byte serializer and the NRZI
// encoder. Each line-bit time (i_en) it emits one line bit. When it inserts a
// stuff bit, o_stuff is high and the presented data bit is NOT consumed —
// upstream must hold it and present it again next cycle (consumed =
// i_en && !o_stuff).

module usb_bit_stuff_tx (
    input  logic i_clk,
    input  logic i_rst,
    input  logic i_en,         // emit one line bit this cycle
    input  logic i_data_bit,   // next unstuffed data bit (held while o_stuff)
    output logic o_line_bit,   // line bit to hand to the NRZI encoder
    output logic o_stuff       // o_line_bit is an inserted 0; upstream holds
);
    // Insert a stuff 0 once six consecutive 1s have been emitted on the line.
    localparam logic [2:0] STUFF_AFTER = 3'd6;

    logic [2:0] ones_q;   // consecutive 1s emitted so far (0..6)
    logic [2:0] ones_d;   // next run count

    always_comb begin
        if (ones_q == STUFF_AFTER) begin
            o_stuff    = 1'b1;
            o_line_bit = 1'b0;
            ones_d     = 3'd0;
        end else begin
            o_stuff    = 1'b0;
            o_line_bit = i_data_bit;
            ones_d     = i_data_bit ? (ones_q + 3'd1) : 3'd0;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst)
            ones_q <= '0;
        else if (i_en)
            ones_q <= ones_d;
    end
endmodule
