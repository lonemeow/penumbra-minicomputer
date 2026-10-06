// Penumbra Ethernet adapter clock-domain crossing
//
// Joins the bus clock to one of the adapter's PHY-side clocks, the receive
// clock or the transmit clock; the adapter holds one of these for each.
// Four things cross, each in the way its changes allow.
//
//   Down, bus to device: what software has written for that side (a frame
//   count, the enables, the station address), as one bundle. A count
//   software writes can jump, so the bundle is held still on the bus side
//   while a toggle crosses to say it is new, and the device side takes it
//   whole. The next bundle waits for the toggle to come back, so nothing
//   here depends on how the two clocks compare.
//
//   Up, device to bus: the device's own frame count, which only ever steps
//   by one. It crosses as a Gray code, held in a register of the device's
//   domain, so that only flip-flops drive a synchroniser.
//
//   Up, device to bus: a pulse, as a toggle. Pulses have to come further
//   apart than three bus cycles, which frames lost to a full ring do.
//
//   Down: the machine's reset, as a request that stays up until the
//   device side has answered it. That side's clock may be stopped while
//   the machine resets (a PHY held in reset stops it), and a request that
//   waits is taken whenever the clock returns.
//
// From a reset until the device side has answered it and come out of its
// own, the bus side reads the count as zero, sends nothing down and passes
// no pulse: what that side holds until then is from before the reset.
//
// Every signal that crosses leaves from a flip-flop and arrives at one. A
// board's constraints have to bound the delay between the two, to less
// than a period of the faster clock.

