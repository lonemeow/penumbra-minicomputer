// Penumbra USB host transaction sequencer (MAC)
//
// Executes one USB transaction atomically above the packet engines — the
// token, the optional data stage, and the handshake — owning the turnaround
// timing a CPU cannot meet, and maps each outcome onto the
// XFER_STATUS.RESULT contract:
//
//   * OUT/SETUP — token, then the buffered payload as DATA0/DATA1
//     (i_toggle), then the device's handshake: ACK/NAK/STALL pass through;
//     a bad check nibble, a non-handshake response, or a line error is
//     ERROR; silence is TIMEOUT.
//   * IN — token, then the device's response: a CRC-good DATAx within the
//     accept bounds is acknowledged in hardware (the host ACK must fall
//     inside the device's turnaround window) and reported with its toggle
//     and length; NAK/STALL pass through; an errored or oversized packet
//     is not acknowledged, so the device offers it again.
//
// Buffer stores are granted to the receiver only while an IN response is
// awaited (o_rx_accept): a protocol-violating DATAx answer to an OUT must
// not scribble the transmit payload the buffer still holds.
//
// All waits are counted in bit times scaled by the port speed, since every
// USB deadline is expressed in bit times. The transmit command port assumes
// this sequencer owns the packet transmitter for the whole transaction (the
// MAC composition arbitrates SOF/keep-alive requests around it).

