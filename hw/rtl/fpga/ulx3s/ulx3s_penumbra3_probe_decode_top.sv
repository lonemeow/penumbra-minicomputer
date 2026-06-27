// ULX3S board top (Penumbra/3 probe P0.2) -- decode->issue boundary.
//
// A synthesis instrument, not a usable machine. It measures the ID read
// cone: a registered pre-decoded bundle -> regmap -> scoreboard read ->
// can_issue. The bundle fields are driven from flops (src*_areg_q etc.),
// which IS the decode->issue claim: ID's cone starts from registered
// fields, not the raw instruction word. The heavy word->bundle decode runs
// on the enqueue side and is modeled here as "the bundle is already
// registered"; its own (fetch-slack) timing is a Phase-1 check when the real
// parallel-decode lands.
//
// vs P0.1: this adds the regmap on the front of the proven issue cone. The
// cone ends at can_issue_q (a clean register), so the LED keep-alive XOR is
// off it. LFSR stimulus, buttons perturb, 25 MHz crystal direct.
module ulx3s_penumbra3_probe_decode_top (
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

    // ── Stimulus: 32-bit maximal LFSR ────────────────────────────
    logic [31:0] lfsr_q;
    always_ff @(posedge clk_25mhz) begin
        if (rst) lfsr_q <= 32'h1;
        else     lfsr_q <= {lfsr_q[30:0],
                            lfsr_q[31] ^ lfsr_q[21] ^ lfsr_q[1] ^ lfsr_q[0] ^ btn[2]};
    end

    // ── Registered pre-decoded bundle (the FIFO slot ID reads) ───
    // The cone under test starts here, at flops.
    logic [3:0] src0_areg_q, src1_areg_q;
    logic       src0_used_q, src1_used_q, supervisor_q, valid_q;
    always_ff @(posedge clk_25mhz) begin
        src0_areg_q  <= lfsr_q[3:0];
        src1_areg_q  <= lfsr_q[7:4];
        src0_used_q  <= lfsr_q[8];
        src1_used_q  <= lfsr_q[9];
        supervisor_q <= lfsr_q[10];
        valid_q      <= lfsr_q[11];
    end

    // ── regmap per source (arch field -> scoreboard index) ───────
    logic [IDX_BITS-1:0] src0_pidx, src1_pidx;
    logic                src0_tracked, src1_tracked;
    penumbra3_regmap #(.IDX_BITS (IDX_BITS)) u_rm0 (
        .i_areg       (src0_areg_q),
        .i_supervisor (supervisor_q),
        .o_pidx       (src0_pidx),
        .o_tracked    (src0_tracked)
    );
    penumbra3_regmap #(.IDX_BITS (IDX_BITS)) u_rm1 (
        .i_areg       (src1_areg_q),
        .i_supervisor (supervisor_q),
        .o_pidx       (src1_pidx),
        .o_tracked    (src1_tracked)
    );

    // ── scoreboard / load-completion / issue (reused from P0.1) ──
    logic                src0_pending, src1_pending;
    logic                load_pending, launch_fill, complete, complete_fault;
    logic [IDX_BITS-1:0] complete_dest;
    logic [31:0]         complete_data;
    logic                can_issue;

    penumbra3_scoreboard #(.NREGS (NREGS)) u_sb (
        .i_clk          (clk_25mhz),
        .i_rst          (rst),
        .i_set_en       (can_issue & lfsr_q[2]),
        .i_set_idx      (lfsr_q[16 +: IDX_BITS]),
        .i_clr_en       (complete),
        .i_clr_idx      (complete_dest),
        .i_src0_idx     (src0_pidx),
        .i_src1_idx     (src1_pidx),
        .o_src0_pending (src0_pending),
        .o_src1_pending (src1_pending)
    );

    penumbra3_load_complete #(.IDX_BITS (IDX_BITS)) u_lc (
        .i_clk            (clk_25mhz),
        .i_rst            (rst),
        .i_load_valid     (lfsr_q[15]),
        .i_cache_hit      (lfsr_q[12]),
        .i_load_dest      (lfsr_q[21 +: IDX_BITS]),
        .i_fill_done      (lfsr_q[27] & load_pending),
        .i_fill_data      (lfsr_q),
        .i_fill_fault     (lfsr_q[28]),
        .o_load_pending   (load_pending),
        .o_launch_fill    (launch_fill),
        .o_complete       (complete),
        .o_complete_dest  (complete_dest),
        .o_complete_data  (complete_data),
        .o_complete_fault (complete_fault)
    );

    penumbra3_issue u_issue (
        .i_valid        (valid_q),
        .i_pipe_hold    (load_pending),
        .i_src0_used    (src0_used_q & src0_tracked),
        .i_src0_pending (src0_pending),
        .i_src0_fwd     (lfsr_q[13]),
        .i_src1_used    (src1_used_q & src1_tracked),
        .i_src1_pending (src1_pending),
        .i_src1_fwd     (lfsr_q[14]),
        .o_can_issue    (can_issue)
    );

    // ── Cone endpoint: the ID/EX enable flop ─────────────────────
    logic can_issue_q;
    always_ff @(posedge clk_25mhz) can_issue_q <= can_issue;

    // ── Keep-alive: fold registered outputs onto the LEDs ────────
    logic [7:0] led_q;
    always_ff @(posedge clk_25mhz) begin
        led_q <= {7'b0,
                  can_issue_q
                  ^ load_pending ^ launch_fill ^ complete ^ complete_fault
                  ^ (^complete_dest) ^ (^complete_data)};
    end
    assign led = led_q;

endmodule
