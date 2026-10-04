// I2S output stage — PCM frames onto a serial link the converter clocks.
//
// The converter drives both of the link's clocks, BCLK (one per bit) and
// LRCLK (one per frame), and the stage follows them, so the sample rate
// is the converter's and no FPGA clock sets it. Each frame is requested
// from the device as the frame before it begins, which gives the device
// a whole frame period to answer.
//
// The format is I2S. LRCLK is low for the left channel and high for the
// right, and changes on a BCLK falling edge. Data changes after each
// falling edge and the converter samples it on the rising one, and a
// channel's MSB comes one BCLK after LRCLK changes. A word is 16 bits
// sent MSB first; a slot can be any width from 16 BCLKs up, the stage
// sending zeros after the word until LRCLK changes again.
//
// The stage oversamples both clocks on the audio clock rather than
// clocking anything on BCLK, so the link adds no clock domain to the
// device. The price is latency: a BCLK edge goes through two
// synchroniser stages and one more register before the data pin
// answers it, LATENCY clocks after it happened at worst. All of that has
// to fit in half a BCLK, the time between the falling edge the data
// follows and the rising edge that samples it, with one clock to spare
// for the pads, the board and the converter's setup time. At 60 MHz and
// a 3.072 MHz BCLK, the data lands 50 ns after a falling edge at most,
// 113 ns ahead of the rising edge.
//
// The device holds each frame on i_frame_l and i_frame_r until the next
// one, so the left word goes out straight from it. The right word is
// copied when the left slot begins: the frame requested then arrives
// during the left slot, and would otherwise replace the right word
// before its slot.
//
// The link is the converter's, and it runs on through a reset of the
// machine, which lasts hundreds of frames. A word cut short would still
// be taken as a whole sample, up to full scale whatever the word was:
// -1 cut after its first bit reads as the most negative value. So a
// reset touches nothing the stage sends. The frame in progress goes out
// whole, the frames after it carry what the device holds in reset, and
// only the requests stop.

module audio_i2s_out #(
    parameter int CLK_HZ  = 60_000_000,
    parameter int BCLK_HZ = 3_072_000      // the fastest BCLK it has to follow
)(
    input  logic               i_clk,
    input  logic               i_rst,
    input  logic signed [15:0] i_frame_l,  // held between frames
    input  logic signed [15:0] i_frame_r,
    output logic               o_frame_req, // one i_clk pulse per frame

    // ── Pins ─────────────────────────────────────────────────
    input  logic               i_bclk,
    input  logic               i_lrclk,
    output logic               o_sdata
);

    // A falling BCLK edge is sampled by the first synchroniser stage up
    // to a clock after it happened, reaches the second a clock later,
    // and moves the data register on the next.
    localparam int LATENCY = 3;

    build_check #(
        .COND (2 * (LATENCY + 1) * BCLK_HZ <= CLK_HZ),
        .MSG  ("audio_i2s_out: CLK_HZ too slow to answer a BCLK edge within half a BCLK")
    ) u_check_ratio ();

    // ── Pins in ──────────────────────────────────────────────
    // ASYNC_REG on every synchroniser flop: Vivado places each pair
    // together and its clock-crossing check recognises it. BCLK and
    // LRCLK go through identical stages, so LRCLK as seen at a BCLK
    // rise is LRCLK as it was at that rise, half a BCLK from either of
    // its changes.
    (* ASYNC_REG = "TRUE" *) logic bclk_s1_q, bclk_s2_q;
    (* ASYNC_REG = "TRUE" *) logic lrclk_s1_q, lrclk_s2_q;
    logic bclk_prev_q;     // bclk_s2_q a clock earlier
    logic lrclk_q;         // LRCLK at the last BCLK rise

    always_ff @(posedge i_clk) begin
        {bclk_s2_q, bclk_s1_q}   <= {bclk_s1_q, i_bclk};
        {lrclk_s2_q, lrclk_s1_q} <= {lrclk_s1_q, i_lrclk};
        bclk_prev_q              <= bclk_s2_q;
    end

    logic bclk_rise, bclk_fall;

    assign bclk_rise = bclk_s2_q && !bclk_prev_q;
    assign bclk_fall = !bclk_s2_q && bclk_prev_q;

    // A slot begins at the first BCLK rise that sees LRCLK changed: the
    // rise that samples the last bit of the slot before, a BCLK ahead of
    // the falling edge that sends the new word's MSB.
    logic slot_start, left_start, right_start;

    assign slot_start  = bclk_rise && (lrclk_s2_q != lrclk_q);
    assign left_start  = slot_start && !lrclk_s2_q;
    assign right_start = slot_start && lrclk_s2_q;

    // No reset, here or on the words below: they follow the link, and
    // the link does not stop for one.
    always_ff @(posedge i_clk) begin
        if (bclk_rise)
            lrclk_q <= lrclk_s2_q;
    end

    // ── Words out ────────────────────────────────────────────
    logic [15:0] word_q;   // the slot's word, next bit out at the top
    logic [15:0] right_q;  // the frame's right word, through its left slot

    always_ff @(posedge i_clk) begin
        if (left_start) begin
            word_q  <= i_frame_l;
            right_q <= i_frame_r;
        end else if (right_start) begin
            word_q  <= right_q;
        end else if (bclk_fall) begin
            o_sdata <= word_q[15];
            word_q  <= {word_q[14:0], 1'b0};
        end
    end

    // ── Frame requests ───────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst)
            o_frame_req <= 1'b0;
        else
            o_frame_req <= left_start;
    end

endmodule
