// Penumbra USB transmit framing (SIE line layer)
//
// Brackets one transmitted packet, the TX mirror of usb_rx_framing: it paces
// the bit clock, walks the packet phases, and drives the control the transmit
// datapath (serializer -> bit-stuffer -> NRZI encoder -> line drive) needs.
//
//   * Pacing — transmit owns the bit clock (receive recovers it): a free
//     divider ticks once per bit time, 5x/40x of the 60 MHz clock by speed.
//   * SYNC — eight bits (seven 0s, a 1) fed to the NRZI encoder ahead of the
//     payload via o_sync_sel/o_sync_bit. The encoder's reference is reloaded
//     to idle J at packet start (o_nrzi_init), and the stuffer's run count is
//     seeded on SYNC's last bit (o_stuff_init) — USB counts that 1 as the
//     first bit of the stuff run. SYNC itself is never stuffed.
//   * Payload — each bit time advances the stuffer (o_payload_en) and the
//     encoder (o_nrzi_en); the serializer's hold-on-stuff is wired outside.
//   * EOP — not a symbol: SE0 driven raw below NRZI for two bit times, one
//     bit time of driven J, then the line drive releases (o_oe).
//
// A packet starts when the serializer holds its first byte (i_ser_active) —
// the MAC expresses packet timing by when it presents bytes.

module usb_tx_framing (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic [1:0] i_speed,        // usb_speed_e: selects the bit period
    input  logic       i_ser_active,   // serializer holds packet data
    input  logic       i_stuff,        // stuffer is inserting a stuff bit
    output logic       o_bit_en,       // one-cycle strobe starting each bit time
    output logic       o_sync_sel,     // encoder input selects the SYNC bit
    output logic       o_sync_bit,     // SYNC field bit (while o_sync_sel)
    output logic       o_payload_en,   // stuffer advances this bit time
    output logic       o_stuff_init,   // seed the stuffer's run count (SYNC's ending 1)
    output logic       o_nrzi_en,      // encoder advances this bit time
    output logic       o_nrzi_init,    // reload the encoder reference to idle J
    output logic       o_se0,          // drive SE0 (EOP)
    output logic       o_drive_j,      // drive J (the EOP tail bit time)
    output logic       o_oe            // line drive enable (SOP through EOP tail)
);
    import usb_pkg::*;

    typedef enum logic [1:0] { S_IDLE, S_SYNC, S_PAYLOAD, S_EOP } state_e;
    state_e state_q, state_d;

    // Bit period in clocks, from the port speed.
    logic [5:0] div;
    assign div = (i_speed == USB_SPEED_LS) ? 6'(USB_OS_LS) : 6'(USB_OS_FS);

    logic [5:0] phase_q, phase_d;       // clock counter within the bit time
    logic [2:0] sync_cnt_q, sync_cnt_d; // SYNC bit index (0..7)
    logic [1:0] eop_cnt_q, eop_cnt_d;   // EOP bit-time index (SE0, SE0, J)

    // Each bit time begins on its first clock; the datapath advances there so
    // the new symbol holds for the whole bit time.
    logic bit_en;
    assign bit_en  = (state_q != S_IDLE) && (phase_q == 6'd0);
    assign o_bit_en = bit_en;

    assign o_sync_sel = (state_q == S_SYNC);
    assign o_sync_bit = (sync_cnt_q == 3'd7);
    assign o_se0      = (state_q == S_EOP) && (eop_cnt_q < 2'd2);
    assign o_drive_j  = (state_q == S_EOP) && (eop_cnt_q == 2'd2);
    assign o_oe       = (state_q != S_IDLE);

    always_comb begin
        state_d      = state_q;
        phase_d      = phase_q;
        sync_cnt_d   = sync_cnt_q;
        eop_cnt_d    = eop_cnt_q;
        o_nrzi_init  = 1'b0;
        o_stuff_init = 1'b0;
        o_payload_en = 1'b0;
        o_nrzi_en    = 1'b0;

        // The pacer runs while a packet is active.
        if (state_q != S_IDLE)
            phase_d = (phase_q == div - 6'd1) ? 6'd0 : phase_q + 6'd1;

        case (state_q)
            S_IDLE: begin
                if (i_ser_active) begin
                    // Packet start: reload the encoder's idle-J reference now;
                    // the first SYNC bit time begins on the next clock.
                    o_nrzi_init = 1'b1;
                    sync_cnt_d  = 3'd0;
                    phase_d     = 6'd0;
                    state_d     = S_SYNC;
                end
            end
            S_SYNC: begin
                if (bit_en) begin
                    o_nrzi_en  = 1'b1;
                    sync_cnt_d = sync_cnt_q + 3'd1;
                    if (sync_cnt_q == 3'd7) begin
                        // SYNC's last bit is on the line this bit time; its 1
                        // seeds the stuff run before the first payload bit.
                        o_stuff_init = 1'b1;
                        state_d      = S_PAYLOAD;
                    end
                end
            end
            S_PAYLOAD: begin
                if (bit_en) begin
                    // Each bit time either transmits one more line bit or
                    // begins the EOP. A stuff bit owed after the serializer
                    // empties (a payload ending in six 1s) still belongs on
                    // the line, so i_stuff extends the packet past
                    // i_ser_active dropping. Asserting both enables is
                    // correct whether the bit is data or stuff — the
                    // serializer's hold-during-stuff is wired outside.
                    if (i_ser_active || i_stuff) begin
                        o_payload_en = 1'b1;
                        o_nrzi_en    = 1'b1;
                    end else begin
                        state_d   = S_EOP;
                        eop_cnt_d = 2'd0;
                    end
                end
            end
            S_EOP: begin
                if (bit_en) begin
                    eop_cnt_d = eop_cnt_q + 2'd1;
                    if (eop_cnt_q == 2'd2)
                        state_d = S_IDLE;
                end
            end
            default: state_d = S_IDLE;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q    <= S_IDLE;
            phase_q    <= 6'd0;
            sync_cnt_q <= 3'd0;
            eop_cnt_q  <= 2'd0;
        end else begin
            state_q    <= state_d;
            phase_q    <= phase_d;
            sync_cnt_q <= sync_cnt_d;
            eop_cnt_q  <= eop_cnt_d;
        end
    end

    // The pacer never runs past the current bit period.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (phase_q < div))
        else $error("usb_tx_framing: phase counter exceeded the bit period");
endmodule
