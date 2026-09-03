// Hands one audio frame across the clock boundary, on demand.
//
// The output side asks for a frame each time it consumes one; the
// register side answers within a sample period. Only a toggle crosses
// in each direction, and the data has been stable for the whole round
// trip before it is sampled.
//
// The contract that makes this safe is that a request is answered
// before the next one is issued. The round trip is two synchronizer
// stages each way — under ten output clocks at any sane ratio — against
// a sample period of hundreds, so the margin is enormous; the assertion
// below is what says so rather than leaving it to be assumed.
//
// Requesting a frame at a time rather than sharing a FIFO across the
// boundary is what keeps the queue's occupancy exact: LEVEL and
// UNDERRUN are counted where frames are written, not inferred from a
// pointer that arrives late.
//
// The output side prefetches, so o_data changes on i_take and not
// whenever the answer happens to arrive. i_prime fills that prefetch
// without consuming, so a channel starts on its first queued frame
// rather than on the empty value the prefetch held before it ran.

module audio_frame_cdc #(
    parameter int W = 32
) (
    // ── Register domain ──────────────────────────────────────
    input  logic         i_clk,
    input  logic         i_rst,
    output logic         o_req,     // one pulse: present a frame this cycle
    input  logic [W-1:0] i_data,    // valid while o_req is high

    // ── Output domain ────────────────────────────────────────
    input  logic         i_aclk,
    input  logic         i_arst,
    input  logic         i_take,    // one pulse per output frame
    input  logic         i_prime,   // fetch without consuming
    output logic [W-1:0] o_data
);

    // ── Output side: consume, then ask for the next ──────────
    logic req_tog_q;
    logic [W-1:0] next_q;

    always_ff @(posedge i_aclk) begin
        if (i_arst) begin
            req_tog_q <= 1'b0;
            o_data    <= '0;
        end else begin
            if (i_take || i_prime) req_tog_q <= ~req_tog_q;
            if (i_take)            o_data    <= next_q;
        end
    end

    // ── Register side: answer the request ────────────────────
    logic req_s1, req_s2, req_s3;
    logic [W-1:0] data_q;
    logic ack_tog_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            {req_s1, req_s2, req_s3} <= '0;
            ack_tog_q                <= 1'b0;
            data_q                   <= '0;
        end else begin
            {req_s3, req_s2, req_s1} <= {req_s2, req_s1, req_tog_q};
            if (o_req) begin
                data_q    <= i_data;
                ack_tog_q <= ~ack_tog_q;
            end
        end
    end

    assign o_req = req_s2 ^ req_s3;

    // ── Output side: collect the answer ──────────────────────
    // data_q settled on the same edge that flipped ack_tog_q, so it has
    // been stable for the two synchronizer stages by the time this
    // fires.
    logic ack_s1, ack_s2, ack_s3;

    logic answered;
    assign answered = ack_s2 ^ ack_s3;

    always_ff @(posedge i_aclk) begin
        if (i_arst) begin
            {ack_s1, ack_s2, ack_s3} <= '0;
            next_q                   <= '0;
        end else begin
            {ack_s3, ack_s2, ack_s1} <= {ack_s2, ack_s1, ack_tog_q};
            if (answered) next_q <= data_q;
        end
    end

    logic outstanding_q;

    always_ff @(posedge i_aclk) begin
        if (i_arst)                 outstanding_q <= 1'b0;
        else if (i_take || i_prime) outstanding_q <= 1'b1;
        else if (answered)          outstanding_q <= 1'b0;
    end

    // synthesis translate_off
    always_ff @(posedge i_aclk)
        if (!i_arst && (i_take || i_prime))
            assert (!outstanding_q || answered)
                else $error("audio_frame_cdc: frame requested before the previous answer arrived");
    // synthesis translate_on

endmodule