module usbhc_txn (
    input  logic        i_clk,
    input  logic        i_rst,
    // Transaction request (register tier); fields sampled on i_start
    input  logic        i_start,       // launch (only while !o_busy)
    input  logic [1:0]  i_pid_sel,     // USBHC_TOKEN_* (TOKEN.PID encoding)
    input  logic [6:0]  i_devaddr,
    input  logic [3:0]  i_endpoint,
    input  logic        i_toggle,      // DATA0/DATA1 of the OUT/SETUP data stage
    input  logic [6:0]  i_length,      // OUT/SETUP: bytes to send; IN: bytes to accept
    input  logic [1:0]  i_speed,       // usb_speed_e — scales every deadline
    output logic        o_busy,
    output logic        o_done,        // one-cycle: result outputs are valid
    output logic [2:0]  o_result,      // USBHC_RESULT_* (XFER_STATUS.RESULT)
    output logic [6:0]  o_rxlen,       // bytes stored by the IN response
    output logic        o_rxtoggle,    // toggle the IN response carried
    // Packet-transmitter command port (usbhc_pkt_tx)
    output logic        o_tx_start,
    output logic [3:0]  o_tx_pid,
    output logic [10:0] o_tx_field,
    output logic [6:0]  o_tx_len,
    input  logic        i_tx_done,
    // Packet-receiver result port (usbhc_pkt_rx) + the seam's window
    input  logic        i_rx_done,
    input  logic [3:0]  i_rx_pid,
    input  logic        i_rx_pid_ok,
    input  logic [6:0]  i_rx_len,
    input  logic        i_rx_crc_ok,
    input  logic        i_rx_err,
    input  logic        i_rx_ovf,
    input  logic        i_rx_active,
    // Buffer store grant: the composition ANDs this with the receiver's write
    output logic        o_rx_accept
);
    import penumbra_pkg::*;
    import usb_pkg::*;

    // Turnaround deadline, counted from the transmitter's last-byte consume:
    // the PHY still drains the final byte plus EOP (~11 bit times), the
    // device may answer up to 18 bit times after EOP, and the receiver only
    // opens its window once the 8-bit SYNC is detected; the rest is slop.
    localparam logic [5:0] TURNAROUND_BT = 6'd40;
    // Gap between two host packets of one transaction (token -> data): the
    // PHY drain (~11 bit times) plus the minimum idle the spec requires
    // between packets, so the gap exists on the wire and not just at the
    // seam.
    localparam logic [5:0] TX_GAP_BT = 6'd16;
    // Gap before the host ACK answers a received EOP: a couple of idle bit
    // times, comfortably inside the 16-bit-time budget the device allows.
    // The receive window closes at the line EOP, so no drain applies.
    localparam logic [5:0] ACK_GAP_BT = 6'd4;

    typedef enum logic [2:0] {
        S_IDLE, S_TOKEN, S_GAP_DATA, S_TXDATA, S_WAIT_HS,
        S_WAIT_RESP, S_GAP_ACK, S_ACK
    } state_e;
    state_e state_q, state_d;

    // Request fields, held for the transaction's duration.
    logic [1:0] pid_sel_q, pid_sel_d;
    logic [6:0] devaddr_q, devaddr_d;
    logic [3:0] endpoint_q, endpoint_d;
    logic       toggle_q, toggle_d;
    logic [6:0] length_q, length_d;

    // Bit-time pacing: a phase divider at the port speed's clocks-per-bit
    // ticks a saturating bit-time counter; both restart on state change.
    logic [5:0] os;
    assign os = (i_speed == USB_SPEED_LS) ? 6'(USB_OS_LS) : 6'(USB_OS_FS);
    logic [5:0] phase_q, phase_d;
    logic [5:0] bt_q, bt_d;

    logic rx_started_q, rx_started_d;   // the response's window opened

    // Results.
    logic [2:0] res_result_q;
    logic [6:0] res_rxlen_q, rxlen_q, rxlen_d;
    logic       res_rxtoggle_q, rxtoggle_q, rxtoggle_d;
    logic       done_q;
    logic       tx_go_q, tx_go_d;       // one-cycle start into the new state

    // Terminal transition: latch the result and return to idle.
    logic       finish;
    logic [2:0] finish_result;

    logic is_in;
    assign is_in = (pid_sel_q == USBHC_TOKEN_IN);

    logic [3:0] token_pid;
    always_comb begin
        case (pid_sel_q)
            USBHC_TOKEN_SETUP: token_pid = USB_PID_SETUP;
            USBHC_TOKEN_OUT:   token_pid = USB_PID_OUT;
            default:           token_pid = USB_PID_IN;
        endcase
    end

    // Response classification splits two concerns. Trust: a response with a
    // broken check nibble or a PHY-flagged line error cannot be believed,
    // whatever it claims to be. Shape: among trusted responses, a data
    // packet takes the data path and anything else is dispatched by its
    // PID (a full-PID compare implies its own group code).
    logic rx_trusted, rx_is_data, rx_over_length;
    assign rx_trusted     = i_rx_pid_ok && !i_rx_err;
    assign rx_is_data     = i_rx_pid_ok && (i_rx_pid[1:0] == USB_PID_GROUP_DATA);
    assign rx_over_length = i_rx_len > length_q;

    // Transmit command values by state; usbhc_pkt_tx samples them on the
    // o_tx_start pulse.
    always_comb begin
        case (state_q)
            S_TXDATA: o_tx_pid = toggle_q ? USB_PID_DATA1 : USB_PID_DATA0;
            S_ACK:    o_tx_pid = USB_PID_ACK;
            default:  o_tx_pid = token_pid;
        endcase
    end
    assign o_tx_field = {endpoint_q, devaddr_q};
    assign o_tx_len   = length_q;
    assign o_tx_start = tx_go_q;

    assign o_busy      = (state_q != S_IDLE);
    assign o_rx_accept = (state_q == S_WAIT_RESP);

    // A wait state times out only while no response window ever opened;
    // once the device starts answering, the packet runs to its own end.
    logic wait_timeout;
    assign wait_timeout = !rx_started_q && (bt_q >= TURNAROUND_BT);

    always_comb begin
        state_d       = state_q;
        pid_sel_d     = pid_sel_q;
        devaddr_d     = devaddr_q;
        endpoint_d    = endpoint_q;
        toggle_d      = toggle_q;
        length_d      = length_q;
        rxlen_d       = rxlen_q;
        rxtoggle_d    = rxtoggle_q;
        rx_started_d  = rx_started_q;
        tx_go_d       = 1'b0;
        finish        = 1'b0;
        finish_result = USBHC_RESULT_ERROR;

        // Free-running bit-time pacing; every state change restarts it.
        phase_d = (phase_q >= os - 6'd1) ? 6'd0 : phase_q + 6'd1;
        bt_d    = bt_q;
        if (phase_q >= os - 6'd1 && bt_q != 6'h3F)
            bt_d = bt_q + 6'd1;

        case (state_q)
            S_IDLE: begin
                if (i_start) begin
                    pid_sel_d  = i_pid_sel;
                    devaddr_d  = i_devaddr;
                    endpoint_d = i_endpoint;
                    toggle_d   = i_toggle;
                    length_d   = i_length;
                    rxlen_d    = 7'd0;
                    rxtoggle_d = 1'b0;
                    tx_go_d    = 1'b1;
                    state_d    = S_TOKEN;
                end
            end
            S_TOKEN: if (i_tx_done) begin
                rx_started_d = 1'b0;
                state_d      = is_in ? S_WAIT_RESP : S_GAP_DATA;
            end
            S_GAP_DATA: if (bt_q >= TX_GAP_BT) begin
                tx_go_d = 1'b1;
                state_d = S_TXDATA;
            end
            S_TXDATA: if (i_tx_done) begin
                rx_started_d = 1'b0;
                state_d      = S_WAIT_HS;
            end
            S_WAIT_HS: begin
                if (i_rx_active)
                    rx_started_d = 1'b1;
                if (i_rx_done) begin
                    // The OUT/SETUP handshake, dispatched by PID once the
                    // response is trusted; a data-group answer or an
                    // unexpected handshake code lands in the default.
                    finish = 1'b1;
                    if (!rx_trusted) begin
                        finish_result = USBHC_RESULT_ERROR;
                    end else begin
                        case (i_rx_pid)
                            USB_PID_ACK:   finish_result = USBHC_RESULT_ACK;
                            USB_PID_NAK:   finish_result = USBHC_RESULT_NAK;
                            USB_PID_STALL: finish_result = USBHC_RESULT_STALL;
                            default:       finish_result = USBHC_RESULT_ERROR;
                        endcase
                    end
                end else if (wait_timeout) begin
                    finish        = 1'b1;
                    finish_result = USBHC_RESULT_TIMEOUT;
                end
            end
            S_WAIT_RESP: begin
                if (i_rx_active)
                    rx_started_d = 1'b1;
                if (i_rx_done) begin
                    // The IN response's stored length and toggle are
                    // reported whatever the verdict; classification decides
                    // whether hardware acknowledges.
                    if (rx_is_data) begin
                        rxlen_d    = i_rx_len;
                        rxtoggle_d = i_rx_pid[3];
                    end
                    if (!rx_trusted) begin
                        finish        = 1'b1;
                        finish_result = USBHC_RESULT_ERROR;
                    end else if (rx_is_data) begin
                        // Broken data is never acknowledged — the silent
                        // host is USB's retry mechanism, so the device
                        // offers the packet again. Oversized-but-intact
                        // data is OVERFLOW, likewise unacknowledged.
                        if (!i_rx_crc_ok) begin
                            finish        = 1'b1;
                            finish_result = USBHC_RESULT_ERROR;
                        end else if (i_rx_ovf || rx_over_length) begin
                            finish        = 1'b1;
                            finish_result = USBHC_RESULT_OVERFLOW;
                        end else begin
                            state_d = S_GAP_ACK;   // acceptable: hardware ACK
                        end
                    end else begin
                        finish = 1'b1;
                        case (i_rx_pid)
                            USB_PID_NAK:   finish_result = USBHC_RESULT_NAK;
                            USB_PID_STALL: finish_result = USBHC_RESULT_STALL;
                            // A handshake that answers no question here,
                            // e.g. an ACK to an IN token.
                            default:       finish_result = USBHC_RESULT_ERROR;
                        endcase
                    end
                end else if (wait_timeout) begin
                    finish        = 1'b1;
                    finish_result = USBHC_RESULT_TIMEOUT;
                end
            end
            S_GAP_ACK: if (bt_q >= ACK_GAP_BT) begin
                tx_go_d = 1'b1;
                state_d = S_ACK;
            end
            S_ACK: if (i_tx_done) begin
                finish        = 1'b1;
                finish_result = USBHC_RESULT_ACK;
            end
            default: state_d = S_IDLE;
        endcase

        if (finish)
            state_d = S_IDLE;
        if (state_d != state_q) begin
            phase_d = 6'd0;
            bt_d    = 6'd0;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q        <= S_IDLE;
            pid_sel_q      <= 2'd0;
            devaddr_q      <= 7'd0;
            endpoint_q     <= 4'd0;
            toggle_q       <= 1'b0;
            length_q       <= 7'd0;
            phase_q        <= 6'd0;
            bt_q           <= 6'd0;
            rx_started_q   <= 1'b0;
            rxlen_q        <= 7'd0;
            rxtoggle_q     <= 1'b0;
            tx_go_q        <= 1'b0;
            done_q         <= 1'b0;
            res_result_q   <= 3'd0;
            res_rxlen_q    <= 7'd0;
            res_rxtoggle_q <= 1'b0;
        end else begin
            state_q      <= state_d;
            pid_sel_q    <= pid_sel_d;
            devaddr_q    <= devaddr_d;
            endpoint_q   <= endpoint_d;
            toggle_q     <= toggle_d;
            length_q     <= length_d;
            phase_q      <= phase_d;
            bt_q         <= bt_d;
            rx_started_q <= rx_started_d;
            rxlen_q      <= rxlen_d;
            rxtoggle_q   <= rxtoggle_d;
            tx_go_q      <= tx_go_d;

            done_q <= finish;
            if (finish) begin
                res_result_q   <= finish_result;
                res_rxlen_q    <= rxlen_d;
                res_rxtoggle_q <= rxtoggle_d;
            end
        end
    end

    assign o_done     = done_q;
    assign o_result   = res_result_q;
    assign o_rxlen    = res_rxlen_q;
    assign o_rxtoggle = res_rxtoggle_q;

    // A start may only land on an idle sequencer, and the reserved TOKEN.PID
    // code never launches; the register tier owns both.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_start |-> state_q == S_IDLE))
        else $error("usbhc_txn: start while a transaction is in flight");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_start |-> i_pid_sel != 2'b11))
        else $error("usbhc_txn: reserved TOKEN.PID code");

    // The pacer never runs past the current bit period.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (phase_q < os))
        else $error("usbhc_txn: phase counter exceeded the bit period");
endmodule
