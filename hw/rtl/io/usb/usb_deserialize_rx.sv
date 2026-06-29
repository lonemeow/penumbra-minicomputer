// Penumbra USB receive deserializer (SIE line layer)
//
// The RX mirror of usb_serialize_tx: reassembles recovered data bits into bytes
// for the MAC. It sits at the top of the receive chain -- sampler ->
// usb_nrzi_decode -> usb_bit_unstuff_rx -> deserialize -> byte across the seam.
// USB sends each byte LSB first, so the first valid bit is bit 0 and every
// eighth valid bit completes a byte.
//
// Only real data bits are clocked in: i_valid mirrors the unstuffer's o_valid,
// which drops on a removed stuff 0, so the deserializer ignores those cycles.
// i_init re-establishes byte alignment at a packet boundary -- the SYNC
// detector strobes it so the next valid bit lands as bit 0 -- and marks a gap
// between packets, so it never rides a data-bit cycle.

module usb_deserialize_rx (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic       i_init,        // packet boundary: next valid bit is bit 0
    input  logic       i_valid,       // a recovered data bit is present this cycle
    input  logic       i_data_bit,    // the recovered data bit (LSB first)
    output logic [7:0] o_byte,        // assembled byte (meaningful when o_byte_valid)
    output logic       o_byte_valid   // a full byte completed this cycle
);
    logic [7:0] sr_q, sr_d;     // bits accumulate here; a new bit enters at the top
    logic [2:0] cnt_q, cnt_d;   // data bits gathered into the current byte (0..7)

    // A new bit shifts in at the MSB; after eight bits the first one received has
    // reached bit 0, so the register reads out LSB first.
    logic [7:0] sr_next;
    assign sr_next = {i_data_bit, sr_q[7:1]};

    // The eighth valid bit completes a byte this cycle.
    assign o_byte       = sr_next;
    assign o_byte_valid = i_valid && (cnt_q == 3'd7);

    always_comb begin
        sr_d  = sr_q;
        cnt_d = cnt_q;
        if (i_init)
            cnt_d = 3'd0;
        else if (i_valid) begin
            sr_d  = sr_next;
            cnt_d = (cnt_q == 3'd7) ? 3'd0 : cnt_q + 3'd1;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            sr_q  <= 8'd0;
            cnt_q <= 3'd0;
        end else begin
            sr_q  <= sr_d;
            cnt_q <= cnt_d;
        end
    end

    // Alignment strobe and a data bit are mutually exclusive by contract; a
    // coincidence would drop a bit and is a framing-layer wiring error.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (!(i_init && i_valid)))
        else $error("usb_deserialize_rx: i_init coincided with a data bit");
endmodule
