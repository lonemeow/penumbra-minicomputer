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
//     The end-marker is believed only after at least one decoded 0 in the
//     window: an idle line decodes 1 continuously (J against the J-seeded
//     NRZI reference), so a 1 with no zero before it means the SOP was a
//     line-state excursion, not a packet -- a speed switch over an idle
//     line reads the stale filtered polarity as K for a cycle -- and the
//     window is abandoned back to idle instead of wedging in payload.
//     A real packet always shows the zero: the SOP K itself decodes 0
//     against the idle-J reference, even with every SYNC bit hub-stripped.
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

    // A SYNC zero has been decoded in the current window: the evidence that
    // a real packet is behind the SOP, armed clear at window entry.
    logic sync_zero_q, sync_zero_d;

    assign o_active = (state_q != S_IDLE);

    always_comb begin
        state_d      = state_q;
        sync_zero_d  = sync_zero_q;
        o_sync_done  = 1'b0;
        o_payload_en = 1'b0;
        o_eop        = 1'b0;

        case (state_q)
            S_IDLE: begin
                if (sop) begin
                    state_d     = S_SYNC;
                    sync_zero_d = 1'b0;
                end
            end
            S_SYNC: begin
                if (eop) begin
                    o_eop   = 1'b1;
                    state_d = S_IDLE;
                end else if (i_bit_en && !i_data_bit) begin
                    sync_zero_d = 1'b1;   // a SYNC zero: the window is real
                end else if (i_bit_en && sync_zero_q) begin
                    o_sync_done = 1'b1;
                    state_d     = S_PAYLOAD;
                end else if (i_bit_en) begin
                    state_d = S_IDLE;     // a 1 with no zero: spurious SOP
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
        if (i_rst) begin
            state_q     <= S_IDLE;
            sync_zero_q <= 1'b0;
        end else begin
            state_q     <= state_d;
            sync_zero_q <= sync_zero_d;
        end
    end

    // SYNC-end and payload-route belong to different states; never both at once.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (!(o_sync_done && o_payload_en)))
        else $error("usb_rx_framing: sync-done and payload-en asserted together");

    // The SYNC end-marker is only ever believed with a SYNC zero behind it.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (o_sync_done |-> sync_zero_q))
        else $error("usb_rx_framing: sync-done without a preceding SYNC zero");
endmodule
