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
// FLUSH is a write-side operation only: snapping the write pointer to
// the synchronized read pointer empties the queue from both sides
// without anything having to cross. The read side sees empty once its
// own view catches up, which is inside the gap between the mixer's
// per-frame bursts.

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
    output logic                   o_empty
);

    localparam int PTR_W = $clog2(DEPTH);

    // One bit above the index, so full and empty are distinguishable.
    logic [PTR_W:0] wr_q, rd_q;
    logic [PTR_W:0] wr_gray_q, rd_gray_q;
    logic [PTR_W:0] wr_gray_a1, wr_gray_a2;   // into the output domain
    logic [PTR_W:0] rd_gray_r1, rd_gray_r2;   // into the register domain

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
    logic [31:0] mem [0:DEPTH-1];

    // ── Write side ───────────────────────────────────────────
    logic [PTR_W:0] rd_in_wr;      // read pointer as this side sees it
    logic [PTR_W:0] used;

    assign rd_in_wr = from_gray(rd_gray_r2);
    assign used     = wr_q - rd_in_wr;
    assign o_level  = (PTR_W+1)'(DEPTH) - used;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            wr_q       <= '0;
            wr_gray_q  <= '0;
            rd_gray_r1 <= '0;
            rd_gray_r2 <= '0;
        end else begin
            rd_gray_r1 <= rd_gray_q;
            rd_gray_r2 <= rd_gray_r1;

            if (i_flush) begin
                wr_q      <= rd_in_wr;
                wr_gray_q <= to_gray(rd_in_wr);
            end else if (i_push && used != (PTR_W+1)'(DEPTH)) begin
                mem[wr_q[PTR_W-1:0]] <= i_wdata;
                wr_q                 <= wr_q + 1'b1;
                wr_gray_q            <= to_gray(wr_q + 1'b1);
            end
        end
    end

    // ── Read side ────────────────────────────────────────────
    logic [PTR_W:0] wr_in_rd;
    logic [31:0]    head;

    assign wr_in_rd = from_gray(wr_gray_a2);
    assign o_empty  = (rd_q == wr_in_rd);

    // Registered read: the head is what rd_q pointed at last cycle, so a
    // pop and the next head land one cycle apart, which is the rate the
    // mixer consumes at.
    always_ff @(posedge i_aclk) begin
        if (i_arst) begin
            rd_q       <= '0;
            rd_gray_q  <= '0;
            wr_gray_a1 <= '0;
            wr_gray_a2 <= '0;
            head       <= '0;
        end else begin
            wr_gray_a1 <= wr_gray_q;
            wr_gray_a2 <= wr_gray_a1;

            if (i_pop && !o_empty) begin
                rd_q      <= rd_q + 1'b1;
                rd_gray_q <= to_gray(rd_q + 1'b1);
            end
            head <= mem[(i_pop && !o_empty) ? (rd_q[PTR_W-1:0] + 1'b1)
                                            : rd_q[PTR_W-1:0]];
        end
    end

    assign o_left  = signed'(head[SAMPLE_W-1:0]);
    assign o_right = signed'(head[16 +: SAMPLE_W]);

endmodule
