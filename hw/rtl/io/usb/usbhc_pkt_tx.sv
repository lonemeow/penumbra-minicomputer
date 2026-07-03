// Penumbra USB host packet transmitter (MAC)
//
// Streams one packet's bytes across the UTMI-shaped MAC-PHY seam: the PID
// byte, then whatever the PID's group implies, then the CRC bytes. The PHY
// frames SYNC and EOP around o_tx_valid; everything byte-shaped above that
// is formed here.
//
//   * token group (OUT / IN / SETUP / SOF) — PID, then the 11-bit field
//     ({ENDP, DEVADDR}, or the frame number for SOF) as two bytes with the
//     CRC5 in the top five bits of the second.
//   * data group (DATA0 / DATA1) — PID, i_len payload bytes pulled through
//     the buffer read port, then the two CRC16 bytes, low byte first.
//   * handshake group (ACK) — the bare PID.
//   * LS keep-alive (i_keepalive) — the bare SOF PID byte, which the PHY
//     decodes into a bare EOP per the seam contract.
//
// The bare CRC cells emit only the LFSR remainder; the on-wire form — the
// remainder complemented and bit-reversed — is formed here
// (sw/tools/usb_crc.py is the readable reference for both layers).
//
// The token CRC5 is fed bit-serially over the 11 cycles after i_start. That
// budget holds because the PHY spends SYNC plus two byte times (24 bit
// times) before it consumes the CRC-carrying third byte; an assertion below
// keeps the assumption honest. (A high-speed port would outrun it — a
// byte-parallel CRC5 belongs with the other additive HS MAC logic.) The
// payload path is single-cycle-per-byte throughout, per the seam's HS
// forward-compatibility rule.

