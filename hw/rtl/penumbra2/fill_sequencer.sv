// fill_sequencer — Penumbra/2 L1 line-fill sequencer (L1↔L2 layer)
//
// Owns the L2→L1 transfer of the atomic full-line fill specified in
// doc/internals/penumbra2/memory-interface.md. Sits between the
// transactional I/D arbiter and the shared L2: single-beat traffic passes
// through transparently; a line request is unrolled into a word-stepped
// read stream against the L2's busy handshake and delivered to the granted
// L1's fill port.
//
// Mode select: `i_re && i_cacheable` is a line request — the L1 back-side
// encoding guarantees a forwarded single-beat read never carries
// cacheable=1, so the pattern is unambiguous. Everything else (cacheable
// write-through, uncacheable read/write) forwards combinationally with its
// full shape, completion by the downstream busy-drop.
//
// Line mode: the request is registered at the engage edge (the L1 holds it
// level for the whole transaction), then the sequencer walks the line's
// word addresses. Each downstream busy-low cycle is one beat: i_l2_rdata
// forwards to the fill port (o_fill_we / o_fill_word / o_fill_wdata) the
// same cycle, and the word counter advances at the edge. The advance is
// registered — presenting the next address one cycle after the beat — which
// is safe against double-capture because the L2 admits a new address only
// once its stage-1 has drained, and it still meets the L2 read pipeline's
// natural initiation interval. o_fill_done coincides with the last beat
// (the L1 fill-port contract allows it).
//
// Upstream o_busy holds high from the engage cycle through fill_done, so
// the arbiter's type-dependent completion never sees a busy-drop on a line
// transaction; pass-through traffic mirrors the downstream handshake.

