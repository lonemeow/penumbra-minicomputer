// Penumbra USB PHY, FPGA-native low/full speed (PHY tier)
//
// The swappable half of the MAC/PHY split: presents the UTMI-shaped seam
// upward and bit-bangs the raw D+/D- pins below it, composing the SIE
// cells in the shapes usb_tx_test and usb_rx_test proved. What the
// composition adds over those two chains:
//
//   * Drive-state decode — the seam's signaling recipes become line
//     drive: the UTMI+ HS-termination state (xcvr 00, term 0) drives SE0
//     directly (the FS/LS carve-out for bus reset); the raw opmode with
//     the transmit channel held drives a continuous K (resume), with the
//     low-speed EOP appended when the channel drops; the non-driving
//     opmode releases the line unconditionally.
//   * Keep-alive intercept — a lone A5h at low speed is a command to the
//     transceiver, not a byte to serialize: it is consumed at the seam
//     and answered with a bare EOP on the wire.
//   * Receive squelch — the packet receiver must not hear our own drive
//     (it would hand the MAC back every transmitted packet), so its line
//     tap is held at idle J while we transmit. The o_line_state sideband
//     stays on the raw pins — the port controller's debounce is built to
//     ride through visible traffic.
//   * Line-state filter — masks the false SE0 that D+/D- skew produces
//     at symbol transitions: SE0 must persist before it is reported,
//     any other state reports after one register delay.  Both the
//     port-controller sideband and the packet receiver's line tap read
//     the filtered line — an unfiltered receiver would take a skew
//     glitch for an EOP mid-packet.
//
// CRC5/CRC16 are not here — the MAC computes them, because a real
// ULPI/UTMI PHY does not. This module is FS/LS-only by physics (no HS
// terminations on the US2 pins); o_caps says so.

