// Penumbra USB host port controller (MAC)
//
// Owns the single downstream port's physical state: it decodes the seam's
// raw line state into connect/speed status, drives the transceiver-control
// recipes for normal traffic, bus reset, and resume, and reports the
// PORT_STATUS view the software root hub manipulates.
//
//   * Connect/speed — as a host the pull-downs idle both lines low (SE0);
//     an attached device's pull-up raises exactly one line, and which line
//     identifies the speed before any packet is exchanged: D+ is the
//     full-speed pull-up, D- the low-speed one. The raw {D-, D+} line
//     state is debounced before any change is believed, so attach bounce
//     and packet traffic (J/K flips, EOP SE0s) never flap CONNECT.
//   * Bus reset — while PORT_CTRL.RESET is set the port drives the UTMI+
//     HS-termination state (xcvr 00, term 0, opmode 10), whose electrical
//     result is SE0; software times the >=10 ms hold. Detection pauses
//     while the host itself drives the bus, and ENABLED rises when a reset
//     completes on a connected port.
//   * Resume — PORT_CTRL.RESUME drives K via opmode 10 with the transmit
//     channel held at 00h (the composition muxes o_tx_override ahead of
//     the packet transmitter); the PHY appends the LS EOP when it drops.
//     Software times the >=20 ms hold.
//
// Every connect, speed, enable, or reset-completion change strobes
// o_change_evt toward the register tier's sticky IRQ_STATUS.PORT_CHANGE.

