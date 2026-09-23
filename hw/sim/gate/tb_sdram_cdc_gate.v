// Gate-level event simulation of the synthesized sdram_cdc, with
// routing skew on every primitive input (ecp5_cell_delays.v).  Two
// incommensurate clocks sweep every phase relationship between the
// domains over a run.
//
// This is the only test that can observe a clock-domain-crossing
// hazard whose failure mode is a combinational glitch: a zero-delay
// simulator evaluates a multi-bit encode atomically, so the
// intermediate value a synchronizer can physically capture never
// exists for it to sample.  The netlist, the per-pin delays and an
// event simulator are all three required.
//
// Checks:
//  * Gray monitor: each capture into a pointer synchronizer's first
//    stage must be a value the source pointer actually held around
//    that edge.  A combinational encode in front of the synchronizer
//    glitches while its inputs switch, and a capture during the
//    glitch is a value the pointer never held.
//  * End to end: requests reach the controller side in order with
//    their payload intact, and every completion returns in order with
//    the right read data.
//
// Plusargs: +seed=N (stimulus and routing-delay draw), +cycles=N
// (system-clock cycles to run), +zero (all delays 0 — the control
// run; see ecp5_delay_cfg).
`timescale 1ns/1ps

module tb_sdram_cdc_gate;
    // Matches the width the DUT was elaborated at.  Synthesis bakes the
    // parameter into the netlist, so this cannot set the DUT's width --
    // it declares what the netlist was built with, and the build passes
    // the same value to both.  A mismatch fails at elaboration on the
    // port widths rather than running against a silently wrong model.
    //
    // The pointer handshake under test is width-independent, so a wide
    // run re-tests the same hazard over the routing a wide payload
    // actually needs.
    parameter integer RSP_BITS = 32;
    localparam integer RSP_WORDS = RSP_BITS / 32;

    integer seed = 1, seed0 = 1;
    integer cycles = 200000;

    // ── Clocks: incommensurate, so the phase between domains sweeps ──
    localparam real TSYS = 40.0;
    localparam real TSD  = 9.9731;
    reg sys_clk = 0, sd_clk = 0;
    always #(TSYS/2) sys_clk = ~sys_clk;
    always #(TSD/2)  sd_clk  = ~sd_clk;
    reg sys_rst = 1, sd_rst = 1;

    // ── DUT ─────────────────────────────────────────────────────
    reg         sys_req_valid = 0, sys_req_we = 0;
    reg  [31:0] sys_req_addr = 0, sys_req_wdata = 0;
    reg  [3:0]  sys_req_byte_en = 0;
    wire        sys_req_ready, sys_rsp_valid, sys_done;
    wire [RSP_BITS-1:0] sys_rsp_data;
    wire        sd_req_valid, sd_req_we, sd_rsp_ready;
    wire [31:0] sd_req_addr, sd_req_wdata;
    wire [3:0]  sd_req_byte_en;
    reg         sd_req_ready = 0, sd_rsp_valid = 0, sd_done = 0;
    reg  [RSP_BITS-1:0] sd_rsp_data = 0;

    sdram_cdc dut (
        .i_sys_clk(sys_clk), .i_sys_rst(sys_rst),
        .i_sys_req_valid(sys_req_valid), .i_sys_req_we(sys_req_we),
        .i_sys_req_addr(sys_req_addr), .i_sys_req_wdata(sys_req_wdata),
        .i_sys_req_byte_en(sys_req_byte_en), .o_sys_req_ready(sys_req_ready),
        .o_sys_rsp_valid(sys_rsp_valid), .o_sys_rsp_data(sys_rsp_data),
        .i_sys_rsp_ready(1'b1), .o_sys_done(sys_done),
        .i_sd_clk(sd_clk), .i_sd_rst(sd_rst),
        .o_sd_req_valid(sd_req_valid), .o_sd_req_we(sd_req_we),
        .o_sd_req_addr(sd_req_addr), .o_sd_req_wdata(sd_req_wdata),
        .o_sd_req_byte_en(sd_req_byte_en), .i_sd_req_ready(sd_req_ready),
        .i_sd_rsp_valid(sd_rsp_valid), .i_sd_rsp_data(sd_rsp_data),
        .o_sd_rsp_ready(sd_rsp_ready), .i_sd_done(sd_done)
    );

    function [RSP_BITS-1:0] rd_data;   // what the controller model returns for a read
        input [31:0] a;
        integer w;
        for (w = 0; w < RSP_WORDS; w = w + 1)
            rd_data[w*32 +: 32] = (a + w) ^ 32'h5A5A_A5A5;
    endfunction

    // ── Ledger ──────────────────────────────────────────────────
    reg     is_write [0:1023];
    integer next_push = 0;      // sequence number of the next request to issue
    integer next_done = 0;      // next completion the system side expects
    integer next_serve = 0;     // next request the controller side expects
    integer errors = 0;
    integer running = 0;

    // ── System side: issue requests, retire completions ─────────
    always @(posedge sys_clk) if (running) begin
        // Completions (registered pulses, sampled at the edge).
        if (sys_done) begin
            if (is_write[next_done % 1024]) begin
                if (sys_rsp_valid) begin
                    errors = errors + 1;
                    if (errors <= 10) $display("%t ERROR: write #%0d completed with rsp_valid", $time, next_done);
                end
            end else if (!sys_rsp_valid || sys_rsp_data !== rd_data(next_done)) begin
                errors = errors + 1;
                if (errors <= 10) $display("%t ERROR: read #%0d rsp_valid=%b data=%h want %h", $time,
                                           next_done, sys_rsp_valid, sys_rsp_data, rd_data(next_done));
            end
            next_done = next_done + 1;
        end else if (sys_rsp_valid) begin
            errors = errors + 1;
            if (errors <= 10) $display("%t ERROR: rsp_valid without done", $time);
        end
        // Acceptance happens on this edge iff valid && ready just before it.
        if (sys_req_valid && sys_req_ready) next_push = next_push + 1;
        #1;
        if ($random(seed) % 4 != 0) begin
            sys_req_valid   <= 1;
            is_write[next_push % 1024] = $random(seed) & 1;
            sys_req_we      <= is_write[next_push % 1024];
            sys_req_addr    <= next_push;
            sys_req_wdata   <= ~next_push;
            sys_req_byte_en <= next_push[3:0];
        end else
            sys_req_valid   <= 0;
    end

    // ── Controller side: accept one request, complete it later ──
    integer wait_cnt = 0;
    reg     busy = 0, cur_write = 0;
    reg [31:0] cur_addr;
    always @(posedge sd_clk) if (running) begin
        #0.5;
        sd_done <= 0; sd_rsp_valid <= 0; sd_req_ready <= 0;
        if (!busy && sd_req_valid && !sd_req_ready) begin
            if (sd_req_addr !== next_serve || sd_req_wdata !== ~next_serve ||
                sd_req_byte_en !== next_serve[3:0] || sd_req_we !== is_write[next_serve % 1024]) begin
                errors = errors + 1;
                if (errors <= 10) $display("%t ERROR: controller got addr=%h we=%b, want #%0d we=%b", $time,
                                           sd_req_addr, sd_req_we, next_serve, is_write[next_serve % 1024]);
            end
            cur_addr = sd_req_addr; cur_write = sd_req_we;
            next_serve = next_serve + 1;
            sd_req_ready <= 1;
            busy = 1;
            wait_cnt = 1 + (($random(seed) & 32'h7fffffff) % 6);
        end else if (busy) begin
            if (wait_cnt == 0) begin
                sd_done <= 1;
                if (!cur_write) begin sd_rsp_valid <= 1; sd_rsp_data <= rd_data(cur_addr); end
                busy = 0;
            end else
                wait_cnt = wait_cnt - 1;
        end
    end

    // ── Gray monitors on each synchronizer's first stage ────────
    // A capture must be a value the source pointer actually held around
    // the capture edge: any value whose holding interval overlaps the
    // WIN ns before the edge (longer than any path delay; shorter than
    // the source's fastest step).  The source can legitimately step more
    // than once between two captures, so "previous capture + 1" is not
    // the rule.
    //
    // The source history is sampled after each source-clock edge
    // settles, not on net-change events: the pointer's bits come from
    // separate flops that switch at the same instant, and a change
    // watcher can see the zero-width in-between value.
    localparam real WIN = 5.0;
    function [1:0] g2b; input [1:0] g; g2b = {g[1], g[1] ^ g[0]}; endfunction
    integer viol_w = 0, viol_r = 0, caps_w = 0, caps_r = 0;
    reg  [1:0] w_v [0:3], r_v [0:3];            // [0] = current value
    real       w_t [0:3], r_t [0:3];            // when each value started
    integer k;
    initial for (k = 0; k < 4; k = k + 1) begin
        w_v[k] = 0; r_v[k] = 0; w_t[k] = -1.0e9; r_t[k] = -1.0e9;
    end
    always @(posedge sys_clk) begin : snap_w
        real te; te = $realtime;
        #0.5 if (dut.sys_wptr_bin !== w_v[0]) begin
            for (k = 3; k > 0; k = k - 1) begin w_v[k] = w_v[k-1]; w_t[k] = w_t[k-1]; end
            w_v[0] = dut.sys_wptr_bin; w_t[0] = te;
        end
    end
    always @(posedge sd_clk) begin : snap_r
        real te; te = $realtime;
        #0.5 if (dut.sd_rptr_bin !== r_v[0]) begin
            for (k = 3; k > 0; k = k - 1) begin r_v[k] = r_v[k-1]; r_t[k] = r_t[k-1]; end
            r_v[0] = dut.sd_rptr_bin; r_t[0] = te;
        end
    end
    // Was `v` held by the source at any time in [t_edge - WIN, t_edge]?
    function held_w(input [1:0] v, input real t_edge);
        integer i; real stop;
        held_w = 0;
        for (i = 0; i < 4; i = i + 1) begin
            stop = (i == 0) ? 1.0e12 : w_t[i-1];
            if (w_v[i] === v && w_t[i] <= t_edge && stop > t_edge - WIN) held_w = 1;
        end
    endfunction
    function held_r(input [1:0] v, input real t_edge);
        integer i; real stop;
        held_r = 0;
        for (i = 0; i < 4; i = i + 1) begin
            stop = (i == 0) ? 1.0e12 : r_t[i-1];
            if (r_v[i] === v && r_t[i] <= t_edge && stop > t_edge - WIN) held_r = 1;
        end
    endfunction

    always @(posedge sd_clk) if (running) begin : mon_w
        real t_edge; reg [1:0] got;
        t_edge = $realtime;
        #0.9 got = g2b(dut.sys_wptr_gray_s1);
        #0.1 if (!held_w(got, t_edge)) begin     // after snap_w has run for this window
            viol_w = viol_w + 1;
            if (viol_w <= 5) $display("%t wptr sync captured %0d; source history %0d %0d %0d", $time, got, w_v[0], w_v[1], w_v[2]);
        end
        caps_w = caps_w + 1;
    end
    always @(posedge sys_clk) if (running) begin : mon_r
        real t_edge; reg [1:0] got;
        t_edge = $realtime;
        #0.9 got = g2b(dut.sd_rptr_gray_s1);
        #0.1 if (!held_r(got, t_edge)) begin
            viol_r = viol_r + 1;
            if (viol_r <= 5) $display("%t rptr sync captured %0d; source history %0d %0d %0d", $time, got, r_v[0], r_v[1], r_v[2]);
        end
        caps_r = caps_r + 1;
    end

    // ── Verdict ─────────────────────────────────────────────────
    // Turns the run's tallies into a pass/fail exit status.  Called
    // once, after the run has stopped; must $fatal on failure so the
    // enclosing make target sees a nonzero exit.
    task check_results;
        begin
            // Completions run about one per three system cycles: the
            // stimulus offers a request on three cycles in four, and the
            // depth-2 FIFO plus the controller model's service time bound
            // the round trip.  The floor only has to separate a working
            // run from a wedged one, so it sits well under that rate.
            if (next_done < cycles / 4)
                $fatal(1, "too few completed transactions: %0d in %0d cycles",
                    next_done, cycles);
            if (errors != 0 || viol_w != 0 || viol_r != 0)
                $fatal(1, "%0d errors, wptr %0d bad, rptr %0d bad",
                        errors, viol_w, viol_r);
        end
    endtask

    // ── Run ─────────────────────────────────────────────────────
    initial begin
        if ($value$plusargs("seed=%d", seed)) ;
        seed0 = seed;
        if ($value$plusargs("cycles=%d", cycles)) ;
        repeat (4) @(posedge sys_clk);
        @(negedge sys_clk) sys_rst = 0;
        @(negedge sd_clk)  sd_rst = 0;
        repeat (4) @(posedge sys_clk);
        running = 1;
        repeat (cycles) @(posedge sys_clk);
        running = 0;
        $display("rsp=%0d seed %0d%s: %0d requests, %0d completed, %0d errors | wptr sync: %0d bad of %0d captures | rptr sync: %0d bad of %0d captures",
                 RSP_BITS, seed0, $test$plusargs("zero") ? " (zero delay)" : "",
                 next_push, next_done, errors, viol_w, caps_w, viol_r, caps_r);
        check_results;
        $finish;
    end
endmodule
