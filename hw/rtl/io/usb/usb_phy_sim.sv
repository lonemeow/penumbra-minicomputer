// Penumbra USB PHY, byte-level simulation model (PHY tier)
//
// The testbench half of the MAC/PHY split: presents the same UTMI-shaped
// seam as usb_phy_ecp5, but replaces the line layer with a byte-level
// packet port for a behavioural device model (UsbDeviceSim), exported
// through the machine integration the way the SPI/SD model is. Bytes
// still move at real bit-time rates — SYNC lead-in, eight bit times per
// byte, EOP tail, all scaled by the selected speed — so the MAC's
// turnaround deadlines and inter-packet gaps stay honest against the
// same timing they meet on the wire.
//
// Device-port contract (the C++ bridge's view):
//   * Host packets arrive as o_pkt_valid/o_pkt_data byte pulses closed
//     by o_pkt_end (the EOP). What the wire would carry implicitly is
//     explicit here: o_keepalive replaces the low-speed bare EOP, and
//     o_bus_reset / o_resume expose the out-of-band drive states.
//   * Device responses use a credit handshake: the device presents a
//     byte on i_rx_valid/i_rx_data (with i_rx_last on the final one) and
//     holds it; each o_rx_ready pulse consumes the byte and asks for the
//     next. The device owns its turnaround — the window opens when it
//     presents the first byte — and the model owns the pacing.
//   * i_dev_connect/i_dev_speed play the pull-up: they set the idle line
//     state the port controller's connect detection reads.
//
// o_rx_error is tied low: a byte-level model has no bit-stuff or framing
// errors to flag. CRC-level corruption still travels through the bytes
// themselves, so the MAC's ERROR paths remain reachable.