module usbhc_port #(
    // Line-state stability window before a connect-level change is
    // believed: long enough to ride through packet traffic and EOPs
    // (an EOP's SE0 is 2 bit times, ~1.3 us at low speed), short enough
    // to spot a real detach promptly. Software's root hub still applies
    // the USB spec's 100 ms attach debounce above this.
    parameter int unsigned DEBOUNCE_CLKS = 600   // 10 us at 60 MHz
) (
    input  logic       i_clk,
    input  logic       i_rst,
    // PORT_CTRL view (register tier; software-timed recipes)
    input  logic       i_power,        // enable port power / pull-downs
    input  logic       i_reset,        // drive bus reset (SE0) while set
    input  logic       i_suspend,
    input  logic       i_resume,       // drive resume K while set
    // PORT_STATUS view
    output logic       o_connect,
    output logic       o_enabled,
    output logic       o_reset_active,
    output logic       o_suspended,
    output logic [1:0] o_speed,        // usb_speed_e of the attached device
    output logic [1:0] o_line,         // raw line state (PORT_STATUS.LINE)
    output logic       o_change_evt,   // one-cycle strobe -> IRQ_STATUS.PORT_CHANGE
    // Seam status sidebands
    input  logic [1:0] i_line_state,   // raw {D-, D+}, SE0-glitch-filtered
    // The hs bit is consumed only by the future HS extension; the FS/LS
    // minimum polices just the low two capability bits.
/* verilator lint_off UNUSEDSIGNAL */
    input  logic [2:0] i_caps,         // PHY capability ceiling {hs, fs, ls}
/* verilator lint_on UNUSEDSIGNAL */
    // Seam transceiver controls
    output logic [1:0] o_xcvr_sel,
    output logic       o_term_sel,
    output logic [1:0] o_opmode,
    output logic       o_port_power,
    // Resume drive for the seam's transmit channel (composition mux)
    output logic       o_tx_override,
    output logic [7:0] o_tx_override_data
);
    import usb_pkg::*;

    // UTMI+ operational modes driven by this port (the seam doc holds the
    // full encoding table).
    localparam logic [1:0] OPMODE_NORMAL = 2'b00;
    localparam logic [1:0] OPMODE_RAW    = 2'b10;  // no bit-stuff/NRZI

    localparam int unsigned DB_W = $clog2(DEBOUNCE_CLKS + 1);

    // The debounced physical view. connect_q is a LEVEL — "a device is
    // attached now", backing PORT_STATUS.CONNECT — not an attach event;
    // the event strobe is change_q, marking commits that altered the view.
    logic       connect_q, connect_d;
    logic [1:0] speed_q, speed_d;         // attached device's pull-up speed;
                                          // meaningful while connect_q, held otherwise
    logic       enabled_q, enabled_d;
    logic [1:0] db_state_q, db_state_d;   // line state being qualified
    logic [DB_W-1:0] db_cnt_q, db_cnt_d;
    logic       change_q, change_d;
    logic       reset_prev_q;             // i_reset one cycle ago (edge detect)

    // The host drives the bus itself during reset and resume; the line
    // does not describe the device then, so detection pauses.
    logic driving_bus;
    assign driving_bus = i_reset || i_resume;
    logic detect_enable;
    assign detect_enable = i_power && !driving_bus;

    // What a committed line state means to a host port: exactly one raised
    // line is a device's pull-up and names its speed (D+ full, D- low).
    // SE0 is an empty port; SE1 (both high) has no legal electrical cause
    // and is a bus fault — neither is a device.
    logic       db_is_device;
    logic [1:0] db_speed;
    assign db_is_device = (db_state_q == USB_LINE_J) ||
                          (db_state_q == USB_LINE_K);
    assign db_speed     = (db_state_q == USB_LINE_K) ? USB_SPEED_LS
                                                     : USB_SPEED_FS;

    // The detector's decision points, named: a candidate that survived a
    // full window, the cycle-by-cycle commit condition (still observing
    // the qualified state, detection running), and whether committing
    // would actually alter the {connect, speed} view.
    logic db_qualified, db_commit, view_differs;
    assign db_qualified = (db_cnt_q == DB_W'(DEBOUNCE_CLKS));
    assign db_commit    = detect_enable && db_qualified &&
                          (i_line_state == db_state_q);
    assign view_differs = (db_is_device != connect_q) ||
                          (db_is_device && (db_speed != speed_q));

    assign o_connect      = connect_q;
    assign o_enabled      = enabled_q;
    assign o_reset_active = i_reset;
    assign o_suspended    = i_suspend;
    assign o_speed        = speed_q;
    assign o_line         = i_line_state;
    assign o_change_evt   = change_q;

    // Transceiver policy: bus reset selects the UTMI+ HS-termination state
    // (SE0 by electrical result — the caps carve-out every PHY implements);
    // resume drives raw K at the port speed; normal traffic runs the
    // detected speed with terminations on. Disconnected idles at full
    // speed, which every conformant PHY supports.
    always_comb begin
        if (i_reset) begin
            o_xcvr_sel = USB_SPEED_HS;
            o_term_sel = 1'b0;
            o_opmode   = OPMODE_RAW;
        end else if (i_resume) begin
            o_xcvr_sel = speed_q;
            o_term_sel = 1'b1;
            o_opmode   = OPMODE_RAW;
        end else begin
            o_xcvr_sel = connect_q ? speed_q : USB_SPEED_FS;
            o_term_sel = 1'b1;
            o_opmode   = OPMODE_NORMAL;
        end
    end
    assign o_port_power = i_power;

    // The resume recipe: the transmit channel holds 00h so the PHY drives
    // a continuous K (opmode raw disables bit-stuffing and NRZI).
    assign o_tx_override      = i_resume;
    assign o_tx_override_data = 8'h00;

    always_comb begin
        connect_d  = connect_q;
        speed_d    = speed_q;
        enabled_d  = enabled_q;
        db_state_d = db_state_q;
        db_cnt_d   = db_cnt_q;
        change_d   = 1'b0;

        // A completed bus reset enables a connected port; losing the
        // device disables it (the connect commit below owns that side).
        if (i_reset)
            enabled_d = 1'b0;
        if (!i_reset && reset_prev_q && connect_q) begin
            enabled_d = 1'b1;
            change_d  = 1'b1;
        end

        // The connect/speed detector, as three flat concerns.

        // Power off: there is no port. Park qualification at the empty
        // state and force the disconnected view, pull-up or not.
        if (!i_power) begin
            db_state_d = USB_LINE_SE0;
            db_cnt_d   = '0;
            if (connect_q) begin
                connect_d = 1'b0;
                enabled_d = 1'b0;
                change_d  = 1'b1;
            end
        end

        // Candidate qualification: a changed observation restarts the
        // window, a stable one ages until qualified. Neither arm runs
        // while the host drives the bus (reset/resume) — the line
        // describes us, not the device — so the pre-reset view and the
        // candidate's age simply ride through the driven SE0.
        if (detect_enable && i_line_state != db_state_q) begin
            db_state_d = i_line_state;
            db_cnt_d   = '0;
        end else if (detect_enable && !db_qualified) begin
            db_cnt_d = db_cnt_q + 1'b1;
        end

        // Commit: a qualified candidate is the view, recommitted
        // idempotently every cycle it holds. The event strobe marks only
        // a view that actually changed — attach, detach, SE1 fault, and
        // the direct polarity swap are all the same comparison — and
        // committing "no device" clears the enable.
        if (db_commit) begin
            connect_d = db_is_device;
            if (db_is_device)
                speed_d = db_speed;
            if (!db_is_device)
                enabled_d = 1'b0;
            if (view_differs)
                change_d = 1'b1;
        end

    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            connect_q  <= 1'b0;
            speed_q    <= USB_SPEED_FS;
            enabled_q  <= 1'b0;
            db_state_q <= USB_LINE_SE0;
            db_cnt_q   <= '0;
            change_q   <= 1'b0;
        end else begin
            connect_q  <= connect_d;
            speed_q    <= speed_d;
            enabled_q  <= enabled_d;
            db_state_q <= db_state_d;
            db_cnt_q   <= db_cnt_d;
            change_q   <= change_d;
        end
        reset_prev_q <= !i_rst && i_reset;
    end

    // The port never selects a transceiver mode beyond the PHY's ceiling;
    // the single carve-out is the bus-reset drive state, which every PHY
    // implements by contract. caps = {hs, fs, ls}.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (o_xcvr_sel == USB_SPEED_FS |-> i_caps[1]))
        else $error("usbhc_port: FS selected beyond the PHY ceiling");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (o_xcvr_sel == USB_SPEED_LS |-> i_caps[0]))
        else $error("usbhc_port: LS selected beyond the PHY ceiling");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (o_xcvr_sel == USB_SPEED_HS |-> i_reset))
        else $error("usbhc_port: HS transceiver code outside bus reset");

    // The qualification age never exceeds its window.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (db_cnt_q <= DB_W'(DEBOUNCE_CLKS)))
        else $error("usbhc_port: debounce counter exceeded the window");
endmodule