module fill_sequencer #(
    parameter int LINE_BYTES = 16
)(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Upstream: the granted L1 request (via the arbiter) ───────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_re,
    input  logic        i_we,
    input  logic        i_cacheable,
    output logic [31:0] o_rdata,
    output logic        o_busy,
    output logic        o_fault,    // pass-through access fault (rides the busy-drop)

    // ── Fill stream to the granted L1 (routed by the arbiter) ────
    output logic                              o_fill_we,
    output logic [$clog2(LINE_BYTES/4)-1:0]   o_fill_word,
    output logic [31:0]                       o_fill_wdata,
    output logic                              o_fill_done,
    output logic                              o_fill_fault,  // line aborted: a beat faulted

    // ── Downstream: the shared L2's CPU-facing port ───────────────
    output logic [31:0] o_l2_addr,
    output logic [31:0] o_l2_wdata,
    output logic [3:0]  o_l2_byte_en,
    output logic        o_l2_re,
    output logic        o_l2_we,
    output logic        o_l2_cacheable,
    input  logic [31:0] i_l2_rdata,
    input  logic        i_l2_busy,
    input  logic        i_l2_fault    // bus-side access fault (rides the busy-drop)
);

    localparam int LINE_WORDS  = LINE_BYTES / 4;
    localparam int WORD_BITS   = $clog2(LINE_WORDS);
    localparam int OFFSET_BITS = $clog2(LINE_BYTES);

    if (LINE_WORDS < 2 || (1 << WORD_BITS) != LINE_WORDS) begin : g_check_line
        $error("fill_sequencer: line must be a power-of-two number of words, at least 2");
    end

    typedef enum logic [1:0] {
        S_IDLE,                 // pass-through; a line request engages
        S_STREAM,               // walking the line against the L2 handshake
        S_DRAIN                 // last registered beat draining to the L1
    } state_t;

    state_t state;

    logic line_req;
    assign line_req = i_re && i_cacheable;

    logic [31:OFFSET_BITS] base_q;      // line-aligned physical base
    logic [WORD_BITS-1:0]  word_q;      // word currently requested

    // One word completes downstream this cycle (data valid on i_l2_rdata).
    logic beat, last_word, beat_fault;
    assign beat       = (state == S_STREAM) && !i_l2_busy;
    assign last_word  = (word_q == WORD_BITS'(LINE_WORDS - 1));
    // A faulting beat aborts the whole line: no more words are requested and
    // the L1 is told to drop the fill (o_fill_fault) instead of installing it.
    assign beat_fault = beat && i_l2_fault;

    // ── Engage / walk ─────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state <= S_IDLE;
        end else begin
            case (state)
                S_IDLE: if (line_req) begin
                    state  <= S_STREAM;
                    base_q <= i_addr[31:OFFSET_BITS];
                    word_q <= '0;
                end
                S_STREAM: if (beat) begin
                    // A fault aborts exactly like reaching the last word — the
                    // transaction ends. The fill outputs are registered (below),
                    // so the last word reaches the L1 one cycle after its beat;
                    // the port drains through S_DRAIN before idling, staying busy
                    // so the still-asserted line request cannot re-engage.
                    if (beat_fault || last_word) state  <= S_DRAIN;
                    else                         word_q <= word_q + 1'b1;
                end
                S_DRAIN: state <= S_IDLE;
                default: state <= S_IDLE;
            endcase
        end
    end

    // ── Downstream drive ──────────────────────────────────────────
    // Pass-through forwards the single-beat shape verbatim. A line
    // request drives nothing during its engage cycle (the walk starts at
    // the edge); the stream then owns the port with cacheable reads, so
    // the L2 installs the line on its own miss as the words pass.
    always_comb begin
        o_l2_addr      = i_addr;
        o_l2_wdata     = i_wdata;
        o_l2_byte_en   = i_byte_en;
        o_l2_re        = i_re && !i_cacheable;
        o_l2_we        = i_we;
        o_l2_cacheable = i_cacheable;
        if (state == S_STREAM) begin
            o_l2_addr      = {base_q, word_q, 2'b00};
            o_l2_re        = 1'b1;
            o_l2_we        = 1'b0;
            o_l2_cacheable = 1'b1;
        end
    end

    // ── Fill stream + upstream response ───────────────────────────
    // The fill stream to the L1 is registered: each beat's {we, word, wdata,
    // done, fault} lands at the L1 one cycle after the beat. This is the timing
    // cut. i_l2_busy is combinational off the L2 tag compare; it gated o_fill_*
    // every beat, and o_fill_done/o_fill_we gated the L1's valid-bit and array
    // writes — so the L2 verdict reached the L1 write enables across the whole
    // fill layer. Registering ends that path at these flops; the L1 writes from
    // registered inputs. Cost: +1 cycle per line fill, pipelined — the per-beat
    // rate and throughput are unchanged. A faulting beat carries no data and
    // ends the transfer via o_fill_fault, not o_fill_done — the L1 aborts on it.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_fill_we    <= 1'b0;
            o_fill_done  <= 1'b0;
            o_fill_fault <= 1'b0;
        end else begin
            o_fill_we    <= beat && !beat_fault;
            o_fill_word  <= word_q;
            o_fill_wdata <= i_l2_rdata;
            o_fill_done  <= beat && last_word && !beat_fault;
            o_fill_fault <= beat_fault;
        end
    end

    assign o_rdata = i_l2_rdata;
    // Busy holds through S_DRAIN so the registered last word reaches the L1
    // before the still-asserted line request can re-engage a new stream.
    assign o_busy  = (state == S_STREAM) || (state == S_DRAIN) || line_req || i_l2_busy;
    // Pass-through fault only: a line's fault leaves via o_fill_fault, so the
    // beat-completion fault is reported solely on the single-beat path.
    assign o_fault = i_l2_fault && (state == S_IDLE) && !line_req;

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // The line request is held level for the whole transaction — the
    // atomicity invariant every layer of this interface relies on. It must
    // still be asserted through S_DRAIN, when the registered last word lands.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_STREAM || state == S_DRAIN) |-> (i_re && i_cacheable))
        else $error("fill_sequencer: line request released mid-stream");

    // Back-to-back beats are impossible with the registered advance: the
    // cycle after a beat presents the *next* address, which the L2 cannot
    // have answered yet. Two beats in a row mean the downstream violated
    // drop-equals-valid for the presented request.
    assert property (@(posedge i_clk) disable iff (i_rst)
        beat |=> !beat)
        else $error("fill_sequencer: consecutive beats — downstream answered an unpresented address");

    // The walk never escapes the line.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_STREAM) |-> (o_l2_addr[31:OFFSET_BITS] == base_q))
        else $error("fill_sequencer: stream address left the requested line");

endmodule