module eth_cdc #(
    parameter int DOWN_W = 32       // bits in the bundle sent down
)(
    // ── Bus side ─────────────────────────────────────────────
    input  logic              i_clk,
    input  logic              i_rst,
    input  logic [DOWN_W-1:0] i_down,       // what the device side should hold
    output logic [15:0]       o_up_count,   // the device's count
    output logic              o_up_pulse,   // one cycle for each of the device's

    // ── Device side ──────────────────────────────────────────
    input  logic              i_dev_clk,
    output logic              o_dev_rst,    // that domain's reset
    output logic [DOWN_W-1:0] o_down,       // the bundle last taken; zero from reset
    input  logic [15:0]       i_up_count,   // steps by at most one a cycle
    input  logic              i_up_pulse    // one cycle
);

    // The Gray code of a count: successive counts differ in one bit of it,
    // the step from all-ones back to zero among them.
    function automatic logic [15:0] bin2gray(input logic [15:0] bin);
        bin2gray = bin ^ (bin >> 1);
    endfunction

    // And back: each bit of the count is the parity of the code's bits from
    // there up.
    function automatic logic [15:0] gray2bin(input logic [15:0] gray);
        logic [15:0] bin;
        bin = gray;
        for (int shift = 1; shift < 16; shift = shift << 1)
            bin = bin ^ (bin >> shift);
        gray2bin = bin;
    endfunction

    // ═════════════════════════════════════════════════════════
    //  Reset
    // ═════════════════════════════════════════════════════════
    // rst_req_q crosses to the device side, where its third flop is that
    // domain's reset. That flop crosses back as rst_busy: high for as long
    // as the device side is in reset, from when it takes a request up until
    // it has seen the request go.
    //
    // The request rises with the machine's reset and falls once that is
    // over and the device side is in its own. The machine's reset lasts
    // 10 ms at least (doc/hardware/bus-protocol.md, "Reset Timing"), far
    // longer than the request takes to cross and rst_busy to come back. So
    // with the device side's clock running, rst_busy as the reset ends is
    // this request's doing and not left from the one before. With that
    // clock stopped the request stays up, and is taken when the clock
    // returns.
    logic rst_req_q;        // the device side is asked to reset

    (* ASYNC_REG = "TRUE" *) logic rst_busy_s1_q, rst_busy_s2_q;
    logic rst_busy;         // the device side is in reset
    assign rst_busy = rst_busy_s2_q;

    (* ASYNC_REG = "TRUE" *) logic dev_req_s1_q, dev_req_s2_q;
    logic dev_rst_q;

    always_ff @(posedge i_dev_clk) begin
        {dev_rst_q, dev_req_s2_q, dev_req_s1_q} <= {dev_req_s2_q, dev_req_s1_q, rst_req_q};
    end

    assign o_dev_rst = dev_rst_q;

    always_ff @(posedge i_clk) begin
        {rst_busy_s2_q, rst_busy_s1_q} <= {rst_busy_s1_q, dev_rst_q};

        if (i_rst)
            rst_req_q <= 1'b1;
        else if (rst_busy)
            rst_req_q <= 1'b0;
    end

    // The device side has taken the reset and is out of its own.
    logic settled;
    assign settled = !i_rst && !rst_req_q && !rst_busy;

    // ═════════════════════════════════════════════════════════
    //  Down: the bundle
    // ═════════════════════════════════════════════════════════
    logic [DOWN_W-1:0] sent_q;      // the bundle last sent, held still to be taken
    logic              send_tog_q;  // flips with each bundle sent

    (* ASYNC_REG = "TRUE" *) logic took_s1_q, took_s2_q;
    logic send_idle;                // the last bundle sent has been taken
    assign send_idle = (send_tog_q == took_s2_q);

    (* ASYNC_REG = "TRUE" *) logic dev_tog_s1_q, dev_tog_s2_q;
    logic              dev_took_q;  // the toggle as of the last bundle taken
    logic [DOWN_W-1:0] dev_down_q;

    always_ff @(posedge i_clk) begin
        {took_s2_q, took_s1_q} <= {took_s1_q, dev_took_q};

        if (i_rst) begin
            sent_q     <= '0;
            send_tog_q <= 1'b0;
        end else if (settled && send_idle && (i_down != sent_q)) begin
            sent_q     <= i_down;
            send_tog_q <= !send_tog_q;
        end
    end

    // sent_q has stood still since before the toggle left, two cycles of
    // this clock ago at the least.
    always_ff @(posedge i_dev_clk) begin
        {dev_tog_s2_q, dev_tog_s1_q} <= {dev_tog_s1_q, send_tog_q};

        if (dev_rst_q) begin
            dev_took_q <= 1'b0;
            dev_down_q <= '0;
        end else if (dev_tog_s2_q != dev_took_q) begin
            dev_took_q <= dev_tog_s2_q;
            dev_down_q <= sent_q;
        end
    end

    assign o_down = dev_down_q;

    // ═════════════════════════════════════════════════════════
    //  Up: the count and the pulse
    // ═════════════════════════════════════════════════════════
    logic [15:0] dev_gray_q;        // the count's Gray code, all a synchroniser sees of it
    logic        dev_pulse_tog_q;   // flips with each pulse

    always_ff @(posedge i_dev_clk) begin
        if (dev_rst_q) begin
            dev_gray_q      <= '0;
            dev_pulse_tog_q <= 1'b0;
        end else begin
            dev_gray_q      <= bin2gray(i_up_count);
            dev_pulse_tog_q <= dev_pulse_tog_q ^ i_up_pulse;
        end
    end

    (* ASYNC_REG = "TRUE" *) logic [15:0] up_gray_s1_q, up_gray_s2_q;
    (* ASYNC_REG = "TRUE" *) logic        pulse_s1_q, pulse_s2_q;
    logic        pulse_s3_q;        // the toggle a cycle ago, to see it move
    logic [15:0] up_count_q;
    logic        up_pulse_q;

    always_ff @(posedge i_clk) begin
        {up_gray_s2_q, up_gray_s1_q}         <= {up_gray_s1_q, dev_gray_q};
        {pulse_s3_q, pulse_s2_q, pulse_s1_q} <= {pulse_s2_q, pulse_s1_q, dev_pulse_tog_q};

        up_count_q <= settled ? gray2bin(up_gray_s2_q) : '0;
        up_pulse_q <= settled && (pulse_s2_q != pulse_s3_q);
    end

    assign o_up_count = up_count_q;
    assign o_up_pulse = up_pulse_q;

endmodule