module usbhc_pkt_tx (
    input  logic        i_clk,
    input  logic        i_rst,
    // Packet request; the fields are sampled on i_start
    input  logic        i_start,      // launch a packet (only while !o_busy)
    input  logic [3:0]  i_pid,        // usb_pid_e — the group selects the shape
    input  logic [10:0] i_field,      // token {ENDP, DEVADDR} / SOF frame number
    input  logic [6:0]  i_len,        // data-group payload bytes (0 = zero-length)
    input  logic        i_keepalive,  // transmit the bare PID (LS keep-alive)
    output logic        o_busy,       // packet in flight
    output logic        o_done,       // one-cycle: the PHY consumed the last byte
    // Data-buffer payload port: synchronous read, i_pl_data reflects
    // o_pl_addr on the following cycle (a BRAM read port)
    output logic [6:0]  o_pl_addr,
    input  logic [7:0]  i_pl_data,
    // MAC-PHY seam, transmit half
    output logic [7:0]  o_tx_data,
    output logic        o_tx_valid,   // held through the packet; dropping it ends it
    input  logic        i_tx_ready    // the PHY consumed the presented byte
);
    import usb_pkg::*;

    // One state per byte position: the state *is* the byte on the seam.
    typedef enum logic [2:0] {
        S_IDLE, S_PID, S_FIELD0, S_FIELD1, S_PAYLOAD, S_CRC_LO, S_CRC_HI
    } state_e;
    state_e state_q, state_d;

    // Request fields, held for the packet's duration.
    logic [3:0]  pid_q, pid_d;
    logic [10:0] field_q, field_d;
    logic [6:0]  len_q, len_d;
    logic        keepalive_q, keepalive_d;

    logic [6:0]  pl_idx_q, pl_idx_d;        // payload byte on the seam
    logic [10:0] crc5_sr_q, crc5_sr_d;      // token-field bits left to feed, bit 0 next
    logic [3:0]  crc5_left_q, crc5_left_d;  // how many remain

    // The PHY takes the presented byte this cycle.
    logic consume;
    assign consume = o_tx_valid && i_tx_ready;

    // Packet shape, decoded from the PID group.
    logic group_token, group_data, group_handshake;
    assign group_token     = (pid_q[1:0] == USB_PID_GROUP_TOKEN);
    assign group_data      = (pid_q[1:0] == USB_PID_GROUP_DATA);
    assign group_handshake = (pid_q[1:0] == USB_PID_GROUP_HANDSHAKE);

    // Token CRC5, fed bit-serially while the field shift register drains.
    logic       crc5_feed;
    logic [4:0] crc5_rem;
    assign crc5_feed = (crc5_left_q != 4'd0);
    usb_crc5 u_crc5 (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_init  (i_start),
        .i_valid (crc5_feed),
        .i_bit   (crc5_sr_q[0]),
        .o_crc   (crc5_rem)
    );

    // Payload CRC16, folded byte-parallel as the seam consumes each byte.
    logic [15:0] crc16_rem;
    usb_crc16 u_crc16 (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_init  (i_start),
        .i_valid (consume && (state_q == S_PAYLOAD)),
        .i_data  (i_pl_data),
        .o_crc   (crc16_rem)
    );

    // On-wire CRC formation above the bare remainders. USB transmits a CRC
    // field as the remainder's 1's complement, high-order term first; the
    // seam's bytes leave LSB first, so the value placed in the packet bytes
    // is the complemented remainder bit-reversed (usb_crc.py: token_crc5 /
    // data_crc16 compute exactly this layer).
    logic [4:0]  crc5_wire;
    logic [15:0] crc16_wire;
    assign crc5_wire  = { << {~crc5_rem} };
    assign crc16_wire = { << {~crc16_rem} };

    // The byte each state presents. crc16_wire's low byte leads on the wire.
    always_comb begin
        case (state_q)
            S_PID:     o_tx_data = {~pid_q, pid_q};
            S_FIELD0:  o_tx_data = field_q[7:0];
            S_FIELD1:  o_tx_data = {crc5_wire, field_q[10:8]};
            S_PAYLOAD: o_tx_data = i_pl_data;
            S_CRC_LO:  o_tx_data = crc16_wire[7:0];
            S_CRC_HI:  o_tx_data = crc16_wire[15:8];
            default:   o_tx_data = 8'd0;
        endcase
    end

    assign o_tx_valid = (state_q != S_IDLE);
    assign o_busy     = (state_q != S_IDLE);

    // The read port leads the seam by one byte: the next index is presented
    // combinationally, so the synchronous read settles while the current
    // byte is still on the seam — the pipelining that sustains one byte per
    // cycle when the seam runs that fast.
    assign o_pl_addr = pl_idx_d;

    always_comb begin
        state_d     = state_q;
        pid_d       = pid_q;
        field_d     = field_q;
        len_d       = len_q;
        keepalive_d = keepalive_q;
        pl_idx_d    = pl_idx_q;
        crc5_sr_d   = crc5_sr_q;
        crc5_left_d = crc5_left_q;
        o_done      = 1'b0;

        // The CRC5 feed drains on its own clock-by-clock schedule,
        // independent of the seam's byte pacing.
        if (crc5_feed) begin
            crc5_sr_d   = {1'b0, crc5_sr_q[10:1]};
            crc5_left_d = crc5_left_q - 4'd1;
        end

        case (state_q)
            S_IDLE: begin
                pl_idx_d = 7'd0;
                if (i_start) begin
                    pid_d       = i_pid;
                    field_d     = i_field;
                    len_d       = i_len;
                    keepalive_d = i_keepalive;
                    // Only a real token computes a CRC5; the keep-alive's
                    // SOF PID travels bare.
                    crc5_sr_d   = i_field;
                    crc5_left_d = (i_pid[1:0] == USB_PID_GROUP_TOKEN
                                   && !i_keepalive) ? 4'd11 : 4'd0;
                    state_d     = S_PID;
                end
            end
            S_PID: if (consume) begin
                if (keepalive_q || group_handshake) begin
                    state_d = S_IDLE;
                    o_done  = 1'b1;
                end else if (group_token) begin
                    state_d = S_FIELD0;
                end else if (group_data) begin
                    state_d = (len_q == 7'd0) ? S_CRC_LO : S_PAYLOAD;
                end
            end
            S_FIELD0: if (consume) state_d = S_FIELD1;
            S_FIELD1: if (consume) begin
                state_d = S_IDLE;
                o_done  = 1'b1;
            end
            S_PAYLOAD: if (consume) begin
                pl_idx_d = pl_idx_q + 7'd1;
                if (pl_idx_q == len_q - 7'd1)
                    state_d = S_CRC_LO;
            end
            S_CRC_LO: if (consume) state_d = S_CRC_HI;
            S_CRC_HI: if (consume) begin
                state_d = S_IDLE;
                o_done  = 1'b1;
            end
            default: state_d = S_IDLE;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q     <= S_IDLE;
            pid_q       <= 4'd0;
            field_q     <= 11'd0;
            len_q       <= 7'd0;
            keepalive_q <= 1'b0;
            pl_idx_q    <= 7'd0;
            crc5_sr_q   <= 11'd0;
            crc5_left_q <= 4'd0;
        end else begin
            state_q     <= state_d;
            pid_q       <= pid_d;
            field_q     <= field_d;
            len_q       <= len_d;
            keepalive_q <= keepalive_d;
            pl_idx_q    <= pl_idx_d;
            crc5_sr_q   <= crc5_sr_d;
            crc5_left_q <= crc5_left_d;
        end
    end

    // A start may only land on an idle transmitter; the arbitration above
    // (transaction FSM vs. frame timer) owns that exclusivity.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_start |-> state_q == S_IDLE))
        else $error("usbhc_pkt_tx: start while a packet is in flight");

    // The special PID group is outside the CLASS_USBHC minimum, and the
    // keep-alive is the SOF PID by definition.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_start |-> i_pid[1:0] != 2'b00))
        else $error("usbhc_pkt_tx: special-group PID requested");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_start && i_keepalive |-> i_pid == USB_PID_SOF))
        else $error("usbhc_pkt_tx: keep-alive with a non-SOF PID");

    // The serial CRC5 must have drained before its byte is presented — the
    // FS/LS byte pacing guarantees the 11-cycle budget; HS would not.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state_q == S_FIELD1 |-> crc5_left_q == 4'd0))
        else $error("usbhc_pkt_tx: CRC5 not ready at its byte position");
endmodule
