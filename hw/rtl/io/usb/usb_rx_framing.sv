// Penumbra USB receive framing (SIE line layer)
//
// Brackets one received packet, turning the free-running bit stream into a
// delimited packet: it watches the line state for start- and end-of-packet and
// the recovered bit stream for the end of the SYNC field, and drives the
// control the rest of the receive chain needs.
//
//   * Start of packet -- the first K out of the idle J -- leaves idle.
//   * SYNC end -- SYNC is a run of 0s ended by a single 1 (that 1 is its last
//     bit), so the first decoded 1 marks it. This is deliberately an end-marker,
//     not a full SYNC-pattern match, so a hub that strips leading SYNC bits does
//     not defeat detection. The strobe aligns the deserializer (o_sync_done ->
//     its i_init) so the next payload bit lands as byte bit 0.
//   * End of packet -- SE0 -- returns to idle.
//
// During the payload it routes each decoded bit to the bit-unstuffer through
// o_payload_en; SYNC bits are not routed (SYNC is not bit-stuffed), so the
// deserializer only ever sees payload. Consumes the nrzi-decoded bit stream
// (i_bit_en / i_data_bit) and the usb_line_state verdict.

module usb_rx_framing (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic [1:0] i_line_state,   // usb_line_e from usb_line_state
    input  logic       i_bit_en,       // a decoded bit is available this cycle
    input  logic       i_data_bit,     // the nrzi-decoded bit (valid when i_bit_en)
    output logic       o_active,        // a packet is in progress (SOP..EOP)
    output logic       o_sync_done,     // SYNC ended: strobe the deserializer i_init
    output logic       o_payload_en,    // route this decoded bit to the bit-unstuffer
    output logic       o_eop            // end of packet this cycle
);
    import usb_pkg::*;

    typedef enum logic [1:0] { S_IDLE, S_SYNC, S_PAYLOAD } state_e;
    state_e state_q, state_d;

    // Start of packet is the first K out of idle; end of packet is SE0.
    logic sop, eop;
    assign sop = (i_line_state == USB_LINE_K);
    assign eop = (i_line_state == USB_LINE_SE0);

    assign o_active = (state_q != S_IDLE);

    always_comb begin
        state_d      = state_q;
        o_sync_done  = 1'b0;
        o_payload_en = 1'b0;
        o_eop        = 1'b0;

        case (state_q)
            S_IDLE: begin
                if (sop) state_d = S_SYNC;
            end
            S_SYNC: begin
                if (eop) begin
                    o_eop   = 1'b1;
                    state_d = S_IDLE;
                end else if (i_bit_en && i_data_bit) begin
                    o_sync_done = 1'b1;
                    state_d     = S_PAYLOAD;
                end
            end
            S_PAYLOAD: begin
                if (eop) begin
                    o_eop   = 1'b1;
                    state_d = S_IDLE;
                end else if (i_bit_en) begin
                    o_payload_en = 1'b1;   // route this bit to the bit-unstuffer
                end
            end
            default: state_d = S_IDLE;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) state_q <= S_IDLE;
        else       state_q <= state_d;
    end

    // SYNC-end and payload-route belong to different states; never both at once.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (!(o_sync_done && o_payload_en)))
        else $error("usb_rx_framing: sync-done and payload-en asserted together");
endmodule
