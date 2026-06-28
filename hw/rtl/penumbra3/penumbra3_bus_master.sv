// penumbra3_bus_master -- the gen3 CPU<->bus transaction decouple.
//
// The single external-bus master for the gen3 core. It serves two
// transaction classes from the on-chip memory hierarchy, both as
// registered, single-outstanding transactions:
//
//   • Line burst  (i_req_line=1): a whole cacheable line -- a read fill
//     or a write-back eviction. LINE_WORDS beats, full words.
//   • Single beat (i_req_line=0): one uncached access, or a cacheable
//     store write-through. One beat, carrying i_req_byte_en so a sub-word
//     store writes only its lanes. A cacheable load never arrives here --
//     sub-word included -- since a hit is served from the cache and a miss
//     takes the line path above; every read pulls a full word and the core
//     extracts the byte locally.
//
// Cacheability is decided upstream; the master only sees the mechanical
// shape of the transfer, so a single beat is just a length-1 burst.
//
// Two properties make this the gen3 boundary rather than gen2's:
//
//  1. Registered transaction boundary. The request is latched once on
//     accept; the response word/line and any fault are presented from
//     registers. Cost is +1 latency per transaction, never a per-beat
//     bubble.
//
//  2. Beats stream back-to-back. o_bus_re / o_bus_we are a pure function
//     of the burst state, so they hold across the whole transfer and
//     never drop between words. The SDRAM adapter speculates the next
//     word (addr+WORD_BYTES) on every accepted read; a gap in the stream
//     orphans that speculation, so holding re unbroken is what keeps the
//     speculation chain alive across a line.
//
// Single-outstanding: one transaction at a time (the burst is the only
// concurrency). o_req_ready is asserted only while idle.
module penumbra3_bus_master #(
    parameter  int LINE_BYTES = 16,
    parameter  int WORD_BYTES = 4,
    localparam int LINE_WORDS = LINE_BYTES / WORD_BYTES  // derived; not overridable
) (
    input  logic                          i_clk,
    input  logic                          i_rst,

    // ── On-chip memory-hierarchy side (transactions) ─────────────
    input  logic                          i_req_valid,
    output logic                          o_req_ready,
    input  logic                          i_req_we,       // 0 = read, 1 = write
    input  logic                          i_req_line,     // 1 = full line, 0 = single beat
    input  logic [31:0]                   i_req_addr,     // line-aligned (line) / exact (single)
    input  logic [3:0]                    i_req_byte_en,  // single-beat write lanes
    input  logic [LINE_WORDS-1:0][31:0]   i_req_wline,    // write data (single beat uses word 0)
    output logic                          o_rsp_valid,    // 1-cycle completion pulse
    output logic [LINE_WORDS-1:0][31:0]   o_rsp_rline,    // read data (single beat in word 0)
    output logic                          o_rsp_fault,    // any beat faulted

    // ── External device-bus master side ─────────────────────────
    output logic [31:0]                   o_bus_addr,
    output logic [31:0]                   o_bus_wdata,
    output logic [3:0]                    o_bus_byte_en,
    output logic                          o_bus_re,
    output logic                          o_bus_we,
    input  logic [31:0]                   i_bus_rdata,
    input  logic                          i_bus_busy,
    input  logic                          i_bus_fault     // coincident with the busy drop
);

    localparam int IDX_BITS = $clog2(LINE_WORDS);
    localparam int WORD_LSB = $clog2(WORD_BYTES);

    typedef enum logic [1:0] {
        M_IDLE,   // ready for a request
        M_READ,   // streaming read beats into line_buf_q
        M_WRITE,  // streaming write beats out of line_buf_q
        M_DONE    // present the completion for one cycle
    } state_e;

    state_e                      state_q, state_d;
    logic [31:0]                 base_addr_q;
    logic [IDX_BITS-1:0]         word_idx_q;
    logic [LINE_WORDS-1:0][31:0] line_buf_q;
    logic                        is_line_q;
    logic [3:0]                  byte_en_q;
    logic                        fault_q;

    // A beat completes on the cycle the bus drops busy (read data valid /
    // write committed). last_beat is the final word: the whole line for a
    // burst, word 0 for a single beat.
    logic [IDX_BITS-1:0] last_idx;
    logic                beat_done;
    logic                last_beat;
    assign last_idx  = is_line_q ? IDX_BITS'(LINE_WORDS - 1) : '0;
    assign beat_done = (state_q == M_READ || state_q == M_WRITE) && !i_bus_busy;
    assign last_beat = (word_idx_q == last_idx);

    // ── Next-state ───────────────────────────────────────────────
    always_comb begin
        state_d = state_q;
        case (state_q)
            M_IDLE:  if (i_req_valid)            state_d = i_req_we ? M_WRITE : M_READ;
            M_READ:  if (beat_done && last_beat) state_d = M_DONE;
            M_WRITE: if (beat_done && last_beat) state_d = M_DONE;
            M_DONE:                              state_d = M_IDLE;
            default:                             state_d = M_IDLE;
        endcase
    end

    // ── Bus master outputs ───────────────────────────────────────
    // re/we are a pure function of the burst state, so they hold across
    // the whole transfer and never gap on busy. A line burst writes full
    // words; a single beat carries its byte mask.
    always_comb begin
        o_bus_addr    = base_addr_q + (32'(word_idx_q) << WORD_LSB);
        o_bus_wdata   = line_buf_q[word_idx_q];
        o_bus_byte_en = is_line_q ? 4'hF : byte_en_q;
        o_bus_re      = (state_q == M_READ);
        o_bus_we      = (state_q == M_WRITE);
    end

    // ── Completion ───────────────────────────────────────────────
    assign o_req_ready = (state_q == M_IDLE);
    assign o_rsp_valid = (state_q == M_DONE);
    assign o_rsp_rline = line_buf_q;
    assign o_rsp_fault = fault_q;

    // ── Sequential state ─────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q     <= M_IDLE;
            word_idx_q  <= '0;
            fault_q     <= 1'b0;
            base_addr_q <= '0;
            line_buf_q  <= '0;
            is_line_q   <= 1'b0;
            byte_en_q   <= '0;
        end else begin
            state_q <= state_d;

            if (state_q == M_IDLE && i_req_valid) begin
                base_addr_q <= i_req_addr;
                word_idx_q  <= '0;
                fault_q     <= 1'b0;
                is_line_q   <= i_req_line;
                byte_en_q   <= i_req_byte_en;
                if (i_req_we) line_buf_q <= i_req_wline;
            end else if (beat_done) begin
                if (state_q == M_READ) line_buf_q[word_idx_q] <= i_bus_rdata;
                fault_q <= fault_q | i_bus_fault;
                if (!last_beat) word_idx_q <= word_idx_q + 1'b1;
            end
        end
    end

    // The bus contract presents a fault only as busy drops, and fault_q is
    // captured on that edge -- a fault raised while still busy would be
    // missed. Catch a producer that violates the contract.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state_q == M_READ || state_q == M_WRITE) |-> !(i_bus_busy && i_bus_fault))
        else $error("penumbra3_bus_master: bus fault asserted while busy");

endmodule