module usb_phy_ecp5 (
    input  logic       i_clk,        // 60 MHz USB clock (the PLL's CLKOS3)
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
    output logic       o_rx_active,  // packet window (SYNC detected -> EOP)
    output logic       o_rx_error,   // bit-stuff violation in the payload
    // MAC-PHY seam, transceiver control and status
    input  logic [1:0] i_opmode,     // UTMI+ operational mode
    input  logic [1:0] i_xcvr_sel,   // usb_speed_e / UTMI+ XcvrSelect
    input  logic       i_term_sel,
    output logic [1:0] o_line_state, // raw {D-, D+}, SE0-glitch-filtered
    output logic [2:0] o_caps,       // capability ceiling {hs, fs, ls}
    // US2 pins: the receive pair, the transmit pair, the pull enables
    input  logic       i_dp,
    input  logic       i_dn,
    output logic       o_tx_dp,      // drive levels (valid while o_tx_oe)
    output logic       o_tx_dn,
    output logic       o_tx_oe,
    output logic       o_pull_dp,    // host 15k pull-down enables; the board
    output logic       o_pull_dn,    // top maps them onto the pull pads
    // Receive-chain debug tap for the line-capture instrument — the
    // CAP_DATA sample layout (doc/system/devices/usb-host.md)
    output logic [15:0] o_dbg
);
    import usb_pkg::*;

    // UTMI+ operational modes (encoding shared with usbhc_port's driver
    // side of the seam).
    localparam logic [1:0] OPMODE_NORMAL   = 2'b00;
    localparam logic [1:0] OPMODE_NONDRIVE = 2'b01;
    localparam logic [1:0] OPMODE_RAW      = 2'b10;

    assign o_clk  = i_clk;
    assign o_caps = 3'b011;   // {hs = never, fs, ls}

    // Host pull-downs are a property of being a host port, not of port
    // power: a real host transceiver's 15k resistors sit on the lines
    // whether the port is powered or not, and an attached device may be
    // externally powered (the board's VBUS is hardwired), sampling its
    // line environment while our port is logically off.  Present them
    // unconditionally so the device never sees floating host lines.
    assign o_pull_dp = 1'b1;
    assign o_pull_dn = 1'b1;

    // The two out-of-band drive states, told apart by the transceiver
    // fields: bus reset is the HS-termination state; resume is the raw
    // opmode at a real port speed with the transmit channel held.
    logic reset_drive;
    logic resume_raw;
    assign reset_drive = (i_xcvr_sel == USB_SPEED_HS) && !i_term_sel;
    assign resume_raw  = (i_opmode == OPMODE_RAW) && !reset_drive;

    // J's pin polarity by speed: D+ carries J at full speed, D- at low
    // speed. Idle J is the level a connected device's pull-up holds.
    logic fs_polarity;
    logic idle_dp, idle_dn;
    assign fs_polarity = (i_xcvr_sel != USB_SPEED_LS);
    assign idle_dp     = fs_polarity;
    assign idle_dn     = ~fs_polarity;

    // ── Keep-alive intercept (seam byte channel) ─────────────────────
    //
    // At low speed a packet whose first byte is A5h (the SOF PID byte)
    // is the keep-alive: low speed has no SOF tokens, so the byte is
    // unambiguous at the moment it is presented. The intercept consumes
    // exactly that one byte at the seam, keeps it away from the
    // serializer, and fires the bare-EOP recipe instead; it holds until
    // the MAC ends the packet so a stray follow-on byte is never
    // serialized into a half-formed packet.
    logic ser_active;      // serializer holds packet data (chain output)
    logic ser_byte_ready;  // serializer consumed the presented byte
    logic rcp_idle;        // the line-recipe FSM is idle (defined below)

    logic ka_q, ka_d;      // intercept holds: a keep-alive byte was taken
    logic ka_fire;         // one-cycle: consume the byte, start the EOP
    logic ka_eligible;     // a fresh first byte that decodes as keep-alive
    assign ka_eligible = (i_opmode == OPMODE_NORMAL) &&
                         (i_xcvr_sel == USB_SPEED_LS) &&
                         i_tx_valid && !ka_q && !ser_active && rcp_idle &&
                         (i_tx_data == 8'hA5);

    always_comb begin
        ka_fire = ka_eligible;
        // The hold follows the packet window: taken on the fire, released
        // when the MAC drops the channel.
        if (ka_fire)
            ka_d = 1'b1;
        else if (!i_tx_valid)
            ka_d = 1'b0;
        else
            ka_d = ka_q;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst)
            ka_q <= 1'b0;
        else
            ka_q <= ka_d;
    end

    // The serializer sees the byte channel only for normal packets that
    // are not an intercepted keep-alive; the ready seam mirrors that,
    // adding the intercept's own consume pulse.
    logic ser_byte_valid;
    assign ser_byte_valid = i_tx_valid && (i_opmode == OPMODE_NORMAL) &&
                            !ka_fire && !ka_q;
    assign o_tx_ready     = ser_byte_ready || ka_fire;

    // ── Transmit chain (the usb_tx_test shape) ───────────────────────
    logic ser_bit, stuff_bit, stuff;
    logic sync_sel, sync_bit;
    logic payload_en, stuff_init;
    logic nrzi_en, nrzi_init, nrzi_line;
    logic pkt_se0, pkt_drive_j, pkt_oe;
    logic tx_bit_en;

    usb_serialize_tx u_serialize (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_en           (payload_en),
        .i_hold         (stuff),
        .i_byte         (i_tx_data),
        .i_byte_valid   (ser_byte_valid),
        .o_byte_ready   (ser_byte_ready),
        .o_data_bit     (ser_bit),
        .o_active       (ser_active)
    );

    usb_bit_stuff_tx u_stuff (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_init         (stuff_init),
        .i_en           (payload_en),
        .i_data_bit     (ser_bit),
        .o_line_bit     (stuff_bit),
        .o_stuff        (stuff)
    );

    usb_tx_framing u_tx_framing (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_speed        (i_xcvr_sel),
        .i_ser_active   (ser_active),
        .i_stuff        (stuff),
        .o_bit_en       (tx_bit_en),
        .o_sync_sel     (sync_sel),
        .o_sync_bit     (sync_bit),
        .o_payload_en   (payload_en),
        .o_stuff_init   (stuff_init),
        .o_nrzi_en      (nrzi_en),
        .o_nrzi_init    (nrzi_init),
        .o_se0          (pkt_se0),
        .o_drive_j      (pkt_drive_j),
        .o_oe           (pkt_oe)
    );

    usb_nrzi_encode u_nrzi_enc (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_init         (nrzi_init),
        .i_en           (nrzi_en),
        .i_data_bit     (sync_sel ? sync_bit : stuff_bit),
        .o_line         (nrzi_line)
    );

    // The transmit pacing strobe is internal to the chain here (the
    // integration DUT exported it for testbench pacing).
    logic unused_tx_bit_en;
    assign unused_tx_bit_en = tx_bit_en;

    // ── Line-recipe unit: resume K and the bare EOP ──────────────────
    //
    // The two transceiver-decoded shapes that bypass the packet chain,
    // sharing one generator because both end in the same low-speed EOP
    // (SE0 for two bit times, J for one): resume holds K for as long as
    // the MAC holds the raw transmit channel, then appends the EOP; a
    // keep-alive is the bare EOP alone.
    typedef enum logic [1:0] { R_IDLE, R_RESUME, R_EOP } rstate_e;
    rstate_e rstate_q, rstate_d;

    logic [5:0] rphase_q, rphase_d;     // clock counter within the LS bit time
    logic [1:0] reop_cnt_q, reop_cnt_d; // EOP bit-time index (SE0, SE0, J)

    logic rphase_tick;
    assign rphase_tick = (rphase_q == 6'(USB_OS_LS - 1));
    assign rcp_idle    = (rstate_q == R_IDLE);

    always_comb begin
        rstate_d   = rstate_q;
        rphase_d   = rphase_q;
        reop_cnt_d = reop_cnt_q;

        case (rstate_q)
            R_IDLE: begin
                rphase_d   = '0;
                reop_cnt_d = '0;
                if (resume_raw && i_tx_valid)
                    rstate_d = R_RESUME;
                else if (ka_fire)
                    rstate_d = R_EOP;
            end
            R_RESUME: begin
                rphase_d = '0;
                if (!(resume_raw && i_tx_valid))
                    rstate_d = R_EOP;
            end
            R_EOP: begin
                rphase_d = rphase_tick ? '0 : rphase_q + 6'd1;
                if (rphase_tick) begin
                    if (reop_cnt_q == 2'd2) begin
                        reop_cnt_d = '0;
                        rstate_d   = R_IDLE;
                    end else begin
                        reop_cnt_d = reop_cnt_q + 2'd1;
                    end
                end
            end
            default: rstate_d = R_IDLE;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            rstate_q   <= R_IDLE;
            rphase_q   <= '0;
            reop_cnt_q <= '0;
        end else begin
            rstate_q   <= rstate_d;
            rphase_q   <= rphase_d;
            reop_cnt_q <= reop_cnt_d;
        end
    end

    logic rcp_drive, rcp_se0, rcp_j;
    assign rcp_drive = (rstate_q != R_IDLE);
    assign rcp_se0   = (rstate_q == R_EOP) && (reop_cnt_q < 2'd2);
    assign rcp_j     = (rstate_q == R_EOP) && (reop_cnt_q == 2'd2);
    // R_RESUME with neither flag drives the K symbol.

    // ── Line drive mux (the usb_tx_test shape, plus the recipes) ─────
    //
    // Priority: the reset drive state, then the recipe unit, then the
    // packet chain, else the line is released to the device pull-up. The
    // non-driving opmode releases unconditionally.
    logic drive_se0, drive_symbol, drive_en;
    always_comb begin
        drive_en     = 1'b1;
        drive_se0    = 1'b0;
        drive_symbol = 1'b1;   // J
        if (reset_drive) begin
            drive_se0 = 1'b1;
        end else if (rcp_drive) begin
            drive_se0    = rcp_se0;
            drive_symbol = rcp_j;
        end else if (pkt_oe) begin
            drive_se0    = pkt_se0;
            drive_symbol = pkt_drive_j ? 1'b1 : nrzi_line;
        end else begin
            drive_en = 1'b0;
        end
        if (i_opmode == OPMODE_NONDRIVE)
            drive_en = 1'b0;
    end

    // Registered pin stage, the IOB flops.
    logic dp_d, dn_d;
    always_comb begin
        if (drive_se0) begin
            dp_d = 1'b0;
            dn_d = 1'b0;
        end else begin
            dp_d = fs_polarity ? drive_symbol : ~drive_symbol;
            dn_d = fs_polarity ? ~drive_symbol : drive_symbol;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_tx_dp <= 1'b0;
            o_tx_dn <= 1'b0;
            o_tx_oe <= 1'b0;
        end else begin
            o_tx_dp <= dp_d;
            o_tx_dn <= dn_d;
            o_tx_oe <= drive_en;
        end
    end

    // ── Receive chain (the usb_rx_test shape), squelched ─────────────
    //
    // While we drive the bus — through the pin flops' one-cycle lag —
    // the receiver's line tap holds idle J so the MAC never receives its
    // own transmissions.
    logic squelch;
    logic rx_dp, rx_dn;
    assign squelch = drive_en || o_tx_oe || reset_drive;
    // The receive tap reads the SE0-filtered line, not the raw pins:
    // D+/D- skew at a symbol transition reads as a false SE0 for a
    // sample or two, and an unfiltered receiver would take it for an
    // EOP and truncate the packet.  The filter delays all states one
    // clock uniformly (harmless to clock recovery) and SE0 by its
    // persistence window, leaving most of a real EOP's two bit times
    // still visible.
    assign rx_dp   = squelch ? idle_dp : line_q[0];
    assign rx_dn   = squelch ? idle_dn : line_q[1];

    logic [1:0] rx_line_state;
    logic       rx_j_level;
    logic       rx_bit_en, rx_sampled_line, rx_data_bit;
    logic       rx_sync_done, rx_payload_en;
    logic       rx_unstuff_bit, rx_unstuff_valid;
    logic       rx_eop;

    assign rx_j_level = (rx_line_state == USB_LINE_J);

    usb_line_state u_line_state (
        .i_speed        (i_xcvr_sel),
        .i_dp           (rx_dp),
        .i_dn           (rx_dn),
        .o_state        (rx_line_state)
    );

    usb_oversample_rx u_oversample (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_speed        (i_xcvr_sel),
        .i_line         (rx_j_level),
        .o_bit_en       (rx_bit_en),
        .o_line         (rx_sampled_line)
    );

    usb_nrzi_decode u_nrzi_dec (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_en           (rx_bit_en),
        .i_line         (rx_sampled_line),
        .o_data_bit     (rx_data_bit)
    );

    usb_rx_framing u_rx_framing (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_line_state   (rx_line_state),
        .i_bit_en       (rx_bit_en),
        .i_data_bit     (rx_data_bit),
        .o_active       (o_rx_active),
        .o_sync_done    (rx_sync_done),
        .o_payload_en   (rx_payload_en),
        .o_eop          (rx_eop)
    );

    usb_bit_unstuff_rx u_unstuff (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_init         (rx_sync_done),
        .i_en           (rx_payload_en),
        .i_line_bit     (rx_data_bit),
        .o_data_bit     (rx_unstuff_bit),
        .o_valid        (rx_unstuff_valid),
        .o_error        (o_rx_error)
    );

    usb_deserialize_rx u_deserialize (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_init         (rx_sync_done),
        .i_valid        (rx_unstuff_valid),
        .i_data_bit     (rx_unstuff_bit),
        .o_byte         (o_rx_data),
        .o_byte_valid   (o_rx_valid)
    );

    // The window edge is implicit in o_rx_active at the seam.
    logic unused_rx_eop;
    assign unused_rx_eop = rx_eop;

    // The capture instrument's per-clock sample of the receive chain
    // (CAP_DATA layout).  LINE is the post-squelch usb_line_e verdict
    // the chain acts on; DP/DN are the synchronized pins before it.
    assign o_dbg = {1'b0,
                    squelch,            // [14]
                    o_tx_oe,            // [13]
                    o_rx_valid,         // [12] BYTE_VALID
                    o_rx_error,         // [11] STUFF_ERR
                    rx_unstuff_valid,   // [10]
                    rx_payload_en,      // [9]
                    rx_sync_done,       // [8]
                    o_rx_active,        // [7]
                    rx_data_bit,        // [6]
                    rx_sampled_line,    // [5] BIT
                    rx_bit_en,          // [4]
                    rx_line_state,      // [3:2] LINE
                    dn_sync_q[1],       // [1]
                    dp_sync_q[1]};      // [0]

    // ── Line-state sideband, from the raw pins ───────────────────────
    //
    // D+/D- are asynchronous to this clock domain; two flops per pin
    // resolve metastability before any logic reads them.  The pins are
    // sensed single-ended, so a transition can still be captured one
    // clock apart between the pair — that residual skew is exactly
    // what the SE0 filter below absorbs.
    logic [1:0] dp_sync_q, dn_sync_q;
    always_ff @(posedge i_clk) begin
        dp_sync_q <= {dp_sync_q[0], i_dp};
        dn_sync_q <= {dn_sync_q[0], i_dn};
    end

    // A symbol transition can read as SE0 for a moment when D+/D- skew:
    // SE0 must hold for the filter window before it is believed, while
    // any driven state reports immediately. The window scales with the
    // bit time (a real EOP's SE0 is two bit times).
    localparam int unsigned SE0_FILT_FS = 2;
    localparam int unsigned SE0_FILT_LS = 14;

    logic [3:0] se0_cnt_q, se0_cnt_d;
    logic [1:0] line_q, line_d;
    logic [3:0] se0_window;
    logic [1:0] raw_line;
    assign se0_window = (i_xcvr_sel == USB_SPEED_LS) ? 4'(SE0_FILT_LS)
                                                     : 4'(SE0_FILT_FS);
    assign raw_line   = {dn_sync_q[1], dp_sync_q[1]};

    always_comb begin
        line_d    = line_q;
        se0_cnt_d = '0;
        if (raw_line != 2'b00) begin
            line_d = raw_line;
        end else if (se0_cnt_q >= se0_window) begin
            line_d = 2'b00;
        end else begin
            se0_cnt_d = se0_cnt_q + 4'd1;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            line_q    <= 2'b00;
            se0_cnt_q <= '0;
        end else begin
            line_q    <= line_d;
            se0_cnt_q <= se0_cnt_d;
        end
    end

    assign o_line_state = line_q;

    // The line has exactly one driver on our side: the packet chain and
    // the recipe unit never assert together (the MAC's arbiter keeps
    // packets out of the recipes' way), and a keep-alive fires only from
    // an idle channel. Opmode 11 (normal without SYNC/EOP) is an HS
    // mechanism no FS/LS MAC generates.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (!(pkt_oe && rcp_drive)))
        else $error("usb_phy_ecp5: packet chain and recipe drive together");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (ka_fire |-> !ser_active && rcp_idle))
        else $error("usb_phy_ecp5: keep-alive intercepted mid-packet");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_opmode != 2'b11))
        else $error("usb_phy_ecp5: HS opmode at a FS/LS port");
endmodule
