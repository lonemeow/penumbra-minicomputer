// CLASS_AUDIO device — the minimum protocol, one channel.
//
// Register interface and semantics are specified in
// doc/system/devices/audio.md. This implements the mandatory core: one
// channel accepting 16-bit signed stereo frames at the device's native
// rate, with a watermark interrupt and underrun reporting. Pitch,
// volume, narrower sample formats and additional channels are the
// optional features the spec gates behind capability bits, and their
// registers read the unity values that make a consumer ignoring the
// bits behave correctly.
//
// The device emits PCM frames rather than driving pins: what converts
// them is the board's business, whether that is a resistor ladder or a
// codec behind a serial link.
//
// The queue lives entirely in the register domain, and only a frame at
// a time crosses to the output domain. LEVEL and UNDERRUN are therefore
// exact counts rather than values inferred across a boundary.

// keep_hierarchy: hold this boundary through synth_ecp5. The device's
// busy feeds the bus stall combine, and letting the placer scatter its
// logic across the die stretched the route out of that combine into the
// arbiter — on a core with no timing margin to spare.
(* keep_hierarchy = "yes" *)
module audio
    import penumbra_pkg::*;
#(
    parameter int DEPTH     = 1024,        // channel FIFO, in frames
    parameter int SAMPLE_HZ = 48_000,
    parameter int AUDIO_HZ  = 60_000_000
)(
    // ── Register domain ──────────────────────────────────────
    input  logic        i_clk,
    input  logic        i_rst,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_addr,   /* bits [8:2] select a register */
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] i_wdata,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_busy,
    output logic        o_irq,

    // ── Output domain ────────────────────────────────────────
    input  logic               i_aclk,
    input  logic               i_arst,
    output logic signed [15:0] o_frame_l,
    output logic signed [15:0] o_frame_r,
    output logic               o_frame_en   // one i_aclk pulse per frame
);

    localparam int ADDR_W  = $clog2(DEPTH);
    localparam int TICK_DIV = AUDIO_HZ / SAMPLE_HZ;

    // Version 1, one channel, no optional features.
    localparam logic [31:0] CAP_VALUE = 32'h0001_0001;
    localparam logic [31:0] UNITY_VOL = 32'hFFFF_FFFF;
    localparam logic [31:0] UNITY_INC = 32'h0001_0000;

    logic       sel_chan;
    logic [2:0] reg_sel;
    assign sel_chan = i_addr[8];
    assign reg_sel  = i_addr[4:2];

    // ── Register state ───────────────────────────────────────
    logic        enable_q;      // CTRL.ENABLE
    logic        chan_en_q;     // CHAN_EN bit 0
    logic        irq_status_q;  // watermark reached, sticky
    logic        irq_enable_q;
    logic        underrun_q;    // sticky
    logic [31:0] wm_q;

    // ── Channel FIFO ─────────────────────────────────────────
    logic [31:0]     fifo [0:DEPTH-1];
    logic [ADDR_W-1:0] wr_q, rd_q;
    logic [ADDR_W:0]   count_q;      // frames held, 0..DEPTH

    // Free space needs only as many bits as DEPTH. Carrying it at 32
    // put a full-width subtract and a full-width compare against WM on
    // the CPU clock every cycle, for a queue that never exceeds 2^ADDR_W.
    localparam int LVL_W = ADDR_W + 1;

    logic [LVL_W-1:0] level;         // free space, in frames
    assign level = LVL_W'(DEPTH) - LVL_W'(count_q);

    // A watermark past the queue's capacity can never be reached. Saying
    // so with a reduction over the high bits keeps WM readable at its
    // written width without widening the comparison.
    logic wm_unreachable;
    assign wm_unreachable = |wm_q[31:LVL_W];

    // ── Output domain: rate tick ─────────────────────────────
    logic run_a1, run_a2;            // enable, into the output domain
    logic [$clog2(TICK_DIV)-1:0] tick_q;
    logic tick;

    assign tick = (tick_q == ($clog2(TICK_DIV))'(TICK_DIV - 1));

    always_ff @(posedge i_aclk) begin
        if (i_arst) begin
            {run_a1, run_a2} <= 2'b00;
            run_a3           <= 1'b0;
            tick_q           <= '0;
        end else begin
            {run_a2, run_a1} <= {run_a1, enable_q & chan_en_q};
            run_a3           <= run_a2;
            tick_q           <= tick ? '0 : tick_q + 1'b1;
        end
    end

    // A disabled device consumes nothing and emits silence; the spec
    // requires that its FIFO not drain while CTRL.ENABLE is clear.
    // Starting the channel primes the prefetch, so the first output
    // frame is the first queued one and not the empty value it held.
    logic take, prime, run_a3;
    assign take  = tick & run_a2;
    assign prime = run_a2 & ~run_a3;

    logic [31:0] frame;
    logic        pop;      // CDC asks for a frame

    audio_frame_cdc #(
        .W (32)
    ) u_cdc (
        .i_clk  (i_clk),
        .i_rst  (i_rst),
        .o_req  (pop),
        .i_data (fifo[rd_q]),
        .i_aclk (i_aclk),
        .i_arst (i_arst),
        .i_take  (take),
        .i_prime (prime),
        .o_data  (frame)
    );

    // The handoff's output already changes on i_take and holds until the
    // next one, so the frame follows it rather than being registered
    // again — a second register here would present the previous frame,
    // both updating on the same edge. o_frame_en marks the cycle after
    // the change, by which point the value has settled.
    //
    // Earlier frames occupy lower bit lanes, so a consumer copying
    // little-endian sample data needs no reordering.
    assign o_frame_l = run_a2 ? signed'(frame[15:0])  : 16'sd0;
    assign o_frame_r = run_a2 ? signed'(frame[31:16]) : 16'sd0;

    always_ff @(posedge i_aclk) begin
        if (i_arst) o_frame_en <= 1'b0;
        else        o_frame_en <= take;
    end

    // ── Register domain: queue and status ────────────────────
    logic push;                      // consumer writes one
    logic flush;
    logic drain;                     // a frame actually left the queue

    assign push  = i_we && sel_chan && (reg_sel == 3'd6) && (count_q != (ADDR_W+1)'(DEPTH));
    assign flush = i_we && sel_chan && (reg_sel == 3'd0) && i_wdata[8];
    assign drain = pop && (count_q != '0);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            wr_q         <= '0;
            rd_q         <= '0;
            count_q      <= '0;
            enable_q     <= 1'b0;
            chan_en_q    <= 1'b0;
            irq_status_q <= 1'b0;
            irq_enable_q <= 1'b0;
            underrun_q   <= 1'b0;
            wm_q         <= '0;
        end else begin
            // FLUSH takes effect between two output frames: the output
            // stage holds a prefetched frame, so emptying the queue here
            // never truncates one already being emitted.
            if (flush) begin
                wr_q    <= '0;
                rd_q    <= '0;
                count_q <= '0;
            end else begin
                if (push) begin
                    fifo[wr_q] <= i_wdata;
                    wr_q       <= wr_q + 1'b1;
                end
                if (drain) rd_q <= rd_q + 1'b1;

                case ({push, drain})
                    2'b10:   count_q <= count_q + 1'b1;
                    2'b01:   count_q <= count_q - 1'b1;
                    default: ;
                endcase
            end

            // An enabled channel asked for a frame and had none.
            if (pop && (count_q == '0)) underrun_q <= 1'b1;

            // Sticky while the free space has reached the watermark. A
            // zero watermark disables the flag rather than asserting it
            // permanently.
            if ((wm_q != 32'd0) && !wm_unreachable &&
                (level >= wm_q[LVL_W-1:0])) irq_status_q <= 1'b1;

            if (i_we && !sel_chan) begin
                case (reg_sel)
                    3'd2: enable_q     <= i_wdata[0];
                    3'd3: chan_en_q    <= i_wdata[0];
                    3'd4: if (i_wdata[0]) irq_status_q <= 1'b0;
                    3'd5: irq_enable_q <= i_wdata[0];
                    3'd6: if (i_wdata[0]) underrun_q   <= 1'b0;
                    default: ;
                endcase
            end
            if (i_we && sel_chan && (reg_sel == 3'd5)) wm_q <= i_wdata;
        end
    end

    assign o_irq = irq_status_q & irq_enable_q;

    // ── Register reads ───────────────────────────────────────
    // Absent optional features read their unity values, so a consumer
    // that never inspects CAP still plays at the native rate and full
    // volume rather than at a wrong pitch or in silence.
    logic [31:0] rdata_next;

    always_comb begin
        if (!sel_chan) begin
            case (reg_sel)
                3'd0:    rdata_next = CAP_VALUE;
                3'd1:    rdata_next = 32'(SAMPLE_HZ);
                3'd2:    rdata_next = {31'd0, enable_q};
                3'd3:    rdata_next = {31'd0, chan_en_q};
                3'd4:    rdata_next = {31'd0, irq_status_q};
                3'd5:    rdata_next = {31'd0, irq_enable_q};
                3'd6:    rdata_next = {31'd0, underrun_q};
                default: rdata_next = UNITY_VOL;      // MASTER_VOL
            endcase
        end else begin
            case (reg_sel)
                3'd0:    rdata_next = 32'd0;          // CHAN CTRL
                3'd1:    rdata_next = UNITY_INC;
                3'd2:    rdata_next = UNITY_VOL;
                3'd3:    rdata_next = 32'(DEPTH);
                3'd4:    rdata_next = 32'(level);
                3'd5:    rdata_next = wm_q;
                default: rdata_next = 32'd0;          // FIFO reads 0
            endcase
        end
    end

    // A read answers from a register one cycle after it is presented,
    // and holds the bus for that cycle. The device-select feeding the
    // top-level read combine is registered to match, so combinational
    // data would be sampled a cycle after it had already gone. Writes
    // are not stalled: filling the FIFO is the one hot path here.
    logic read_pending_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_rdata        <= 32'd0;
            read_pending_q <= 1'b0;
        end else begin
            o_rdata        <= i_re ? rdata_next : 32'd0;
            read_pending_q <= i_re & ~read_pending_q;
        end
    end

    assign o_busy = i_re & ~read_pending_q;

endmodule
