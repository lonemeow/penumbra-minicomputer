// penumbra3_stream_buffer -- stream buffers beside the gen3 L2.
//
// Each buffer follows one stream of the L2's memory fills, a fill being a
// cacheable line read the L2 misses, and keeps its expected line: the line
// after its stream's latest fill, the one the stream reads next if it goes
// on (doc/internals/penumbra3/memory-performance.md, "Next-line prefetch
// into stream buffers"). The expected line is the only one a buffer wants
// fetched, fetches or holds. At a fill of line X:
//
//  - a buffer holding X serves it: X + 1 becomes its expected line, and it
//    wants that line fetched;
//  - otherwise a buffer expecting X has seen two fills to consecutive
//    lines: X + 1 becomes its expected line, and it wants that line
//    fetched;
//  - otherwise the buffer used least recently gives up its stream, and
//    what it held or was fetching, for a new one: X + 1 becomes its
//    expected line, and it wants nothing yet, as one fill alone shows no
//    walk.
//
// A buffer never wants a line in another page than its stream's latest
// fill. Every address here is a line address: the offset within the line
// dropped.
//
// The uncore asks at its verdict whether a buffer holds a line (o_hit) or
// is fetching it (o_pending). It serves every fill whose line a buffer
// holds from that buffer, and waits out a fetch of the line instead of
// filling it. It reports each fill, post and uncached write a cycle
// later, from its registers, so the policy and every drop run off the
// verdict's cone. A write to a line a buffer holds drops it,
// and one to a line a buffer is fetching discards the fetch: what was
// fetched predates the write. The fetch a buffer wants is offered on
// o_pf_*, from registers, and the uncore runs it on the bus master when the
// bus has nothing else to do. A fetch that faults, or that is discarded, is
// thrown away when it arrives: nothing waits on a prefetch, so nothing
// hears of it.
module penumbra3_stream_buffer
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
#(
    parameter  int LINE_BYTES     = 16,
    localparam int LINE_WORDS     = LINE_BYTES / 4,
    localparam int OFFSET_BITS    = $clog2(LINE_BYTES),
    localparam int LINE_ADDR_BITS = 32 - OFFSET_BITS,
    localparam int PAGE_LINE_BITS = PAGE_LOG2 - OFFSET_BITS
) (
    input  logic                        i_clk,
    input  logic                        i_rst,

    // ── The verdict's question, combinational ────────────────────
    input  logic [LINE_ADDR_BITS-1:0]   i_lookup_addr,
    output logic                        o_hit,          // a buffer holds the line
    output stream_buf_t                 o_hit_buf,      // which
    output logic                        o_pending,      // a buffer is fetching it

    // ── A buffer's held line ─────────────────────────────────────
    input  stream_buf_t                 i_read_buf,
    output logic [LINE_WORDS-1:0][31:0] o_read_line,

    // ── Events, a cycle after the verdict ────────────────────────
    input  logic                        i_fill,         // a fill: a line read the L2 missed
    input  logic                        i_write,        // a post, or an uncached write
    input  logic [LINE_ADDR_BITS-1:0]   i_event_addr,   // the fill's or the write's
    input  logic                        i_clear,        // the L2's invalidation starts

    // ── The prefetch ─────────────────────────────────────────────
    output logic                        o_pf_valid,
    output logic [LINE_ADDR_BITS-1:0]   o_pf_addr,
    input  logic                        i_pf_taken,     // the bus master took it
    input  logic                        i_pf_done,      // the bus master completed it
    input  logic                        i_pf_fault,     // valid with i_pf_done
    input  logic [LINE_WORDS-1:0][31:0] i_pf_line       // valid with i_pf_done
);

    // The replacement state is the buffer used less recently, and the
    // other buffer is the one used more recently.
    build_check #(
        .COND (STREAM_BUFFERS == 2),
        .MSG  ("penumbra3_stream_buffer: the replacement state is written for two buffers")
    ) u_check_buffers ();

    typedef logic [LINE_ADDR_BITS-1:0]   line_addr_t;
    typedef logic [LINE_WORDS-1:0][31:0] line_t;

    // Per buffer: whether it follows a stream, its expected line, and what
    // it does with that line: wants it fetched, is fetching it, or holds it.
    // A fetch marked for discard is thrown away when it arrives: a write, a
    // new stream or the L2's invalidation overtook it.
    logic [STREAM_BUFFERS-1:0] tracking_q, want_q, fetching_q, held_q, discard_q;
    line_addr_t                expected_q [STREAM_BUFFERS];
    line_t                     line_q     [STREAM_BUFFERS];  // the held line
    stream_buf_t               lru_q;        // the buffer used less recently
    stream_buf_t               fetch_buf_q;  // the buffer whose fetch the bus master runs

    // ── The verdict's question ───────────────────────────────────
    logic [STREAM_BUFFERS-1:0] lookup_expected;
    always_comb begin
        o_hit     = 1'b0;
        o_hit_buf = '0;
        o_pending = 1'b0;
        for (int b = 0; b < STREAM_BUFFERS; b++) begin
            lookup_expected[b] = expected_q[b] == i_lookup_addr;
            if (held_q[b] && lookup_expected[b]) begin
                o_hit     = 1'b1;
                o_hit_buf = stream_buf_t'(b);
            end
            if (fetching_q[b] && !discard_q[b] && lookup_expected[b])
                o_pending = 1'b1;
        end
    end

    assign o_read_line = line_q[i_read_buf];

    // ── The fetch offered ────────────────────────────────────────
    // The lowest buffer wanting one. Not while an event arrives: it may
    // change what the buffer wants.
    stream_buf_t offer_buf;
    always_comb begin
        offer_buf = '0;
        for (int b = STREAM_BUFFERS - 1; b >= 0; b--)
            if (want_q[b]) offer_buf = stream_buf_t'(b);
    end
    assign o_pf_valid = |want_q && !i_fill && !i_write && !i_clear;
    assign o_pf_addr  = expected_q[offer_buf];

    // ── The policy at a fill ─────────────────────────────────────
    // stream_buf is the buffer whose stream the fill belongs to; new_stream
    // that the fill starts that stream, rather than one going on. A buffer
    // holding the line expects it too, so a fill both buffers expect goes to
    // the one holding it, which served it.
    logic [STREAM_BUFFERS-1:0] fill_expected;
    stream_buf_t               stream_buf;
    logic                      new_stream;
    always_comb begin
        for (int b = 0; b < STREAM_BUFFERS; b++)
            fill_expected[b] = tracking_q[b] && expected_q[b] == i_event_addr;
        stream_buf = lru_q;
        new_stream = 1'b1;
        for (int b = 0; b < STREAM_BUFFERS; b++)
            if (fill_expected[b]) begin
                stream_buf = stream_buf_t'(b);
                new_stream = 1'b0;
            end
        for (int b = 0; b < STREAM_BUFFERS; b++)
            if (fill_expected[b] && held_q[b]) stream_buf = stream_buf_t'(b);
    end

    line_addr_t after_fill;
    logic       after_fill_in_page;
    assign after_fill         = i_event_addr + 1'b1;
    assign after_fill_in_page = after_fill[PAGE_LINE_BITS-1:0] != '0;

    // ── State ────────────────────────────────────────────────────
    // Later assignments win: a fetch's completion, then the next fetch taken
    // in the same cycle, then the event.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tracking_q  <= '0;
            want_q      <= '0;
            fetching_q  <= '0;
            held_q      <= '0;
            discard_q   <= '0;
            lru_q       <= '0;
            fetch_buf_q <= '0;
        end else begin
            if (i_pf_done) begin
                fetching_q[fetch_buf_q] <= 1'b0;
                discard_q[fetch_buf_q]  <= 1'b0;
                if (!discard_q[fetch_buf_q] && !i_pf_fault) held_q[fetch_buf_q] <= 1'b1;
            end
            if (i_pf_taken) begin
                want_q[offer_buf]     <= 1'b0;
                fetching_q[offer_buf] <= 1'b1;
                discard_q[offer_buf]  <= 1'b0;
                fetch_buf_q           <= offer_buf;
            end
            if (i_clear) begin
                tracking_q <= '0;
                want_q     <= '0;
                held_q     <= '0;
                discard_q  <= fetching_q;
            end
            if (i_write)
                for (int b = 0; b < STREAM_BUFFERS; b++)
                    if (expected_q[b] == i_event_addr) begin
                        held_q[b] <= 1'b0;
                        if (fetching_q[b]) discard_q[b] <= 1'b1;
                    end
            if (i_fill) begin
                tracking_q[stream_buf] <= 1'b1;
                want_q[stream_buf]     <= !new_stream && after_fill_in_page;
                held_q[stream_buf]     <= 1'b0;
                if (new_stream && fetching_q[stream_buf]) discard_q[stream_buf] <= 1'b1;
                lru_q <= ~stream_buf;
            end
        end
        if (i_fill) expected_q[stream_buf] <= after_fill;
        if (i_pf_done && !discard_q[fetch_buf_q] && !i_pf_fault) line_q[fetch_buf_q] <= i_pf_line;
    end

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst) begin
            assert (!(i_fill && i_write))
                else $error("penumbra3_stream_buffer: a fill and a write in one cycle");
            assert (!(i_pf_taken && !o_pf_valid))
                else $error("penumbra3_stream_buffer: a fetch taken that was not offered");
            assert (!(i_pf_done && !fetching_q[fetch_buf_q]))
                else $error("penumbra3_stream_buffer: a fetch completed that was not running");
            assert (!(i_fill && |(fill_expected & fetching_q & ~discard_q)))
                else $error("penumbra3_stream_buffer: a fill of a line a buffer is fetching");
            assert ((held_q & fetching_q) == '0)
                else $error("penumbra3_stream_buffer: a buffer holds its line and fetches it");
        end
    end

endmodule
