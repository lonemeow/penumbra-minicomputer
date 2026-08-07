// Penumbra USB host MAC (board-neutral)
//
// Composes the five MAC leaf cells — packet transmitter/receiver,
// transaction sequencer, frame timer, port controller — behind the
// UTMI-shaped seam. The composition owns exactly the glue the cells
// delegated upward:
//
//   * The transmit arbiter: the packet transmitter is shared between the
//     transaction sequencer and the frame timer's SOF/keep-alive markers.
//     A marker never splits a transaction, and a register-tier start that
//     arrives while a marker holds the transmitter is deferred, not lost
//     (o_busy covers the deferral so the "launch only while !o_busy"
//     contract composes).
//   * The command mux and done routing: whoever owns the transmitter has
//     its command port wired through, and receives the completion strobe.
//   * The seam transmit mux: the port controller's resume drive overrides
//     the packet path (a continuous 00h holds the K state).
//   * The receive store gate: buffer writes reach the data buffer only
//     under the sequencer's IN-response grant.
//
// The data-buffer ports face the register tier's dual-clock BRAM
// (usbhc_cdc); everything here runs in the seam's 60 MHz clock domain.
// The transaction request fields are register outputs and must hold
// stable while o_busy — a deferred start samples them at launch time.

module usbhc_mac #(
    // Passed through to the leaf cells; overridable so testbenches can
    // run short frames and fast connect detection.
    parameter int unsigned CLKS_PER_MS   = 60_000,
    parameter int unsigned DEBOUNCE_CLKS = 600,
    parameter int          BUF_BYTES     = 64
) (
    input  logic        i_clk,
    input  logic        i_rst,
    // Transaction request (register tier); launch only while !o_busy
    input  logic        i_start,
    input  logic [1:0]  i_pid_sel,     // USBHC_TOKEN_* (TOKEN.PID encoding)
    input  logic [6:0]  i_devaddr,
    input  logic [3:0]  i_endpoint,
    input  logic        i_toggle,
    input  logic [6:0]  i_length,
    output logic        o_busy,
    output logic        o_done,        // one-cycle: result outputs are valid
    output logic [2:0]  o_result,      // USBHC_RESULT_* (XFER_STATUS.RESULT)
    output logic [6:0]  o_rxlen,
    output logic        o_rxtoggle,
    // PORT_CTRL view (register tier; software-timed recipes)
    input  logic        i_run,         // PORT_CTRL.RUN: emit frame markers
    input  logic        i_power,
    input  logic        i_reset,       // drive bus reset while set
    input  logic        i_suspend,
    input  logic        i_resume,      // drive resume K while set
    // PORT_STATUS view
    output logic        o_connect,
    output logic        o_enabled,
    output logic        o_reset_active,
    output logic        o_suspended,
    output logic [1:0]  o_port_speed,  // usb_speed_e of the attached device
    output logic [1:0]  o_port_line,   // raw line state (PORT_STATUS.LINE)
    output logic        o_port_change, // one-cycle -> IRQ_STATUS.PORT_CHANGE
    // FRAME view
    output logic        o_sof_irq,     // one-cycle -> IRQ_STATUS.SOF
    output logic [10:0] o_frame,
    output logic [15:0] o_sof_tx_cnt,  // markers actually transmitted
    // Data-buffer ports (the register tier's dual-clock BRAM)
    output logic [6:0]  o_buf_raddr,   // transmit payload, synchronous read
    input  logic [7:0]  i_buf_rdata,
    output logic [6:0]  o_buf_waddr,   // receive store, gated by the grant
    output logic [7:0]  o_buf_wdata,
    output logic        o_buf_we,
    // MAC-PHY seam, transmit half
    output logic [7:0]  o_tx_data,
    output logic        o_tx_valid,
    input  logic        i_tx_ready,
    // MAC-PHY seam, receive half
    input  logic [7:0]  i_rx_data,
    input  logic        i_rx_valid,
    input  logic        i_rx_active,
    input  logic        i_rx_error,
    // MAC-PHY seam, status and transceiver control
    input  logic [1:0]  i_line_state,
    input  logic [2:0]  i_caps,        // PHY capability ceiling {hs, fs, ls}
    output logic [1:0]  o_xcvr_sel,
    output logic        o_term_sel,
    output logic [1:0]  o_opmode,
    output logic        o_port_power
);

    // ── Leaf-cell interconnect ───────────────────────────────────────

    // Transaction sequencer <-> packet engines
    logic        txn_busy;       // level: transaction in flight, token to result
    logic        txn_tx_start;
    logic [3:0]  txn_tx_pid;
    logic [10:0] txn_tx_field;
    logic [6:0]  txn_tx_len;
    logic        txn_tx_done;
    logic        txn_rx_accept;

    // Frame timer <-> arbiter and packet transmitter
    logic        frm_req;        // level: a marker is due and untransmitted
    logic        frm_grant;      // level: the marker holds the transmitter
    logic        frm_tx_start;
    logic [3:0]  frm_tx_pid;
    logic [10:0] frm_tx_field;
    logic        frm_tx_keepalive;
    logic        frm_tx_done;

    // Packet transmitter command (the arbiter's mux output) and results
    logic        ptx_start;
    logic [3:0]  ptx_pid;
    logic [10:0] ptx_field;
    logic [6:0]  ptx_len;
    logic        ptx_keepalive;
    logic        ptx_busy;       // level: a packet in flight (can outlive frm_req)
    logic        ptx_done;       // one-cycle: the packet's last byte consumed
    logic [7:0]  ptx_tx_data;
    logic        ptx_tx_valid;

    // Packet receiver results (shared: the sequencer classifies them)
    logic        prx_done;
    logic [3:0]  prx_pid;
    logic        prx_pid_ok;
    logic [6:0]  prx_len;
    logic        prx_crc_ok;
    logic        prx_err;
    logic        prx_ovf;
    logic        prx_buf_we;

    // Port controller sidebands
    logic [1:0]  prt_speed;
    logic        prt_tx_override;
    logic [7:0]  prt_tx_override_data;

    // ── Transmit arbiter ─────────────────────────────────────────────
    //
    // Two requesters share the packet transmitter. The transaction
    // sequencer launches on txn_start and then assumes ownership for the
    // whole transaction (its i_start contract: only while idle). The
    // frame timer transmits only under frm_grant, in a four-phase
    // handshake whose phases the arbiter must know:
    //
    //   frm_req rises    — a frame boundary made a marker due
    //   frm_grant rises  — this arbiter's decision
    //   frm_req falls    — one cycle after the marker's completion strobe
    //                      (frm_tx_done) retires it; or at any moment,
    //                      even mid-transmit, when software clears RUN
    //                      and the frame timer walks away
    //   frm_grant falls  — the arbiter follows frm_req down
    //
    // The done routing below is part of that loop: frm_tx_done reaches
    // the frame timer only while ownership is held, so a grant released
    // before the completion strobe arrives would misroute the strobe and
    // leave frm_req up forever. The RUN-clear case is why release keys
    // on frm_req and not on the transmitter going idle — the disowned
    // marker still drains in usbhc_pkt_tx after frm_req falls, which is
    // also why every launch must wait for ptx_busy to clear.
    //
    // marker_owns_q is the ownership level that routes the command mux
    // and the done strobe; start_pending_q defers a register-tier start
    // that could not launch on arrival. While the host itself drives the
    // bus (reset/resume recipes), nothing launches.
    logic marker_owns_q, marker_owns_d;     // marker holds the transmitter
    logic start_pending_q, start_pending_d; // a deferred transaction start
    logic txn_start;                        // one-cycle launch pulse

    // The host drives the bus during the software-timed recipes; the
    // packet transmitter must stay quiet under them.
    logic bus_driven;
    assign bus_driven = i_reset || i_resume;

    // A transaction start asking to launch this cycle: fresh from the
    // register tier or held over from a blocked arrival.
    logic start_req;
    assign start_req = i_start || start_pending_q;

    // The transmitter is free to take a new owner: nothing in flight or
    // launching on either side, no recipe driving the bus, no disowned
    // packet still draining.
    logic tx_free;
    assign tx_free = !txn_busy && !marker_owns_q && !bus_driven && !ptx_busy;

    // A start beats a due marker: transactions are latency-sensitive,
    // while a marker tolerates losing the tie by design (the frame timer
    // sends only the latest), so its delay stays bounded by the
    // transaction it waits out.
    assign txn_start = tx_free && start_req;

    // The grant rises when the free transmitter has no start to serve,
    // and follows frm_req down while held.
    assign marker_owns_d = (marker_owns_q || (tx_free && !start_req)) &&
                           frm_req;

    // The start ledger, orthogonal to who owns the transmitter: a launch
    // consumes the request, an arrival that could not launch is
    // remembered until it can.
    always_comb begin
        if (txn_start)
            start_pending_d = 1'b0;
        else if (i_start)
            start_pending_d = 1'b1;
        else
            start_pending_d = start_pending_q;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            marker_owns_q   <= 1'b0;
            start_pending_q <= 1'b0;
        end else begin
            marker_owns_q   <= marker_owns_d;
            start_pending_q <= start_pending_d;
        end
    end

    assign frm_grant = marker_owns_q;

    // The register tier's busy view covers the deferral window, so a
    // deferred start never looks idle to software.
    assign o_busy = txn_busy || start_pending_q;

    // ── Packet-transmitter command mux + done routing ────────────────
    // The owner's command port is wired through; the completion strobe
    // returns to the owner. A marker is always a bare SOF (no payload),
    // so the length leg is the sequencer's alone and the keep-alive leg
    // the frame timer's.
    assign ptx_start     = marker_owns_q ? frm_tx_start : txn_tx_start;
    assign ptx_pid       = marker_owns_q ? frm_tx_pid   : txn_tx_pid;
    assign ptx_field     = marker_owns_q ? frm_tx_field : txn_tx_field;
    assign ptx_len       = marker_owns_q ? 7'd0         : txn_tx_len;
    assign ptx_keepalive = marker_owns_q && frm_tx_keepalive;
    // The frm_req term closes a one-cycle race: RUN cleared mid-transmit
    // drops the frame timer's marker-in-flight state before ownership
    // releases, and the drain's completion strobe must not reach a frame
    // timer that no longer expects one.
    assign frm_tx_done   = ptx_done && marker_owns_q && frm_req;
    assign txn_tx_done   = ptx_done && !marker_owns_q;

    // Markers that really went out, for the SOF_TX debug register — a
    // device experiences marker starvation as a missing-SOF gap and
    // answers it with suspend, which the FRAME count cannot reveal.
    logic [15:0] sof_tx_cnt_q;
    always_ff @(posedge i_clk) begin
        if (i_rst)
            sof_tx_cnt_q <= 16'd0;
        else if (frm_tx_done)
            sof_tx_cnt_q <= sof_tx_cnt_q + 16'd1;
    end
    assign o_sof_tx_cnt = sof_tx_cnt_q;

    // ── Seam transmit mux ────────────────────────────────────────────
    // The resume recipe holds the transmit channel at 00h (raw opmode:
    // the PHY drives a continuous K); it preempts the packet path, which
    // the arbiter keeps quiet while the recipe runs.
    assign o_tx_data  = prt_tx_override ? prt_tx_override_data : ptx_tx_data;
    assign o_tx_valid = prt_tx_override ? 1'b1                 : ptx_tx_valid;

    // ── Receive store gate ───────────────────────────────────────────
    // Buffer writes land only under the sequencer's IN-response grant: a
    // protocol-violating DATAx answer to an OUT must not scribble the
    // transmit payload the buffer still holds.
    assign o_buf_we = prx_buf_we && txn_rx_accept;

    // ── Leaf cells ───────────────────────────────────────────────────

    usbhc_txn u_txn (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_start     (txn_start),
        .i_pid_sel   (i_pid_sel),
        .i_devaddr   (i_devaddr),
        .i_endpoint  (i_endpoint),
        .i_toggle    (i_toggle),
        .i_length    (i_length),
        .i_speed     (prt_speed),
        .o_busy      (txn_busy),
        .o_done      (o_done),
        .o_result    (o_result),
        .o_rxlen     (o_rxlen),
        .o_rxtoggle  (o_rxtoggle),
        .o_tx_start  (txn_tx_start),
        .o_tx_pid    (txn_tx_pid),
        .o_tx_field  (txn_tx_field),
        .o_tx_len    (txn_tx_len),
        .i_tx_done   (txn_tx_done),
        .i_rx_done   (prx_done),
        .i_rx_pid    (prx_pid),
        .i_rx_pid_ok (prx_pid_ok),
        .i_rx_len    (prx_len),
        .i_rx_crc_ok (prx_crc_ok),
        .i_rx_err    (prx_err),
        .i_rx_ovf    (prx_ovf),
        .i_rx_active (i_rx_active),
        .o_rx_accept (txn_rx_accept)
    );

    usbhc_frame #(
        .CLKS_PER_MS (CLKS_PER_MS)
    ) u_frame (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_run          (i_run),
        .i_speed        (prt_speed),
        .o_sof_irq      (o_sof_irq),
        .o_frame        (o_frame),
        .o_req          (frm_req),
        .i_grant        (frm_grant),
        .o_tx_start     (frm_tx_start),
        .o_tx_pid       (frm_tx_pid),
        .o_tx_field     (frm_tx_field),
        .o_tx_keepalive (frm_tx_keepalive),
        .i_tx_done      (frm_tx_done)
    );

    usbhc_port #(
        .DEBOUNCE_CLKS (DEBOUNCE_CLKS)
    ) u_port (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_power            (i_power),
        .i_reset            (i_reset),
        .i_suspend          (i_suspend),
        .i_resume           (i_resume),
        .o_connect          (o_connect),
        .o_enabled          (o_enabled),
        .o_reset_active     (o_reset_active),
        .o_suspended        (o_suspended),
        .o_speed            (prt_speed),
        .o_line             (o_port_line),
        .o_change_evt       (o_port_change),
        .i_line_state       (i_line_state),
        .i_caps             (i_caps),
        .o_xcvr_sel         (o_xcvr_sel),
        .o_term_sel         (o_term_sel),
        .o_opmode           (o_opmode),
        .o_port_power       (o_port_power),
        .o_tx_override      (prt_tx_override),
        .o_tx_override_data (prt_tx_override_data)
    );

    usbhc_pkt_tx u_pkt_tx (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_start     (ptx_start),
        .i_pid       (ptx_pid),
        .i_field     (ptx_field),
        .i_len       (ptx_len),
        .i_keepalive (ptx_keepalive),
        .o_busy      (ptx_busy),
        .o_done      (ptx_done),
        .o_pl_addr   (o_buf_raddr),
        .i_pl_data   (i_buf_rdata),
        .o_tx_data   (ptx_tx_data),
        .o_tx_valid  (ptx_tx_valid),
        .i_tx_ready  (i_tx_ready)
    );

    usbhc_pkt_rx #(
        .BUF_BYTES (BUF_BYTES)
    ) u_pkt_rx (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_rx_data   (i_rx_data),
        .i_rx_valid  (i_rx_valid),
        .i_rx_active (i_rx_active),
        .i_rx_error  (i_rx_error),
        .o_buf_addr  (o_buf_waddr),
        .o_buf_data  (o_buf_wdata),
        .o_buf_we    (prx_buf_we),
        .o_done      (prx_done),
        .o_pid       (prx_pid),
        .o_pid_ok    (prx_pid_ok),
        .o_len       (prx_len),
        .o_crc_ok    (prx_crc_ok),
        .o_err       (prx_err),
        .o_overflow  (prx_ovf)
    );

    assign o_port_speed = prt_speed;

    // Ownership routes commands exclusively: a command from the side that
    // does not hold the transmitter means the arbiter granted both, and a
    // start into a busy transmitter means a packet was split.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (txn_tx_start |-> !marker_owns_q))
        else $error("usbhc_mac: transaction command while the marker owns the transmitter");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (frm_tx_start |-> marker_owns_q))
        else $error("usbhc_mac: marker command without ownership");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (ptx_start |-> !ptx_busy))
        else $error("usbhc_mac: packet start into a busy transmitter");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (txn_start |-> !txn_busy && !marker_owns_q && !bus_driven && !ptx_busy))
        else $error("usbhc_mac: transaction launched into an occupied transmitter");
endmodule
