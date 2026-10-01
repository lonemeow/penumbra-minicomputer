// CLASS_AUDIO device.
//
// Register interface and semantics are specified in
// doc/system/devices/audio.md. CHANNELS channels, each accepting 16-bit
// signed stereo frames, with per-channel rate and gain, a watermark
// interrupt and underrun reporting.
//
// The device emits PCM frames rather than driving pins: what converts
// them is the board's business, whether that is a resistor ladder or a
// codec behind a serial link.
//
// The device does not pace itself: the output stage requests each frame
// (i_frame_req), because what sets the sample rate belongs to the
// converter — a divider off a fixed clock for a modulator, the frame
// clock a codec drives for a serial link. RATE reports SAMPLE_HZ, so
// the requests have to arrive at that rate.
//
// The output stage also reports whether it is ready (i_out_busy,
// i_out_fault, and a code naming what failed), which the device
// presents as STATUS: some stages have to be brought up before they
// convert anything, and that can fail.
// While either is set the device consumes no frames, whatever the stage
// requests. A codec left running by an earlier configuration keeps
// clocking frames while its setup rewrites it, and a stream consumed
// then would be lost to a converter nobody has configured.
//
// Two clock domains meet here. Registers and queue writes are on i_clk;
// the frame requests, the mixer and the queue reads are on i_aclk. Only
// pointers and a few slow-changing control values cross, so the mixer
// never waits on the register side and the register side never waits on
// a frame.
//
// Rate and gain values cross without a handshake. They are written at
// tick rate at most and read once per frame, so a write landing on the
// sampling edge costs at most one frame at a stale value — a single
// sample of phase error or gain, against a 1-in-a-frame-period chance
// of happening at all. A handshake per value per channel would cost far
// more than it buys.
//
// One channel's pair crosses at a time, selected by the mixer. The
// datapath is shared, so it reads one channel's rate and gain per
// frame and never two at once; synchronising every channel's pair
// continuously would be four times the registers to serve the same one
// read. The mixer names the channel it wants far enough ahead for both
// synchroniser stages, which is what makes the select safe to mux in
// front of them.

module audio
    import penumbra_pkg::*;