module usb_phy_sim (
    input  logic       i_clk,        // the 60 MHz USB domain clock
    input  logic       i_rst,
    // MAC-PHY seam: clock, sourced by the PHY per UTMI
    output logic       o_clk,
    // MAC-PHY seam, transmit half
    input  logic [7:0] i_tx_data,
    input  logic       i_tx_valid,   // held through the packet; dropping it ends it
    output logic       o_tx_ready,   // one-cycle: the presented byte was consumed
    // MAC-PHY seam, receive half
    output logic [7:0] o_rx_data,
    output logic       o_rx_valid,
    output logic       o_rx_active,  // packet window (SYNC -> EOP)
    output logic       o_rx_error,
    // MAC-PHY seam, transceiver control and status
    input  logic [1:0] i_opmode,     // UTMI+ operational mode
    input  logic [1:0] i_xcvr_sel,   // usb_speed_e / UTMI+ XcvrSelect
    input  logic       i_term_sel,
    input  logic       i_port_power,
    output logic [1:0] o_line_state, // raw {D-, D+}
    output logic [2:0] o_caps,       // capability ceiling {hs, fs, ls}
    // Device port (machine-integration export, testbench-driven)
    output logic [7:0] o_pkt_data,   // host packet byte
    output logic       o_pkt_valid,  // one-cycle: o_pkt_data is fresh
    output logic       o_pkt_end,    // one-cycle: the packet's EOP
    output logic       o_keepalive,  // one-cycle: LS keep-alive on the wire
    output logic       o_bus_reset,  // level: the reset drive state holds
    output logic       o_resume,     // level: resume K is being driven
    output logic       o_rx_ready,   // one-cycle credit: byte consumed, next please
    input  logic       i_rx_valid,   // device presents a response byte
    input  logic [7:0] i_rx_data,
    input  logic       i_rx_last,    // this byte ends the response packet
    input  logic       i_dev_connect,// a device's pull-up is on the line
    input  logic [1:0] i_dev_speed   // usb_speed_e of that pull-up
);
    import usb_pkg::*;

    // UTMI+ operational modes (encoding shared with the seam's driver).
    // Non-driving (01) needs no handling here: there is no line to
    // release, and the normal-mode gate already ignores its channel.
    localparam logic [1:0] OPMODE_NORMAL = 2'b00;
    localparam logic [1:0] OPMODE_RAW    = 2'b10;

    assign o_clk      = i_clk;
    assign o_caps     = 3'b011;   // {hs = never, fs, ls}
    assign o_rx_error = 1'b0;

    // The out-of-band drive states, decoded as usb_phy_ecp5 does.
    logic reset_drive;
    logic resume_drive;
    assign reset_drive  = (i_xcvr_sel == USB_SPEED_HS) && !i_term_sel;
    assign resume_drive = (i_opmode == OPMODE_RAW) && !reset_drive &&
                          i_tx_valid;
    assign o_bus_reset  = reset_drive;
    assign o_resume     = resume_drive;

    // Bit-time pacing at the selected speed; a byte is eight bit times.
    logic [8:0] byte_clks;
    assign byte_clks = (i_xcvr_sel == USB_SPEED_LS)
                     ? 9'(8 * USB_OS_LS) : 9'(8 * USB_OS_FS);

    // ── Transmit half: seam bytes to device-port pulses ──────────────
    //
    // SYNC lead-in, then one consume per byte time; the packet closes
    // when the MAC drops the channel. A lone A5h at low speed is the
    // keep-alive: consumed and reported as the event the bare EOP would
    // be on the wire, holding until the channel drops like any packet.
    typedef enum logic [1:0] { T_IDLE, T_SYNC, T_BYTES, T_KA } tstate_e;
    tstate_e tstate_q, tstate_d;
    logic [8:0] tcnt_q, tcnt_d;

    logic tx_normal;
    assign tx_normal = (i_opmode == OPMODE_NORMAL) && i_tx_valid;

    logic ka_decode;
    assign ka_decode = (i_xcvr_sel == USB_SPEED_LS) && (i_tx_data == 8'hA5);

    always_comb begin
        tstate_d    = tstate_q;
        tcnt_d      = tcnt_q;
        o_tx_ready  = 1'b0;
        o_pkt_valid = 1'b0;
        o_pkt_data  = i_tx_data;
        o_pkt_end   = 1'b0;
        o_keepalive = 1'b0;

        case (tstate_q)
            T_IDLE: begin
                if (tx_normal) begin
                    if (ka_decode) begin
                        o_tx_ready  = 1'b1;
                        o_keepalive = 1'b1;
                        tstate_d    = T_KA;
                    end else begin
                        tcnt_d   = byte_clks;   // the SYNC lead-in
                        tstate_d = T_SYNC;
                    end
                end
            end
            T_SYNC: begin
                tcnt_d = tcnt_q - 9'd1;
                if (tcnt_q == 9'd1) begin
                    // The first byte is consumed as SYNC completes.
                    o_tx_ready  = 1'b1;
                    o_pkt_valid = 1'b1;
                    tcnt_d      = byte_clks;
                    tstate_d    = T_BYTES;
                end
            end
            T_BYTES: begin
                if (!i_tx_valid) begin
                    o_pkt_end = 1'b1;
                    tstate_d  = T_IDLE;
                end else begin
                    tcnt_d = tcnt_q - 9'd1;
                    if (tcnt_q == 9'd1) begin
                        o_tx_ready  = 1'b1;
                        o_pkt_valid = 1'b1;
                        tcnt_d      = byte_clks;
                    end
                end
            end
            T_KA: begin
                if (!i_tx_valid)
                    tstate_d = T_IDLE;
            end
            default: tstate_d = T_IDLE;
        endcase
    end

    // ── Receive half: the credit handshake, paced to the seam ────────
    //
    // The device opens a response by presenting its first byte; the
    // window rises at once (SYNC on the wire), the first byte lands a
    // SYNC time later, and each credit consumes the presented byte at
    // the byte rate. The last-flagged byte is followed by the EOP tail
    // before the window closes.
    typedef enum logic [1:0] { R_IDLE, R_SYNC, R_BYTES, R_EOP } rstate_e;
    rstate_e rstate_q, rstate_d;
    logic [8:0] rcnt_q, rcnt_d;
    logic       rlast_q, rlast_d;   // the consumed byte carried i_rx_last

    always_comb begin
        rstate_d   = rstate_q;
        rcnt_d     = rcnt_q;
        rlast_d    = rlast_q;
        o_rx_valid = 1'b0;
        o_rx_data  = i_rx_data;
        o_rx_ready = 1'b0;

        case (rstate_q)
            R_IDLE: begin
                if (i_rx_valid) begin
                    rcnt_d   = byte_clks;   // SYNC on the wire
                    rlast_d  = 1'b0;
                    rstate_d = R_SYNC;
                end
            end
            R_SYNC: begin
                rcnt_d = rcnt_q - 9'd1;
                if (rcnt_q == 9'd1) begin
                    rcnt_d   = 9'd1;        // deliver on the next cycle
                    rstate_d = R_BYTES;
                end
            end
            R_BYTES: begin
                rcnt_d = rcnt_q - 9'd1;
                if (rcnt_q == 9'd1) begin
                    // Consume the presented byte into the seam and
                    // credit the device for the next one.
                    o_rx_valid = 1'b1;
                    o_rx_ready = 1'b1;
                    rlast_d    = i_rx_last;
                    rcnt_d     = byte_clks;
                    if (i_rx_last) begin
                        // Two bit times of EOP close the window.
                        rcnt_d   = byte_clks >> 2;
                        rstate_d = R_EOP;
                    end
                end
            end
            R_EOP: begin
                rcnt_d = rcnt_q - 9'd1;
                if (rcnt_q == 9'd1)
                    rstate_d = R_IDLE;
            end
            default: rstate_d = R_IDLE;
        endcase
    end

    assign o_rx_active = (rstate_q == R_SYNC) || (rstate_q == R_BYTES) ||
                         (rstate_q == R_EOP);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tstate_q <= T_IDLE;
            tcnt_q   <= '0;
            rstate_q <= R_IDLE;
            rcnt_q   <= '0;
            rlast_q  <= 1'b0;
        end else begin
            tstate_q <= tstate_d;
            tcnt_q   <= tcnt_d;
            rstate_q <= rstate_d;
            rcnt_q   <= rcnt_d;
            rlast_q  <= rlast_d;
        end
    end

    // ── Line-state sideband: the pull-up, the recipes ────────────────
    //
    // Raw {D-, D+} as the connect detector reads it: the reset drive is
    // SE0 whatever hangs on the port; resume shows the driven K at the
    // port speed; otherwise a connected device's pull-up holds idle J at
    // its own speed's polarity, and an empty powered port reads SE0.
    logic [1:0] line_d;
    logic       xcvr_fs_pol, dev_fs_pol;
    assign xcvr_fs_pol = (i_xcvr_sel != USB_SPEED_LS);
    assign dev_fs_pol  = (i_dev_speed != USB_SPEED_LS);

    always_comb begin
        if (reset_drive)
            line_d = 2'b00;
        else if (resume_drive)
            line_d = xcvr_fs_pol ? 2'b10 : 2'b01;   // K
        else if (i_port_power && i_dev_connect)
            line_d = dev_fs_pol ? 2'b01 : 2'b10;    // idle J
        else
            line_d = 2'b00;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst)
            o_line_state <= 2'b00;
        else
            o_line_state <= line_d;
    end

    // The device must hold its presented byte until the credit, and the
    // MAC never runs the HS-only opmode at this port.
    assert property (@(posedge i_clk) disable iff (i_rst)
        ((rstate_q inside {R_SYNC, R_BYTES}) && !rlast_q |-> i_rx_valid))
        else $error("usb_phy_sim: response byte withdrawn before its credit");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_opmode != 2'b11))
        else $error("usb_phy_sim: HS opmode at a FS/LS port");
endmodule
