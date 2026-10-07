// Gate-level event simulation of the synthesized eth_cdc, with routing
// skew on every primitive input (ecp5_cell_delays.v).  Two incommensurate
// clocks sweep every phase relationship between the domains over a run,
// at the two periods the command line gives.
//
// This is the only test that can observe a clock-domain-crossing hazard
// whose failure mode is a combinational glitch: a zero-delay simulator
// evaluates a multi-bit encode atomically, so the intermediate value a
// synchronizer can physically capture never exists for it to sample.
// The netlist, the per-pin delays and an event simulator are all three
// required.
//
// Checks:
//  * Gray monitor: each capture into the count synchronizer's first
//    stage must be a value the device side's Gray register held around
//    that edge.  A combinational encode in front of the synchronizer
//    glitches while its inputs switch, and a capture during the glitch
//    is a value the register never held.
//  * End to end: every bundle the device side holds is one the bus side
//    offered, whole and never older than the last; the count read on the
//    bus side never runs ahead of the device's or back from its own;
//    pulses arrive one for one; and at rest both sides agree.
//
// Plusargs: +seed=N (stimulus and routing-delay draw), +cycles=N (bus
// clock cycles to run), +tbus=NS and +tdev=NS (the two clock periods),
// +zero (all delays 0 -- the control run; see ecp5_delay_cfg).
`timescale 1ns/1ps

module tb_eth_cdc_gate;
    // Matches the bundle width the DUT was elaborated at.  Synthesis bakes
    // the parameter into the netlist, so this cannot set the DUT's width:
    // it declares what the netlist was built with, and the build passes
    // the same value to both.  A mismatch fails at elaboration on the port
    // widths rather than running against a silently wrong model.
    //
    // The handshake under test is width-independent, so a wide run
    // re-tests it over the routing a wide bundle actually needs.
    parameter integer DOWN_W = 69;

    integer seed = 1, seed0 = 1;
    integer cycles = 20000;
    real    tbus = 26.667, tdev = 7.993;    // the bus and device clocks' periods, ns

    // The shortest periods the checks stand at.  Nothing here checks
    // setup, so a bus clock too fast for the delays the cell models draw
    // reads as a broken crossing: the bundle's comparison, the longest
    // path there is, starts to miss at about 6 ns, and the floor is twice
    // that.  The device side's paths are a cell long, and its bound is
    // the Gray monitor's window.
    localparam real MIN_TBUS = 12.0;
    localparam real WIN      = 5.0;

    // ── Clocks: incommensurate, so the phase between domains sweeps ──
    reg bus_clk = 0, dev_clk = 0;
    reg clocks_on = 0;          // the periods have been read
    always begin wait (clocks_on); #(tbus/2) bus_clk = ~bus_clk; end
    always begin wait (clocks_on); #(tdev/2) dev_clk = ~dev_clk; end

    // ── DUT ──────────────────────────────────────────────────────
    reg               bus_rst = 1;
    reg  [DOWN_W-1:0] down = 0;
    wire [15:0]       up_count;
    wire              up_pulse;
    wire              dev_rst;
    wire [DOWN_W-1:0] down_dev;
    reg  [15:0]       dev_count = 0;
    reg               dev_pulse = 0;

    eth_cdc dut (
        .i_clk(bus_clk), .i_rst(bus_rst), .i_down(down),
        .o_up_count(up_count), .o_up_pulse(up_pulse),
        .i_dev_clk(dev_clk), .o_dev_rst(dev_rst), .o_down(down_dev),
        .i_up_count(dev_count), .i_up_pulse(dev_pulse)
    );

    // A bundle carries a sequence number in its low 16 bits, and the rest
    // follows from it in alternating inverted and plain copies, so one
    // made of two bundles' bits shows.  No bundle is all zeroes, which is
    // what the device side holds before the first.
    function [DOWN_W-1:0] bundle(input [15:0] n);
        integer i;
        for (i = 0; i < DOWN_W; i = i + 1)
            bundle[i] = n[i % 16] ^ ((i / 16) & 1);
    endfunction

    integer errors = 0;
    integer running = 0;        // the checks are on
    integer moving = 0;         // the stimulus is still changing things

    // ── Bus side: offer bundles, watch the count and the pulses ──
    reg  [15:0] down_n = 1;             // the number of the bundle on offer
    integer     down_gap = 0;           // bus cycles until the next is offered
    integer     dev_total = 0;          // counted on the device side, unwrapped
    integer     shown = 0;              // the count read on the bus side, unwrapped
    integer     bus_pulses = 0, dev_pulses = 0;
    reg  [15:0] step;                   // how far the count read has moved

    always @(posedge bus_clk) if (running) begin
        // Outputs are registered: sampled at the edge.
        step = up_count - shown[15:0];
        if (step >= 16'h8000) begin
            errors = errors + 1;
            if (errors <= 10) $display("%t ERROR: count read %0d after %0d", $time, up_count, shown[15:0]);
        end else begin
            shown = shown + step;
            if (shown > dev_total) begin
                errors = errors + 1;
                if (errors <= 10) $display("%t ERROR: count read %0d, the device's is %0d", $time, shown, dev_total);
            end
        end
        if (up_pulse) bus_pulses = bus_pulses + 1;
        if (bus_pulses > dev_pulses) begin
            errors = errors + 1;
            if (errors <= 10) $display("%t ERROR: %0d pulses up for %0d sent", $time, bus_pulses, dev_pulses);
        end
        #0.5;
        // Gaps from none to longer than a bundle takes to cross, so that a
        // new one is offered both with the last in flight and at rest.
        if (moving) begin
            if (down_gap == 0) begin
                down_n = down_n + 1;
                down_gap = ($random(seed) & 32'h7fffffff) % 24;
            end else
                down_gap = down_gap - 1;
        end
        down <= bundle(down_n);
    end

    // ── Device side: take bundles, count and pulse ───────────────
    // Like a receiver with its enable in the bundle: nothing moves until
    // one has come down.
    reg  [15:0] taken_n = 0;            // the number of the bundle last taken
    reg         taken_any = 0;
    integer     bundles = 0;            // distinct bundles taken
    integer     since_pulse = 0;        // device cycles since the last pulse
    // A pulse goes out no sooner than this many device cycles after the
    // one before.
    integer     pulse_spacing = 0;
    reg  [15:0] newer;                  // how far a bundle's number is past the last taken

    always @(posedge dev_clk) if (running) begin
        if (down_dev !== {DOWN_W{1'b0}}) begin
            if (down_dev !== bundle(down_dev[15:0])) begin
                errors = errors + 1;
                if (errors <= 10) $display("%t ERROR: bundle %h is not one that was offered", $time, down_dev);
            end else begin
                newer = down_dev[15:0] - taken_n;
                if (taken_any && newer >= 16'h8000) begin
                    errors = errors + 1;
                    if (errors <= 10) $display("%t ERROR: bundle %0d taken after %0d", $time, down_dev[15:0], taken_n);
                end
                if (!taken_any || newer != 0) bundles = bundles + 1;
                taken_n = down_dev[15:0];
                taken_any = 1;
            end
        end
        #0.5;
        dev_pulse <= 0;
        since_pulse = since_pulse + 1;
        if (moving && taken_any) begin
            if ($random(seed) & 1) begin
                dev_total = dev_total + 1;
                dev_count <= dev_total[15:0];
            end
            if (since_pulse >= pulse_spacing && ($random(seed) & 1)) begin
                dev_pulse <= 1;
                dev_pulses = dev_pulses + 1;
                since_pulse = 0;
            end
        end
    end

    // ── Gray monitor on the count synchronizer's first stage ─────
    // A capture must be a value the device side's Gray register held
    // around the capture edge: any value whose holding interval overlaps
    // the WIN ns before the edge (longer than any path delay; shorter
    // than the register's fastest step).  The register can step more than
    // once between two captures, so "previous capture + 1" is not the
    // rule.
    //
    // The register takes, at each device clock edge, the count presented
    // before it.  Its history is kept here from that count and not read
    // out of the netlist, so the monitor holds whatever drives the
    // synchronizer to what that register would have held.
    localparam integer HIST = 4;
    function [15:0] g2b(input [15:0] g);
        integer i;
        begin
            g2b = g;
            for (i = 1; i < 16; i = i * 2) g2b = g2b ^ (g2b >> i);
        end
    endfunction

    integer viol = 0, caps = 0;
    reg  [15:0] h_v [0:HIST-1];         // [0] = current value
    real        h_t [0:HIST-1];         // when each value started
    integer k;
    initial for (k = 0; k < HIST; k = k + 1) begin h_v[k] = 0; h_t[k] = -1.0e9; end

    always @(posedge dev_clk) begin : snap
        if (dev_count !== h_v[0]) begin
            for (k = HIST - 1; k > 0; k = k - 1) begin h_v[k] = h_v[k-1]; h_t[k] = h_t[k-1]; end
            h_v[0] = dev_count; h_t[0] = $realtime;
        end
    end

    // Was `v` held by the register at any time in [t_edge - WIN, t_edge]?
    function held(input [15:0] v, input real t_edge);
        integer i; real stop;
        begin
            held = 0;
            for (i = 0; i < HIST; i = i + 1) begin
                stop = (i == 0) ? 1.0e12 : h_t[i-1];
                if (h_v[i] === v && h_t[i] <= t_edge && stop > t_edge - WIN) held = 1;
            end
        end
    endfunction

    always @(posedge bus_clk) if (running) begin : mon
        real t_edge; reg [15:0] got;
        t_edge = $realtime;
        #0.9 got = g2b(dut.up_gray_s1_q);
        #0.1 if (!held(got, t_edge)) begin
            viol = viol + 1;
            if (viol <= 5) $display("%t count sync captured %0d; register history %0d %0d %0d", $time,
                                    got, h_v[0], h_v[1], h_v[2]);
        end
        caps = caps + 1;
    end

    // ── Verdict ──────────────────────────────────────────────────
    // Turns the run's tallies into a pass/fail exit status.  Called once,
    // after the run has stopped; must $fatal on failure so the enclosing
    // make target sees a nonzero exit.
    task check_results;
        real dev_cycles;
        begin
            // At rest: the latest bundle is held, and the two sides agree
            // on the count and the pulses.
            if (down_dev !== down)
                $fatal(1, "at rest: bundle %h held, %h offered", down_dev, down);
            if (up_count !== dev_total[15:0])
                $fatal(1, "at rest: count read %0d, the device's is %0d", up_count, dev_total[15:0]);
            if (bus_pulses != dev_pulses)
                $fatal(1, "at rest: %0d pulses up for %0d sent", bus_pulses, dev_pulses);
            // The floors only have to separate a working run from a wedged
            // one, so they sit well under the rates the stimulus runs at: a
            // bundle crosses and its toggle returns in about three cycles
            // of each clock, the count steps on every other device cycle,
            // and a pulse follows the one before by its spacing and a
            // cycle or two.
            dev_cycles = cycles * tbus / tdev;
            if (bundles < cycles * tbus / (40.0 * (tbus + tdev)))
                $fatal(1, "too few bundles crossed: %0d in %0d bus cycles", bundles, cycles);
            if (dev_total < dev_cycles / 8.0)
                $fatal(1, "the count stepped too little: %0d in %0d bus cycles", dev_total, cycles);
            if (dev_pulses < dev_cycles / (8.0 * (pulse_spacing + 2)))
                $fatal(1, "too few pulses sent: %0d in %0d bus cycles", dev_pulses, cycles);
            if (errors != 0 || viol != 0)
                $fatal(1, "%0d errors, count sync %0d bad", errors, viol);
        end
    endtask

    // ── Run ──────────────────────────────────────────────────────
    integer settle;             // bus cycles for a change to cross and come back, with room
    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        seed0 = seed;
        if ($value$plusargs("cycles=%d", cycles)) ;
        if ($value$plusargs("tbus=%f", tbus)) ;
        if ($value$plusargs("tdev=%f", tdev)) ;
        if (tbus < MIN_TBUS)
            $fatal(1, "a bus clock period of %.3f ns is shorter than the %.1f ns the netlist's paths are given", tbus, MIN_TBUS);
        if (tdev <= WIN)
            $fatal(1, "a device clock period of %.3f ns is no longer than the Gray monitor's %.1f ns window", tdev, WIN);

        // The crossing asks for pulses further apart than three bus
        // cycles, and a run sends the closest that allows: a crossing that
        // needed more room than it is given would pass at any wider
        // spacing.
        pulse_spacing = $rtoi(3.0 * tbus / tdev) + 1;
        if (pulse_spacing * tdev <= 3.0 * tbus)
            $fatal(1, "pulses %0d device cycles apart are not further apart than three bus cycles", pulse_spacing);

        settle = 20 + $rtoi(20.0 * tdev / tbus);
        clocks_on = 1;

        // The machine's reset: far longer than the crossing takes.
        repeat (10 * settle) @(posedge bus_clk);
        @(negedge bus_clk) bus_rst = 0;
        repeat (2 * settle) @(posedge bus_clk);
        if (dev_rst !== 1'b0)
            $fatal(1, "the device side is still in reset %0d bus cycles after the machine's ended", 2 * settle);

        running = 1;
        moving = 1;
        repeat (cycles) @(posedge bus_clk);
        moving = 0;
        repeat (4 * settle) @(posedge bus_clk);
        running = 0;
        $display("down=%0d bus %.3f ns dev %.3f ns seed %0d%s: %0d bundles, count %0d, %0d pulses, %0d errors | count sync: %0d bad of %0d captures",
                 DOWN_W, tbus, tdev, seed0, $test$plusargs("zero") ? " (zero delay)" : "",
                 bundles, dev_total, dev_pulses, errors, viol, caps);
        check_results;
        $finish;
    end
endmodule
