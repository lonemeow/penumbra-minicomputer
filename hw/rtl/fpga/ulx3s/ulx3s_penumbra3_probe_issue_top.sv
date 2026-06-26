// ULX3S board top (Penumbra/3 probe P0.1) -- issue gate / load-completion.
//
// A synthesis instrument, not a usable machine. It puts the gen3 back-end
// stall in front of nextpnr: the scoreboard, the registered load-completion
// FSM, and the issue gate. Two things are under test:
//
//  1. Timing -- the issue cone (scoreboard read -> can_issue -> the ID/EX
//     enable flop). The cone ends at can_issue_q, a clean register, so the
//     LED keep-alive XOR is off it (the P0.3 lesson).
//  2. Structure -- the cache-hit verdict reaches the back-end ONLY through
//     load_complete's registered o_load_pending. It feeds issue's pipe-hold
//     as a flop, so it can never appear on the can_issue combinational cone.
//     The timing report's critical path must start at scoreboard/load_pending
//     flops, not at the cache_hit input.
//
// An LFSR drives the stimulus every cycle (buttons perturb it) and the
// registered outputs fold onto the LEDs so synthesis keeps the design.
// Clocking is the 25 MHz crystal directly, one clean domain.
module ulx3s_penumbra3_probe_issue_top (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [6:0] btn,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic       wifi_en
);
    localparam int NREGS    = 22;
    localparam int IDX_BITS = $clog2(NREGS);

    assign wifi_en = 1'b0;

    // ── Reset: power-on counter + btn[1] ─────────────────────────
    logic btn1_sync1_q, btn1_sync2_q;
    always_ff @(posedge clk_25mhz) begin
        btn1_sync1_q <= btn[1];
        btn1_sync2_q <= btn1_sync1_q;
    end
    /* verilator lint_off PROCASSINIT */
    logic [18:0] rst_cnt_q = '0;
    /* verilator lint_on PROCASSINIT */
    logic rst;
    always_ff @(posedge clk_25mhz) begin
        if (btn1_sync2_q)        rst_cnt_q <= '0;
        else if (!rst_cnt_q[18]) rst_cnt_q <= rst_cnt_q + 1;
    end
    assign rst = !rst_cnt_q[18];

    // ── Stimulus: 32-bit maximal LFSR, perturbed by the buttons ──
    logic [31:0] lfsr_q;
    always_ff @(posedge clk_25mhz) begin
        if (rst) lfsr_q <= 32'h1;
        else     lfsr_q <= {lfsr_q[30:0],
                            lfsr_q[31] ^ lfsr_q[21] ^ lfsr_q[1] ^ lfsr_q[0] ^ btn[2]};
    end

    // ── Back-end DUT signals ─────────────────────────────────────
    logic                src0_pending, src1_pending;
    logic                load_pending, launch_fill, complete, complete_fault;
    logic [IDX_BITS-1:0] complete_dest;
    logic [31:0]         complete_data;
    logic                can_issue;

    // Scoreboard: set on an issuing pending-class op, clear on completion.
    penumbra3_scoreboard #(
        .NREGS (NREGS)
    ) u_sb (
        .i_clk          (clk_25mhz),
        .i_rst          (rst),
        .i_set_en       (can_issue & lfsr_q[2]),
        .i_set_idx      (lfsr_q[IDX_BITS-1:0]),
        .i_clr_en       (complete),
        .i_clr_idx      (complete_dest),
        .i_src0_idx     (lfsr_q[4 +: IDX_BITS]),
        .i_src1_idx     (lfsr_q[10 +: IDX_BITS]),
        .o_src0_pending (src0_pending),
        .o_src1_pending (src1_pending)
    );

    // Load-completion: the cache-hit verdict enters here and leaves only as
    // the registered load_pending -- the structural break P0.1 checks.
    penumbra3_load_complete #(
        .IDX_BITS (IDX_BITS)
    ) u_lc (
        .i_clk            (clk_25mhz),
        .i_rst            (rst),
        .i_load_valid     (lfsr_q[15]),
        .i_cache_hit      (lfsr_q[16]),
        .i_load_dest      (lfsr_q[21 +: IDX_BITS]),
        .i_fill_done      (lfsr_q[27] & load_pending),   // done only while pending
        .i_fill_data      (lfsr_q),
        .i_fill_fault     (lfsr_q[28]),
        .o_load_pending   (load_pending),
        .o_launch_fill    (launch_fill),
        .o_complete       (complete),
        .o_complete_dest  (complete_dest),
        .o_complete_data  (complete_data),
        .o_complete_fault (complete_fault)
    );

    // Issue gate (the cone under test).
    penumbra3_issue u_issue (
        .i_valid        (lfsr_q[29]),
        .i_pipe_hold    (load_pending),
        .i_src0_used    (lfsr_q[30]),
        .i_src0_pending (src0_pending),
        .i_src0_fwd     (lfsr_q[31]),
        .i_src1_used    (lfsr_q[14]),
        .i_src1_pending (src1_pending),
        .i_src1_fwd     (lfsr_q[3]),
        .o_can_issue    (can_issue)
    );

    // ── Cone endpoint: the ID/EX enable flop ─────────────────────
    logic can_issue_q;
    always_ff @(posedge clk_25mhz) can_issue_q <= can_issue;

    // ── Keep-alive: fold the registered outputs onto the LEDs ────
    logic [7:0] led_q;
    always_ff @(posedge clk_25mhz) begin
        led_q <= {7'b0,
                  can_issue_q
                  ^ load_pending ^ launch_fill ^ complete ^ complete_fault
                  ^ (^complete_dest) ^ (^complete_data)};
    end
    assign led = led_q;

endmodule
