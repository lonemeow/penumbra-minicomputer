// penumbra4_fetch_queue — holds fetched ops until issue takes them.
//
// Up to ENQ_WIDTH entries enter in a cycle, in order, and the oldest
// HEAD_WIDTH are presented to issue, which takes some number of them from
// the head. The entries are an opaque payload, so where decode and the fuser
// sit changes what enters, not the queue.
//
// o_room says ENQ_WIDTH entries fit now, from a register: IF2 enqueues by
// it, and the fetch unit launches by it. A flush empties the queue whatever
// else the cycle does.

module penumbra4_fetch_queue #(
    parameter type entry_t    = logic,
    parameter int  DEPTH      = 4,
    parameter int  ENQ_WIDTH  = 4,
    parameter int  HEAD_WIDTH = 3
)(
    input  logic                               i_clk,
    input  logic                               i_rst,
    input  logic                               i_flush,

    input  logic                               i_enq,
    input  entry_t [ENQ_WIDTH-1:0]             i_entries,
    input  logic [$clog2(ENQ_WIDTH+1)-1:0]     i_enq_count,
    output logic                               o_room,

    output entry_t [HEAD_WIDTH-1:0]            o_head,
    output logic   [HEAD_WIDTH-1:0]            o_head_valid,
    input  logic [$clog2(HEAD_WIDTH+1)-1:0]    i_deq_count
);

    localparam int PTR_BITS   = $clog2(DEPTH);
    localparam int COUNT_BITS = $clog2(DEPTH + 1);

    typedef logic [PTR_BITS-1:0]   ptr_t;
    typedef logic [COUNT_BITS-1:0] count_t;

    build_check #(
        .COND ((DEPTH & (DEPTH - 1)) == 0 && DEPTH >= ENQ_WIDTH && DEPTH >= HEAD_WIDTH),
        .MSG  ("penumbra4_fetch_queue: DEPTH must be a power of two holding a line and the head")
    ) u_check_depth ();

    (* ram_style = "registers" *) entry_t entries_q [DEPTH];

    ptr_t   head_q, head_d;
    ptr_t   tail_q, tail_d;
    count_t count_q, count_d;
    logic   room_q, room_d;
    count_t enq_count;
    count_t deq_count;

    assign enq_count = i_enq ? count_t'(i_enq_count) : '0;
    assign deq_count = count_t'(i_deq_count);

    always_comb begin
        if (i_flush) begin
            head_d  = '0;
            tail_d  = '0;
            count_d = '0;
        end else begin
            head_d  = head_q + ptr_t'(deq_count);
            tail_d  = tail_q + ptr_t'(enq_count);
            count_d = count_q + enq_count - deq_count;
        end
        room_d = (count_t'(DEPTH) - count_d) >= count_t'(ENQ_WIDTH);
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            head_q  <= '0;
            tail_q  <= '0;
            count_q <= '0;
            room_q  <= 1'b1;
        end else begin
            head_q  <= head_d;
            tail_q  <= tail_d;
            count_q <= count_d;
            room_q  <= room_d;
        end
    end

    always_ff @(posedge i_clk) begin
        for (int k = 0; k < ENQ_WIDTH; k++) begin
            if (i_enq && !i_flush && count_t'(k) < count_t'(i_enq_count)) begin
                entries_q[tail_q + ptr_t'(k)] <= i_entries[k];
            end
        end
    end

    always_comb begin
        for (int k = 0; k < HEAD_WIDTH; k++) begin
            o_head[k]       = entries_q[head_q + ptr_t'(k)];
            o_head_valid[k] = count_t'(k) < count_q;
        end
    end

    assign o_room = room_q;

    // ── Contract ─────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst && !i_flush) begin
            assert (!i_enq || room_q)
                else $error("penumbra4_fetch_queue: an enqueue without room");
            assert (deq_count <= count_q)
                else $error("penumbra4_fetch_queue: more taken than the queue holds");
        end
    end

endmodule
