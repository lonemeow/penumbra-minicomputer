// Penumbra USB host packet receiver (MAC)
//
// Reassembles one received packet from the UTMI-shaped seam's byte channel
// and reports its classification when the packet window closes: PID (with
// the check-nibble verified), stored payload length, CRC16 verdict, and the
// PHY's line-error flag. The transaction FSM reads the result on o_done and
// maps it to XFER_STATUS; this module has no notion of what was expected.
//
//   * The first byte in the window is the PID; its upper nibble must be the
//     complement of the lower (o_pid_ok).
//   * For data-group PIDs the packet body is stored into the buffer as it
//     arrives — the two bytes that turn out to be the trailing CRC16
//     included, since nothing marks the boundary until EOP. o_len excludes
//     them, so software never reads that far (the device contract states
//     buffer content beyond RXLEN is not meaningful).
//   * The CRC16 folds every post-PID byte including the CRC field itself;
//     an intact packet leaves the LFSR at the fixed residual (the same
//     invariant sw/tools/usb_crc.py demonstrates), so no on-wire CRC is
//     ever re-formed on the receive side.
//   * A payload beyond the buffer sets o_overflow; writes clamp at the
//     buffer bound and o_len reports what was stored.
//
// Results latch at EOP and hold until the next packet's EOP, so a slow
// consumer always reads a settled classification. The byte path is
// single-cycle throughout, per the seam's HS forward-compatibility rule.

