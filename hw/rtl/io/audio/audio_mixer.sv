// Channel mixing datapath: rate, gain and sum, sequenced over channels.
//
// Runs in the output clock domain — the same clock the rate divider and
// the modulator use. On the ULX3S that is the 60 MHz net which also
// serves USB, which is an availability accident and not a dependency;
// nothing here knows about USB.
//
// One output frame arrives every rate-divider tick, some 1250 clocks at
// 60 MHz and 48 kHz, so the channels are walked through a single
// datapath rather than replicated. A channel then costs registers and
// walk cycles, never combinational depth — the class ceiling of 32
// still finishes inside a fifth of a frame period. The rate, gain and
// saturation logic exists once however many channels there are, which
// is also what puts the one multiply on a DSP.
//
// Cycles are what sequencing buys, and they are spent freely here. A
// gain multiply, an accumulate, the master multiply and the saturation
// each get their own cycle, with the sample and gain registered ahead
// of the multiply so the DSP sees registers on both sides. Collapsing
// that chain into one cycle costs nothing in cycles and everything in
// depth — two cascaded multiplies with the channel mux in front of them
// is some 30 ns, which is the whole clock period several times over.
//
// So the walk is longer than it needs to be on purpose:
//
//   CHANNELS * (prepare + MAX_ADVANCE pops + select + multiply + add)
//   + master multiply + saturate
//
// which for four channels at 4x is 34 cycles against a frame period of
// some 1250, and 258 at the class ceiling of 32 channels. It still has
// to finish inside that period or a tick lands on a walk in progress,
// dropping an output frame and leaving every channel's phase behind the
// rate; the assertion at the foot is what says so rather than leaving
// it assumed, since the number that shrinks the budget is the divider
// and that is set elsewhere.
//
// Per frame, per channel: work out how far the source advances, take
// that many frames from the queue, scale what lands by VOL, and add it
// to the sum. The sum carries full width and saturates once, after
// MASTER_VOL, as the class requires — a wrapped sum turns a loud
// passage into broadband noise where a clipped one is distortion.
//
// Queues deal in words and sample formats; what reaches here is always
// a signed pair, so format costs nothing per channel either.
//
// A discarded queue takes its channel's phase with it. The fraction is
// where in a source frame the output currently sits, which is a fact
// about material that no longer exists once the queue is flushed;
// keeping it would offset the first frame of the replacement by part of
// a sample.