#(
    parameter int CHANNELS  = 1,
    parameter int DEPTH     = 1024,        // per channel, in frames
    parameter int SAMPLE_HZ = 48_000
)(
    // ── Register domain ──────────────────────────────────────
    input  logic        i_clk,
    input  logic        i_rst,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_addr,   /* bits [10:2] select a register */
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] i_wdata,
    input  logic        i_we,
    input  logic        i_re,
    output logic [31:0] o_rdata,
    output logic        o_ack,
    output logic        o_irq,
    // The output stage's readiness, in this domain: busy while it is
    // being brought up, fault once that has failed, with a code in the
    // stage's own terms naming what failed. A stage with nothing to
    // bring up ties all three low.
    input  logic        i_out_busy,
    input  logic        i_out_fault,
    input  logic [7:0]  i_out_fault_code,

    // ── Output domain ────────────────────────────────────────
    input  logic               i_aclk,
    input  logic               i_arst,
    input  logic               i_frame_req, // one i_aclk pulse per frame wanted
    output logic signed [15:0] o_frame_l,
    output logic signed [15:0] o_frame_r,
    output logic               o_frame_en   // one i_aclk pulse per frame
);

    localparam int CH_W    = (CHANNELS > 1) ? $clog2(CHANNELS) : 1;
    localparam int LVL_W   = $clog2(DEPTH) + 1;

    // Version 1; rate, gain and readiness reporting implemented.
    localparam logic [31:0] CAP_VALUE =
        32'd1 | 32'h0000_0100 | 32'h0000_0200 | 32'h0000_2000 |
        (32'(CHANNELS) << 16);

    localparam logic [31:0] UNITY_VOL = 32'hFFFF_FFFF;
    localparam logic [31:0] UNITY_INC = 32'h0001_0000;

    // ── Address decode ───────────────────────────────────────
    // Global registers sit in the first 32-byte block; channel n is
    // block 8 + n, which is what 0x100 + n * 0x20 means. Every other
    // block below 0x100 mirrors the global registers, which the class
    // allows: it gates each later register behind a CAP bit rather than
    // relying on an offset reading zero.
    //
    // STATUS (0x020) shares CAP's slot and is told apart by address
    // bit 5 alone, so it mirrors into every odd block. Decoding its block
    // in full puts a mux in front of every bit a read returns; the one
    // bit gates only the few bits where CAP is set.
    logic [5:0]      blk;
    logic [2:0]      reg_sel;
    logic            sel_chan;
    logic [CH_W-1:0] chan_idx;

    assign blk      = i_addr[10:5];
    assign reg_sel  = i_addr[4:2];
    assign sel_chan = (blk >= 6'd8) && (blk < 6'(8 + CHANNELS));
    assign chan_idx = CH_W'(blk - 6'd8);

    // ── Bus handshake: a simple device ───────────────────────
    // Every access resolves in its first cycle (bus_read / bus_write)
    // and is acknowledged the next. Every side effect — a queue push
    // above all — keys on these, never on the held strobes.
    logic bus_read, bus_write;

    bus_device_simple u_bus (
        .i_clk   (i_clk),
        .i_rst   (i_rst),
        .i_re    (i_re),
        .i_we    (i_we),
        .o_read  (bus_read),
        .o_write (bus_write),
        .o_ack   (o_ack)
    );

    // ── Register state ───────────────────────────────────────
    logic                enable_q;
    logic [CHANNELS-1:0] chan_en_q;
    logic [CHANNELS-1:0] irq_status_q;
    logic [CHANNELS-1:0] irq_enable_q;
    logic [CHANNELS-1:0] underrun_q;
    (* ram_style = "registers" *) logic [31:0] wm_q  [CHANNELS];
    (* ram_style = "registers" *) logic [31:0] inc_q [CHANNELS];
    (* ram_style = "registers" *) logic [31:0] vol_q [CHANNELS];
    logic [31:0]         master_vol_q;

    logic [LVL_W-1:0]    level    [CHANNELS];
    logic [CHANNELS-1:0] push;
    logic [CHANNELS-1:0] flush;

    // ── Output-stage status ──────────────────────────────────
    // FAULT is the device's to hold rather than the stage's: the class
    // keeps it set until reset whatever the stage does next, and keeps
    // the code it arrived with, the first failure's. BUSY reads clear
    // once FAULT is set, since a bring-up that failed has ended.
    logic        fault_q;
    logic [7:0]  fault_code_q;
    logic [15:0] status;      // STATUS: [15:8] FAULT_CODE, [1] FAULT, [0] BUSY
    logic        down_q;      // the stage is not ready

    assign status = {fault_code_q, 6'd0, fault_q, i_out_busy & ~fault_q};

    // Readiness crosses on a synchroniser of its own rather than folded
    // into the enable's. A stage with nothing to bring up ties its inputs
    // low, so down_q is constant and it and its synchroniser vanish,
    // leaving that board the enable path it has without STATUS. down_q
    // is registered before it crosses, so the synchroniser samples one
    // flop rather than a gate whose inputs can change in the same cycle.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            fault_q      <= 1'b0;
            fault_code_q <= 8'd0;
            down_q       <= 1'b0;
        end else begin
            if (i_out_fault && !fault_q) begin
                fault_q      <= 1'b1;
                fault_code_q <= i_out_fault_code;
            end
            down_q <= i_out_busy | i_out_fault | fault_q;
        end
    end

    // ── Output domain: frame requests, enable and readiness ──
    logic run_a1, run_a2;
    logic down_a1, down_a2;
    logic frame;

    assign frame = i_frame_req & run_a2 & ~down_a2;

    always_ff @(posedge i_aclk) begin
        if (i_arst) begin
            {run_a2, run_a1}   <= 2'b00;
            {down_a2, down_a1} <= 2'b00;
        end else begin
            {run_a2, run_a1}   <= {run_a1, enable_q};
            {down_a2, down_a1} <= {down_a1, down_q};
        end
    end

    // ── Control values into the output domain ────────────────
    logic [CH_W-1:0]     mix_ctrl_ch;   // channel the mixer wants next
    logic [CHANNELS-1:0] chan_en_a1, chan_en_a2;
    logic [31:0]         inc_a1, inc_a2;
    logic [31:0]         vol_a1, vol_a2;
    logic [31:0]         mvol_a1, mvol_a2;

    always_ff @(posedge i_aclk) begin
        chan_en_a1 <= chan_en_q;
        chan_en_a2 <= chan_en_a1;
        mvol_a1    <= master_vol_q;
        mvol_a2    <= mvol_a1;
        inc_a1     <= inc_q[mix_ctrl_ch];
        inc_a2     <= inc_a1;
        vol_a1     <= vol_q[mix_ctrl_ch];
        vol_a2     <= vol_a1;
    end

    // ── Queues ───────────────────────────────────────────────
    logic [CH_W-1:0]     mix_ch;
    logic                mix_advance;
    logic signed [15:0]  q_left  [CHANNELS];
    logic signed [15:0]  q_right [CHANNELS];
    logic [CHANNELS-1:0] q_empty;
    logic [CHANNELS-1:0] q_flushed;

    for (genvar c = 0; c < CHANNELS; c++) begin : g_chan
        audio_chan_fifo #(
            .DEPTH    (DEPTH),
            .SAMPLE_W (16)
        ) u_fifo (
            .i_clk    (i_clk),
            .i_rst    (i_rst),
            .i_push   (push[c]),
            .i_wdata  (i_wdata),
            .i_flush  (flush[c]),
            .o_level  (level[c]),
            .i_aclk   (i_aclk),
            .i_arst   (i_arst),
            .i_pop    (mix_advance && (mix_ch == CH_W'(c))),
            .o_left   (q_left[c]),
            .o_right  (q_right[c]),
            .o_empty  (q_empty[c]),
            .o_flushed(q_flushed[c])
        );
    end

    // ── Mixer ────────────────────────────────────────────────
    logic [CHANNELS-1:0] underrun_a;

    audio_mixer #(
        .CHANNELS (CHANNELS),
        .SAMPLE_W (16)
    ) u_mix (
        .i_clk        (i_aclk),
        .i_rst        (i_arst),
        .i_frame      (frame),
        .i_flush      (q_flushed),
        .i_chan_en    (chan_en_a2),
        .i_inc        (inc_a2),
        .i_vol        (vol_a2),
        .i_master_vol (mvol_a2),
        .o_ctrl_ch    (mix_ctrl_ch),
        .o_ch         (mix_ch),
        .o_advance    (mix_advance),
        .i_left       (q_left[mix_ch]),
        .i_right      (q_right[mix_ch]),
        .i_empty      (q_empty[mix_ch]),
        .o_underrun   (underrun_a),
        .o_left       (o_frame_l),
        .o_right      (o_frame_r),
        .o_valid      (o_frame_en)
    );

    // Underrun is held for a frame on the output side, which is far
    // wider than this clock's period, so two stages are enough.
    // Edge, not level: the output side holds the signal for the whole
    // frame, so a consumer clearing the flag inside that frame would
    // watch it set again immediately.
    logic [CHANNELS-1:0] underrun_r1, underrun_r2, underrun_r3;
    logic [CHANNELS-1:0] underrun_edge;

    always_ff @(posedge i_clk) begin
        underrun_r1 <= underrun_a;
        underrun_r2 <= underrun_r1;
        underrun_r3 <= underrun_r2;
    end

    assign underrun_edge = underrun_r2 & ~underrun_r3;

    // ── Register writes ──────────────────────────────────────
    always_comb begin
        push  = '0;
        flush = '0;
        if (sel_chan && bus_write) begin
            if (reg_sel == 3'd6) push[chan_idx]  = 1'b1;   // FIFO
            if (reg_sel == 3'd0 && i_wdata[8])
                flush[chan_idx] = 1'b1;                    // CTRL.FLUSH
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            enable_q     <= 1'b0;
            chan_en_q    <= '0;
            irq_status_q <= '0;
            irq_enable_q <= '0;
            underrun_q   <= '0;
            master_vol_q <= UNITY_VOL;
            for (int c = 0; c < CHANNELS; c++) begin
                wm_q[c]  <= '0;
                inc_q[c] <= UNITY_INC;
                vol_q[c] <= UNITY_VOL;
            end
        end else begin
            for (int c = 0; c < CHANNELS; c++) begin
                // Sticky while free space has reached the watermark. A
                // zero watermark disables the flag rather than raising
                // it permanently.
                if (wm_q[c] != 32'd0 && |wm_q[c][31:LVL_W] == 1'b0 &&
                    level[c] >= wm_q[c][LVL_W-1:0])
                    irq_status_q[c] <= 1'b1;

                if (underrun_edge[c])
                    underrun_q[c] <= 1'b1;
            end

            if (bus_write && !sel_chan) begin
                case (reg_sel)
                    3'd2: enable_q     <= i_wdata[0];
                    3'd3: chan_en_q    <= i_wdata[CHANNELS-1:0];
                    3'd4: irq_status_q <= irq_status_q &
                                          ~i_wdata[CHANNELS-1:0];
                    3'd5: irq_enable_q <= i_wdata[CHANNELS-1:0];
                    3'd6: underrun_q   <= underrun_q &
                                          ~i_wdata[CHANNELS-1:0];
                    3'd7: master_vol_q <= i_wdata;
                    default: ;
                endcase
            end
            if (bus_write && sel_chan) begin
                case (reg_sel)
                    3'd1: inc_q[chan_idx] <= i_wdata;
                    3'd2: vol_q[chan_idx] <= i_wdata;
                    3'd5: wm_q[chan_idx]  <= i_wdata;
                    default: ;
                endcase
            end
        end
    end

    assign o_irq = |(irq_status_q & irq_enable_q);

    // ── Register reads ───────────────────────────────────────
    logic [31:0] rdata_next;

    always_comb begin
        if (!sel_chan) begin
            case (reg_sel)
                3'd0:    rdata_next = i_addr[5] ? 32'(status) : CAP_VALUE;
                3'd1:    rdata_next = 32'(SAMPLE_HZ);
                3'd2:    rdata_next = {31'd0, enable_q};
                3'd3:    rdata_next = 32'(chan_en_q);
                3'd4:    rdata_next = 32'(irq_status_q);
                3'd5:    rdata_next = 32'(irq_enable_q);
                3'd6:    rdata_next = 32'(underrun_q);
                default: rdata_next = master_vol_q;
            endcase
        end else begin
            case (reg_sel)
                3'd0:    rdata_next = 32'd0;              // CHAN CTRL
                3'd1:    rdata_next = inc_q[chan_idx];
                3'd2:    rdata_next = vol_q[chan_idx];
                3'd3:    rdata_next = 32'(DEPTH);
                3'd4:    rdata_next = 32'(level[chan_idx]);
                3'd5:    rdata_next = wm_q[chan_idx];
                default: rdata_next = 32'd0;              // FIFO reads 0
            endcase
        end
    end

    // A read answers from a register, presented in the ack cycle.
    always_ff @(posedge i_clk) begin
        if (i_rst)
            o_rdata <= 32'd0;
        else if (bus_read)
            o_rdata <= rdata_next;
    end

endmodule