module usbhc_pkt_rx #(
    parameter int BUF_BYTES = 64   // data-buffer capacity (CAP's buffer size)
) (
    input  logic       i_clk,
    input  logic       i_rst,
    // MAC-PHY seam, receive half
    input  logic [7:0] i_rx_data,
    input  logic       i_rx_valid,    // i_rx_data is a fresh byte this cycle
    input  logic       i_rx_active,   // packet window (SYNC detected -> EOP)
    input  logic       i_rx_error,    // PHY bit-stuff / framing error
    // Data-buffer write port
    output logic [6:0] o_buf_addr,
    output logic [7:0] o_buf_data,
    output logic       o_buf_we,
    // Result, latched at EOP and held until the next packet completes
    output logic       o_done,        // one-cycle: the window closed, results valid
    output logic [3:0] o_pid,         // received PID code
    output logic       o_pid_ok,      // check nibble matched
    output logic [6:0] o_len,         // payload bytes stored in the buffer
    output logic       o_crc_ok,      // CRC16 residual held (data packets)
    output logic       o_err,         // PHY reported a line error in the window
    output logic       o_overflow     // payload exceeded the buffer
);
    import usb_pkg::*;

    // A good CRC16 folded over payload plus its own on-wire bytes always
    // lands on this remainder (derived from sw/tools/usb_crc.py; it is the
    // residual the USB 2.0 spec publishes).
    localparam logic [15:0] CRC16_RESIDUAL = 16'h800D;

    // Packet-window edges from the seam.
    logic active_q;
    logic pkt_start, pkt_end;
    assign pkt_start = i_rx_active && !active_q;
    assign pkt_end   = !i_rx_active && active_q;

    // In-window receive state.
    logic       have_pid_q, have_pid_d;   // the PID byte has arrived
    logic [3:0] pid_q, pid_d;
    logic       pid_ok_q, pid_ok_d;
    logic [7:0] n_q, n_d;                 // post-PID bytes so far (saturating)
    logic       err_q, err_d;             // line error seen in this window

    // Results, latched at EOP.
    logic [3:0] res_pid_q;
    logic       res_pid_ok_q;
    logic [6:0] res_len_q;
    logic       res_crc_ok_q;
    logic       res_err_q;
    logic       res_ovf_q;
    logic       done_q;

    logic byte_arrives, pid_byte, payload_byte;
    assign byte_arrives = i_rx_valid;
    assign pid_byte     = byte_arrives && !have_pid_q;
    assign payload_byte = byte_arrives && have_pid_q;

    // Only a data-group packet carries buffer payload; a malformed
    // handshake trailing extra bytes must not scribble the buffer.
    logic group_data;
    assign group_data = (pid_q[1:0] == USB_PID_GROUP_DATA);

    // CRC16 over every post-PID byte, including the on-wire CRC field.
    logic [15:0] crc16_rem;
    usb_crc16 u_crc16 (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_init  (pkt_start),
        .i_valid (payload_byte),
        .i_data  (i_rx_data),
        .o_crc   (crc16_rem)
    );

    // Payload length accounting at EOP: everything after the PID except the
    // two CRC bytes. The counter saturates, so a garbage stream cannot wrap
    // it back into looking short.
    logic [7:0] payload_len;
    logic       overflow;
    assign payload_len = (n_q >= 8'd2) ? n_q - 8'd2 : 8'd0;
    assign overflow    = payload_len > 8'(BUF_BYTES);

    always_comb begin
        have_pid_d = have_pid_q;
        pid_d      = pid_q;
        pid_ok_d   = pid_ok_q;
        n_d        = n_q;
        err_d      = err_q;
        o_buf_we   = 1'b0;
        o_buf_addr = 7'd0;
        o_buf_data = 8'd0;

        if (pkt_start) begin
            // Clear the whole classification, PID included — a window that
            // ends byteless must not report the previous packet's PID.
            have_pid_d = 1'b0;
            pid_d      = 4'd0;
            pid_ok_d   = 1'b0;
            n_d        = 8'd0;
            err_d      = 1'b0;
        end else if (i_rx_active) begin
            if (i_rx_error)
                err_d = 1'b1;

            if (pid_byte) begin
                pid_d      = i_rx_data[3:0];
                pid_ok_d   = (i_rx_data[7:4] == ~i_rx_data[3:0]);
                have_pid_d = 1'b1;
            end

            if (payload_byte) begin
                if (n_q != 8'hFF)
                    n_d = n_q + 8'd1;
                // The packet body is stored as it arrives, trailing CRC
                // bytes included: nothing marks the payload/CRC boundary
                // until EOP, and o_len already excludes them, so software
                // never reads that far. Stores clamp at the buffer bound.
                if (group_data && (n_q < 8'(BUF_BYTES))) begin
                    o_buf_we   = 1'b1;
                    o_buf_addr = 7'(n_q);
                    o_buf_data = i_rx_data;
                end
            end
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            active_q     <= 1'b0;
            have_pid_q   <= 1'b0;
            pid_q        <= 4'd0;
            pid_ok_q     <= 1'b0;
            n_q          <= 8'd0;
            err_q        <= 1'b0;
            res_pid_q    <= 4'd0;
            res_pid_ok_q <= 1'b0;
            res_len_q    <= 7'd0;
            res_crc_ok_q <= 1'b0;
            res_err_q    <= 1'b0;
            res_ovf_q    <= 1'b0;
            done_q       <= 1'b0;
        end else begin
            active_q   <= i_rx_active;
            have_pid_q <= have_pid_d;
            pid_q      <= pid_d;
            pid_ok_q   <= pid_ok_d;
            n_q        <= n_d;
            err_q      <= err_d;

            done_q <= pkt_end;
            if (pkt_end) begin
                res_pid_q    <= pid_q;
                res_pid_ok_q <= have_pid_q && pid_ok_q;
                res_len_q    <= overflow ? 7'(BUF_BYTES) : payload_len[6:0];
                // A packet shorter than the CRC field cannot have a valid
                // CRC, whatever the LFSR happens to hold.
                res_crc_ok_q <= (crc16_rem == CRC16_RESIDUAL) && (n_q >= 8'd2);
                res_err_q    <= err_q;
                res_ovf_q    <= overflow;
            end
        end
    end

    assign o_done     = done_q;
    assign o_pid      = res_pid_q;
    assign o_pid_ok   = res_pid_ok_q;
    assign o_len      = res_len_q;
    assign o_crc_ok   = res_crc_ok_q;
    assign o_err      = res_err_q;
    assign o_overflow = res_ovf_q;

    // Seam contract: bytes only arrive inside a packet window.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_rx_valid |-> i_rx_active))
        else $error("usbhc_pkt_rx: rx byte outside the packet window");

    // Stores never pass the buffer bound, whatever arrives on the wire.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (o_buf_we |-> o_buf_addr < 7'(BUF_BYTES)))
        else $error("usbhc_pkt_rx: buffer write beyond the bound");
endmodule
