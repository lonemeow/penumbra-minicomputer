// One channel's frame queue, written and read on different clocks.
//
// The single-channel device kept the whole queue in the register domain
// and handed one frame across on request, which made LEVEL an exact
// count. A mixer walking several channels cannot work that way: the
// pops originate on the output side, and a round trip per pop would put
// four channels' worth of handshakes inside every frame period.
//
// So the memory is dual-ported — written on the register clock, read on
// the output clock — and only the pointers cross, gray-coded. LEVEL is
// then computed against a read pointer that is a few output clocks old.
// That staleness is one-sided: it under-reports free space, never over-
// reports it, so a consumer writing LEVEL frames still cannot overrun.
// At 48 kHz a frame is some 1250 output clocks and the crossing is
// three, so in practice the count is exact anyway.
//
// FLUSH discards the queue by moving the *read* pointer forward to the
// writer rather than the write pointer back to the reader. Which one
// moves is not a free choice: a gray code survives a clock crossing
// only because successive values differ in one bit, so a pointer that
// jumps cannot be the one whose gray code another domain samples
// asynchronously. The write pointer is sampled that way on every frame,
// so it is the one that must never jump. The read pointer's own gray
// code does jump, and the write side is kept clear of it by measuring
// occupancy against the target it asked the reader to jump to, trusting
// the synchronized value again only once it reads back that target.
//
// Frames pushed after a flush are kept: the target is latched before
// they arrive, so the reader lands exactly where the discarded material
// ended.

