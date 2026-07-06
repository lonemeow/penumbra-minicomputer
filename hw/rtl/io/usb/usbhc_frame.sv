// Penumbra USB host frame timer (MAC)
//
// Keeps the bus alive while PORT_CTRL.RUN is set: every USB frame (1 ms) it
// advances the FRAME counter, raises the SOF interrupt, and requests one
// frame marker from the packet transmitter — an SOF token carrying the
// frame number at full speed, the bare keep-alive at low speed (the PHY
// turns the lone SOF PID into a bare EOP per the seam contract).
//
// The marker must never split a transaction, so this module does not own
// the transmitter: it raises o_req and transmits only under the MAC
// arbiter's i_grant, which the arbiter withholds while a transaction is in
// flight and holds until o_req drops. A marker delayed past its own frame
// boundary is stale — the request policy sends only the latest marker, and
// the FRAME counter tracks elapsed frames, not transmitted ones.

module usbhc_frame #(
    // USB-domain clocks per USB frame: 1 ms of the seam's 60 MHz clock.
    // Overridable so testbenches can run short frames.
    parameter int unsigned CLKS_PER_MS = 60_000
) (
    input  logic        i_clk,
    input  logic        i_rst,
    input  logic        i_run,          // PORT_CTRL.RUN: emit frame markers
    input  logic [1:0]  i_speed,        // usb_speed_e: SOF at FS, keep-alive at LS
    output logic        o_sof_irq,      // one-cycle event strobe per frame boundary; the
                                        // register tier latches it into the sticky W1C
                                        // IRQ_STATUS.SOF, whose masked OR is the CPU-level IRQ
    output logic [10:0] o_frame,        // FRAME register value
    // Marker request toward the MAC's transmit arbiter
    output logic        o_req,          // a marker is due and untransmitted
    input  logic        i_grant,        // the packet transmitter is ours; held until o_req drops
    // Packet-transmitter command (sampled on o_tx_start)
    output logic        o_tx_start,
    output logic [3:0]  o_tx_pid,
    output logic [10:0] o_tx_field,
    output logic        o_tx_keepalive,
    input  logic        i_tx_done
);
    import usb_pkg::*;

    // The millisecond divider; held cleared while stopped so the first
    // frame after RUN rises is a full one.
    localparam int unsigned MS_CNT_W = $clog2(CLKS_PER_MS);
    logic [MS_CNT_W-1:0] ms_cnt_q, ms_cnt_d;
    logic ms_tick;
    assign ms_tick = i_run && (ms_cnt_q == MS_CNT_W'(CLKS_PER_MS - 1));

    logic [10:0] frame_cnt_q, frame_cnt_d;
    logic        pending_q, pending_d;   // a marker is due
    logic        sending_q, sending_d;   // the transmitter is running our marker
    logic        tx_go_q, tx_go_d;
    logic        sof_irq_q, sof_irq_d;

    assign o_frame        = frame_cnt_q;
    assign o_req          = pending_q;
    assign o_sof_irq      = sof_irq_q;
    assign o_tx_start     = tx_go_q;
    assign o_tx_pid       = USB_PID_SOF;
    assign o_tx_field     = frame_cnt_q;
    assign o_tx_keepalive = (i_speed == USB_SPEED_LS);

    always_comb begin
        frame_cnt_d   = frame_cnt_q;
        pending_d = pending_q;
        sending_d = sending_q;
        tx_go_d   = 1'b0;
        sof_irq_d = 1'b0;

        if (!i_run || ms_tick)
            ms_cnt_d = '0;
        else
            ms_cnt_d = ms_cnt_q + 1'b1;

        if (!i_run) begin
            // Stopped: nothing is due, and a marker mid-transmit simply
            // finishes on its own in the packet transmitter.
            pending_d = 1'b0;
            sending_d = 1'b0;
        end else begin
            if (ms_tick) begin
                // The boundary acts unconditionally: the counter tracks
                // elapsed frames whether or not the previous marker ever
                // won the transmitter, and re-marking an already-due
                // marker is exactly the send-only-the-latest policy, since
                // o_tx_field reads the live counter at transmit time.
                sof_irq_d   = 1'b1;
                frame_cnt_d = frame_cnt_q + 11'd1;
                pending_d   = 1'b1;
            end

            if (pending_q && i_grant && !sending_q) begin
                tx_go_d   = 1'b1;
                sending_d = 1'b1;
            end
            if (sending_q && i_tx_done) begin
                sending_d = 1'b0;
                pending_d = 1'b0;
            end
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ms_cnt_q     <= '0;
            frame_cnt_q   <= 11'd0;
            pending_q <= 1'b0;
            sending_q <= 1'b0;
            tx_go_q   <= 1'b0;
            sof_irq_q <= 1'b0;
        end else begin
            ms_cnt_q     <= ms_cnt_d;
            frame_cnt_q   <= frame_cnt_d;
            pending_q <= pending_d;
            sending_q <= sending_d;
            tx_go_q   <= tx_go_d;
            sof_irq_q <= sof_irq_d;
        end
    end

    // The divider never runs past the frame period, and a transmit
    // completion can only arrive while our marker is in flight (the
    // arbiter routes the done strobe).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (ms_cnt_q < MS_CNT_W'(CLKS_PER_MS)))
        else $error("usbhc_frame: divider exceeded the frame period");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_tx_done |-> sending_q))
        else $error("usbhc_frame: transmit done without a marker in flight");
endmodule