module audio_mixer #(
    parameter int CHANNELS    = 4,
    parameter int SAMPLE_W    = 16,
    // Whole source frames per output frame the walk budgets for. The
    // class requires 1/256x through 4x; a larger INC is clamped rather
    // than allowed to stretch the frame period.
    parameter int MAX_ADVANCE = 4
) (
    input  logic i_clk,
    input  logic i_rst,

    input  logic i_frame,                       // rate-divider tick

    // One pulse per channel whose queue was discarded, raised in this
    // domain by the queue itself so the phase clears on the same cycle
    // as the frames it belonged to.
    input  logic [CHANNELS-1:0] i_flush,

    // Written in the register domain, changing at tick rate at most, so
    // these cross as plain synchronised values.
    input  logic [CHANNELS-1:0] i_chan_en,
    input  logic [31:0]         i_inc [CHANNELS],   // 16.16
    input  logic [31:0]         i_vol [CHANNELS],   // {right, left}
    input  logic [31:0]         i_master_vol,

    // Queue read port. i_left/i_right are the head of channel o_ch;
    // o_advance consumes it, and the next head is ready the cycle after.
    // Width guarded: $clog2(1) is zero, and a single-channel instance
    // is a legal configuration of this class.
    output logic [((CHANNELS > 1) ? $clog2(CHANNELS) : 1)-1:0] o_ch,
    output logic                        o_advance,
    input  logic signed [SAMPLE_W-1:0]  i_left,
    input  logic signed [SAMPLE_W-1:0]  i_right,
    input  logic                        i_empty,

    // Held for the frame it happened in, not sticky: the register
    // domain owns the sticky bit the class exposes, and a frame is long
    // enough for a slower clock to catch this without a handshake.
    output logic [CHANNELS-1:0]         o_underrun,

    output logic signed [SAMPLE_W-1:0]  o_left,
    output logic signed [SAMPLE_W-1:0]  o_right,
    output logic                        o_valid
);

    localparam int CH_W  = (CHANNELS > 1) ? $clog2(CHANNELS) : 1;
    localparam int ADV_W = $clog2(MAX_ADVANCE + 1);
    // Headroom for CHANNELS full-scale channels summed before scaling.
    localparam int ACC_W = SAMPLE_W + CH_W + 1;

    localparam logic signed [SAMPLE_W-1:0] OUT_MIN =
        signed'({1'b1, {(SAMPLE_W-1){1'b0}}});
    localparam logic signed [SAMPLE_W-1:0] OUT_MAX =
        signed'({1'b0, {(SAMPLE_W-1){1'b1}}});

    typedef enum logic [2:0] {
        S_IDLE,    // between frames
        S_PREP,    // work out this channel's advance
        S_POP,     // take one source frame, once per cycle
        S_SEL,     // register the sample and gain the multiply will see
        S_VOL,     // multiply, into a register
        S_ACC,     // add into the sum
        S_MASTER,  // master multiply, into a register
        S_SAT      // saturate and emit
    } state_e;

    state_e             state_q;
    logic [CH_W-1:0]    ch_q;
    logic [ADV_W-1:0]   to_pop_q;

    // Only the fraction survives a frame: the whole part of the phase is
    // consumed as it appears, so keeping it would be keeping a number
    // that is always immediately spent.
    logic [15:0]                frac_q   [CHANNELS];
    logic signed [SAMPLE_W-1:0] held_l_q [CHANNELS];
    logic signed [SAMPLE_W-1:0] held_r_q [CHANNELS];

    logic signed [ACC_W-1:0]    acc_l_q, acc_r_q;

    assign o_ch = ch_q;

    // ── Rate ─────────────────────────────────────────────────
    logic [16:0]      frac_sum;   // carry in the top bit
    logic [16:0]      inc_whole;
    logic [ADV_W-1:0] advance_n;

    assign frac_sum  = {1'b0, frac_q[ch_q]} + {1'b0, i_inc[ch_q][15:0]};
    assign inc_whole = {1'b0, i_inc[ch_q][31:16]} + {16'd0, frac_sum[16]};
    assign advance_n = (inc_whole > 17'(MAX_ADVANCE)) ? ADV_W'(MAX_ADVANCE)
                                                      : ADV_W'(inc_whole);

    // ── Gain ─────────────────────────────────────────────────
    // Unsigned 0..0xFFFF against a signed sample, one multiply shared by
    // every channel.
    //
    // The class calls 0xFFFF unity, and 0xFFFF/0x10000 is not: a sample
    // through two of these — channel then master — loses a couple of
    // low bits at nominally full gain. Mapping the top code to 0x10000
    // makes unity exact and leaves every other code where it was.
    function automatic logic [16:0] gain(input logic [15:0] v);
        return {1'b0, v} + 17'((&v) ? 1 : 0);
    endfunction

    // Registered on the way in, so the multiply sees a register rather
    // than the channel mux, and registered on the way out.
    logic signed [SAMPLE_W-1:0]  samp_l_q, samp_r_q;
    logic [16:0]                 vgain_l_q, vgain_r_q;
    logic signed [SAMPLE_W+17:0] prod_l_q, prod_r_q;

    // The master gain changes at most once a frame, so it is taken once
    // at the top of the walk rather than re-derived at the end of it.
    logic [16:0]                 mgain_l_q, mgain_r_q;
    logic signed [ACC_W+17:0]    mprod_l_q, mprod_r_q;



    function automatic logic signed [SAMPLE_W-1:0] sat(
            input logic signed [ACC_W-1:0] v);
        logic [ACC_W-SAMPLE_W:0] top;
        top = v[ACC_W-1:SAMPLE_W-1];
        if (&top || ~|top) return SAMPLE_W'(v);
        else               return v[ACC_W-1] ? OUT_MIN : OUT_MAX;
    endfunction

    // ── Walk ─────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q    <= S_IDLE;
            ch_q       <= '0;
            to_pop_q   <= '0;
            acc_l_q    <= '0;
            acc_r_q    <= '0;
            o_advance  <= 1'b0;
            o_valid    <= 1'b0;
            o_left     <= '0;
            o_right    <= '0;
            o_underrun <= '0;
            samp_l_q   <= '0;
            samp_r_q   <= '0;
            vgain_l_q  <= '0;
            vgain_r_q  <= '0;
            prod_l_q   <= '0;
            prod_r_q   <= '0;
            mgain_l_q  <= '0;
            mgain_r_q  <= '0;
            mprod_l_q  <= '0;
            mprod_r_q  <= '0;
            for (int c = 0; c < CHANNELS; c++) begin
                frac_q[c]   <= '0;
                held_l_q[c] <= '0;
                held_r_q[c] <= '0;
            end
        end else begin
            o_valid   <= 1'b0;
            o_advance <= 1'b0;

            case (state_q)
                S_IDLE: if (i_frame) begin
                    ch_q       <= '0;
                    acc_l_q    <= '0;
                    acc_r_q    <= '0;
                    o_underrun <= '0;
                    mgain_l_q  <= gain(i_master_vol[15:0]);
                    mgain_r_q  <= gain(i_master_vol[31:16]);
                    state_q    <= S_PREP;
                end

                S_PREP: begin
                    frac_q[ch_q] <= frac_sum[15:0];
                    to_pop_q     <= advance_n;
                    // A disabled channel still keeps its phase, so
                    // re-enabling resumes where it left off.
                    state_q <= (i_chan_en[ch_q] && advance_n != '0)
                             ? S_POP : S_SEL;
                end

                S_POP: begin
                    if (i_empty) begin
                        // The class holds the last consumed sample
                        // rather than emitting a gap, and reports it.
                        o_underrun[ch_q] <= 1'b1;
                        state_q          <= S_SEL;
                    end else begin
                        o_advance      <= 1'b1;
                        held_l_q[ch_q] <= i_left;
                        held_r_q[ch_q] <= i_right;
                        to_pop_q       <= to_pop_q - 1'b1;
                        if (to_pop_q == ADV_W'(1))
                            state_q <= S_SEL;
                    end
                end

                // Take the channel mux here, so the multiply that
                // follows sees registers and not a select. A disabled
                // channel contributes zero rather than being skipped,
                // which keeps the walk's length independent of what
                // happens to be playing.
                S_SEL: begin
                    samp_l_q  <= i_chan_en[ch_q] ? held_l_q[ch_q] : '0;
                    samp_r_q  <= i_chan_en[ch_q] ? held_r_q[ch_q] : '0;
                    vgain_l_q <= gain(i_vol[ch_q][15:0]);
                    vgain_r_q <= gain(i_vol[ch_q][31:16]);
                    state_q   <= S_VOL;
                end

                S_VOL: begin
                    prod_l_q <= samp_l_q * signed'({1'b0, vgain_l_q});
                    prod_r_q <= samp_r_q * signed'({1'b0, vgain_r_q});
                    state_q  <= S_ACC;
                end

                S_ACC: begin
                    acc_l_q <= acc_l_q + ACC_W'(prod_l_q >>> 16);
                    acc_r_q <= acc_r_q + ACC_W'(prod_r_q >>> 16);

                    if (ch_q == CH_W'(CHANNELS - 1)) begin
                        state_q <= S_MASTER;
                    end else begin
                        ch_q    <= ch_q + 1'b1;
                        state_q <= S_PREP;
                    end
                end

                S_MASTER: begin
                    mprod_l_q <= acc_l_q * signed'({1'b0, mgain_l_q});
                    mprod_r_q <= acc_r_q * signed'({1'b0, mgain_r_q});
                    state_q   <= S_SAT;
                end

                S_SAT: begin
                    o_left  <= sat(ACC_W'(mprod_l_q >>> 16));
                    o_right <= sat(ACC_W'(mprod_r_q >>> 16));
                    o_valid <= 1'b1;
                    state_q <= S_IDLE;
                end

                default: state_q <= S_IDLE;
            endcase

            // After the walk's own update, so a flush landing on the
            // step that advances this channel still wins: the fraction
            // it clears belongs to material the consumer has discarded,
            // and carrying it forward would offset the first frame of
            // whatever replaces it.
            for (int c = 0; c < CHANNELS; c++)
                if (i_flush[c])
                    frac_q[c] <= '0;
        end
    end

    // synthesis translate_off
    always_ff @(posedge i_clk)
        if (!i_rst && i_frame)
            assert (state_q == S_IDLE)
                else $error("audio_mixer: rate tick arrived before the walk finished");
    // synthesis translate_on

endmodule