module audio_chan_fifo #(
    parameter int DEPTH    = 1024,   // frames
    parameter int SAMPLE_W = 16
) (
    // ── Write side: register domain ──────────────────────────
    input  logic                   i_clk,
    input  logic                   i_rst,
    input  logic                   i_push,
    input  logic [31:0]            i_wdata,
    input  logic                   i_flush,
    output logic [$clog2(DEPTH):0] o_level,   // free frames

    // ── Read side: output domain ─────────────────────────────
    input  logic                   i_aclk,
    input  logic                   i_arst,
    input  logic                   i_pop,
    output logic signed [SAMPLE_W-1:0] o_left,
    output logic signed [SAMPLE_W-1:0] o_right,
    output logic                   o_empty,
    // One i_aclk pulse on the cycle the discard takes effect, so state
    // held elsewhere about this channel's stream clears with it.
    output logic                   o_flushed
);

    localparam int PTR_W = $clog2(DEPTH);

    // One bit above the index, so full and empty are distinguishable.
    logic [PTR_W:0] wr_q, rd_q;
    logic [PTR_W:0] wr_gray_q, rd_gray_q;
    logic [PTR_W:0] wr_gray_a1, wr_gray_a2;   // into the output domain
    logic [PTR_W:0] rd_gray_r1, rd_gray_r2;   // into the register domain

    // Latched before any push that follows the flush, so it names the
    // end of the discarded material and not the end of the queue.
    logic [PTR_W:0] flush_target_q;
    logic           flush_pending_q;
    // Toggles rather than pulses: one bit crossing a clock boundary
    // needs no gray code, and a level the reader can edge-detect costs
    // no acknowledgement back.
    logic           flush_tog_q;

    function automatic logic [PTR_W:0] to_gray(input logic [PTR_W:0] v);
        return v ^ (v >> 1);
    endfunction

    function automatic logic [PTR_W:0] from_gray(input logic [PTR_W:0] g);
        logic [PTR_W:0] v;
        v = g;
        for (int i = 1; i <= PTR_W; i++)
            v = v ^ (g >> i);
        return v;
    endfunction

    // The memory itself. One word is one frame — 16-bit stereo, left in
    // the low half — so nothing here interprets the contents.
    (* ram_style = "block" *) logic [31:0] mem [0:DEPTH-1];

    // ── Write side ───────────────────────────────────────────
    logic [PTR_W:0] rd_in_wr;      // read pointer as this side sees it
    logic [PTR_W:0] used;
    logic           flush_done;

    assign rd_in_wr   = from_gray(rd_gray_r2);
    assign flush_done = flush_pending_q && (rd_in_wr == flush_target_q);

    // While the reader is mid-jump its gray code crosses more than one
    // bit, so a sample taken in flight decodes to an arbitrary pointer.
    // Occupancy is measured against the target instead, which counts
    // exactly the frames pushed since the flush — an under-count of what
    // is physically still there, so free space is over-reported for the
    // few cycles it takes the reader to land. The margin that makes that
    // safe is the queue depth against a round trip of a few cycles,
    // which the assertion below states rather than leaves implied.
    assign used    = flush_pending_q ? (wr_q - flush_target_q)
                                     : (wr_q - rd_in_wr);
    assign o_level = (PTR_W+1)'(DEPTH) - used;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            wr_q            <= '0;
            wr_gray_q       <= '0;
            rd_gray_r1      <= '0;
            rd_gray_r2      <= '0;
            flush_target_q  <= '0;
            flush_pending_q <= 1'b0;
            flush_tog_q     <= 1'b0;
        end else begin
            rd_gray_r1 <= rd_gray_q;
            rd_gray_r2 <= rd_gray_r1;

            if (i_flush && !flush_pending_q) begin
                flush_target_q  <= wr_q;
                flush_pending_q <= 1'b1;
                flush_tog_q     <= ~flush_tog_q;
            end else if (flush_done) begin
                flush_pending_q <= 1'b0;
            end

            if (i_push && used != (PTR_W+1)'(DEPTH)) begin
                mem[wr_q[PTR_W-1:0]] <= i_wdata;
                wr_q                 <= wr_q + 1'b1;
                wr_gray_q            <= to_gray(wr_q + 1'b1);
            end
        end
    end

    // ── Read side ────────────────────────────────────────────
    logic [PTR_W:0] wr_in_rd;
    logic [PTR_W:0] rd_d;
    logic [31:0]    head;
    logic           flush_a1, flush_a2, flush_a3;

    assign wr_in_rd  = from_gray(wr_gray_a2);
    assign o_empty   = (rd_q == wr_in_rd);
    assign o_flushed = flush_a2 ^ flush_a3;

    always_comb begin
        if (o_flushed)              rd_d = flush_target_q;
        else if (i_pop && !o_empty) rd_d = rd_q + 1'b1;
        else                        rd_d = rd_q;
    end

    // Registered read: the head is what rd_q pointed at last cycle, so a
    // pop and the next head land one cycle apart, which is the rate the
    // mixer consumes at.
    always_ff @(posedge i_aclk) begin
        if (i_arst) begin
            rd_q       <= '0;
            rd_gray_q  <= '0;
            wr_gray_a1 <= '0;
            wr_gray_a2 <= '0;
            flush_a1   <= 1'b0;
            flush_a2   <= 1'b0;
            flush_a3   <= 1'b0;
            head       <= '0;
        end else begin
            wr_gray_a1 <= wr_gray_q;
            wr_gray_a2 <= wr_gray_a1;

            {flush_a3, flush_a2, flush_a1} <= {flush_a2, flush_a1,
                                               flush_tog_q};

            rd_q      <= rd_d;
            rd_gray_q <= to_gray(rd_d);
            head      <= mem[rd_d[PTR_W-1:0]];
        end
    end

    assign o_left  = signed'(head[SAMPLE_W-1:0]);
    assign o_right = signed'(head[16 +: SAMPLE_W]);

    // synthesis translate_off
    logic [PTR_W:0] wr_gray_prev;

    always_ff @(posedge i_clk) begin
        wr_gray_prev <= wr_gray_q;

        if (!i_rst) begin
            // FLUSH and FIFO are separate registers, so one bus write
            // cannot be both; latching the target would otherwise drop
            // a frame pushed in the same cycle.
            assert (!(i_flush && i_push))
                else $error("audio_chan_fifo: flush and push in one cycle");

            // A second flush would move a target the reader is still
            // travelling to. Two of them cannot be closer than two bus
            // writes, which is longer than the round trip.
            assert (!(i_flush && flush_pending_q))
                else $error("audio_chan_fifo: flush while one is in flight");

            // What lets this pointer's gray code cross at all.
            assert ($countones(wr_gray_q ^ wr_gray_prev) <= 1)
                else $error("audio_chan_fifo: write pointer moved more than one step");

            // Free space is over-reported until the reader lands, so a
            // consumer must not be able to fill the queue inside the
            // round trip.
            assert (!flush_pending_q || used < (PTR_W+1)'(DEPTH))
                else $error("audio_chan_fifo: queue filled while a flush was in flight");
        end
    end
    // synthesis translate_on

endmodule
