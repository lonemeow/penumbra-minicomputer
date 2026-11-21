// penumbra3_bus_master -- the gen3 CPU<->bus transaction decouple.
//
// The single external-bus master for the gen3 core. It serves two
// transaction classes from the on-chip memory hierarchy, both as
// registered transactions:
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
// Three properties define this boundary:
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
//  3. Writes stream back-to-back. While a single-beat write is on the
//     bus the master takes one more behind it (o_behind_ready) and puts it
//     on the bus the cycle after the first one's ack, the strobe held: a
//     strobe held past an ack is the next access (doc/hardware/
//     bus-protocol.md, "Sync handshake"). A slave sees the writes as one
//     burst, and the SDRAM adapter takes each without passing through
//     idle. The write behind waits, pending, in registers of its own, so
//     the bus's ack only moves it onto the bus: nothing outside the
//     master hangs off the ack.
//
// One transaction is on the bus at a time. o_req_ready is asserted while
// idle, which is the cycle each completion is presented unless a write
// goes on behind it, so the next transaction is taken as the last one
// completes and leaves the strobe low one cycle between them. Each
// completion is a one-cycle pulse the cycle after the last beat's ack,
// with that transaction's own address and fault, since the write behind
// it is already on the bus. A write taken in that cycle replaces
// o_rsp_rline at the clock edge.
module penumbra3_bus_master #(
    parameter  int LINE_BYTES = 16,
    parameter  int WORD_BYTES = 4,
    localparam int LINE_WORDS = LINE_BYTES / WORD_BYTES  // derived; not overridable
) (
    input  logic                          i_clk,
    input  logic                          i_rst,

    // ── On-chip memory-hierarchy side (transactions) ─────────────
    input  logic                          i_req_valid,
    output logic                          o_req_ready,    // idle: any request may be taken
    output logic                          o_behind_ready, // a single-beat write is on the bus with
                                                          // nothing pending: a single-beat write may
                                                          // be taken to go behind it
    input  logic                          i_req_we,       // 0 = read, 1 = write
    input  logic                          i_req_line,     // 1 = full line, 0 = single beat
    input  logic [31:0]                   i_req_addr,     // line-aligned (line) / exact (single)
    input  logic [3:0]                    i_req_byte_en,  // single-beat write lanes
    input  logic [LINE_WORDS-1:0][31:0]   i_req_wline,    // write data (single beat uses word 0)
    output logic                          o_rsp_valid,    // 1-cycle completion pulse
    output logic [LINE_WORDS-1:0][31:0]   o_rsp_rline,    // read data (single beat in word 0)
    output logic                          o_rsp_fault,    // any beat faulted
    output logic [31:0]                   o_rsp_addr,     // the completing request's i_req_addr

    // ── External device-bus master side ─────────────────────────
    output logic [31:0]                   o_bus_addr,
    output logic [31:0]                   o_bus_wdata,
    output logic [3:0]                    o_bus_byte_en,
    output logic                          o_bus_re,
    output logic                          o_bus_we,
    input  logic [31:0]                   i_bus_rdata,
    input  logic                          i_bus_ack,
    input  logic                          i_bus_fault     // valid with i_bus_ack
);

    localparam int IDX_BITS = $clog2(LINE_WORDS);
    localparam int WORD_LSB = $clog2(WORD_BYTES);

    typedef enum logic [1:0] {
        M_IDLE,   // nothing on the bus: ready for a request
        M_READ,   // streaming read beats into line_buf_q
        M_WRITE   // streaming write beats out of line_buf_q
    } state_e;

    state_e                      state_q, state_d;
    logic [31:0]                 base_addr_q;
    logic [IDX_BITS-1:0]         word_idx_q;
    logic [LINE_WORDS-1:0][31:0] line_buf_q;
    logic                        is_line_q;
    logic [3:0]                  byte_en_q;
    logic                        fault_q;

    // The write taken behind the one on the bus, waiting for it to end.
    logic                        pending_q;
    logic [31:0]                 pending_addr_q;
    logic [31:0]                 pending_wdata_q;
    logic [3:0]                  pending_byte_en_q;

    // The completion: the transaction whose last beat was acked last cycle.
    logic                        done_q;
    logic [31:0]                 done_addr_q;
    logic                        done_fault_q;

    // A beat completes on the cycle the bus acks (read data valid /
    // write committed). last_beat is the final word: the whole line for a
    // burst, word 0 for a single beat. ending is the last beat completing.
    logic [IDX_BITS-1:0] last_idx;
    logic                beat_done;
    logic                last_beat;
    logic                ending;
    assign last_idx  = is_line_q ? IDX_BITS'(LINE_WORDS - 1) : '0;
    assign beat_done = (state_q == M_READ || state_q == M_WRITE) && i_bus_ack;
    assign last_beat = (word_idx_q == last_idx);
    assign ending    = beat_done && last_beat;

    // take: the master is idle and takes a request. take_behind: a
    // single-beat write is on the bus with nothing pending, and the master
    // takes a write to go behind it. Both readies come from registers,
    // never from the ack.
    logic take, take_behind;
    assign o_req_ready    = (state_q == M_IDLE);
    assign o_behind_ready = (state_q == M_WRITE) && !is_line_q && !pending_q;
    assign take           = o_req_ready && i_req_valid;
    assign take_behind    = o_behind_ready && i_req_valid;

    // ── What goes on the bus at this edge ────────────────────────
    // The pending write goes on as the write ahead of it ends. A request
    // taken goes straight on if nothing is on the bus by then: the master
    // is idle, or the write ahead ends this cycle. Otherwise it waits,
    // pending. Only a write ending moves the pending one onto the bus, so
    // a write taken in that cycle cannot wait.
    logic start_pending, start_taken, hold_taken;
    assign start_pending = ending && pending_q;
    assign start_taken   = take || (take_behind && ending);
    assign hold_taken    = take_behind && !ending;

    // ── Next-state ───────────────────────────────────────────────
    always_comb begin
        state_d = state_q;
        if (start_pending || (start_taken && i_req_we)) state_d = M_WRITE;
        else if (start_taken)                           state_d = M_READ;
        else if (ending)                                state_d = M_IDLE;
    end

    // ── Bus master outputs ───────────────────────────────────────
    // re/we are a pure function of the burst state, so they hold across
    // the whole transfer and never gap between beats. A line burst writes
    // full words; a single beat carries its byte mask.
    always_comb begin
        o_bus_addr    = base_addr_q + (32'(word_idx_q) << WORD_LSB);
        o_bus_wdata   = line_buf_q[word_idx_q];
        o_bus_byte_en = is_line_q ? 4'hF : byte_en_q;
        o_bus_re      = (state_q == M_READ);
        o_bus_we      = (state_q == M_WRITE);
    end

    // ── Completion ───────────────────────────────────────────────
    assign o_rsp_valid = done_q;
    assign o_rsp_rline = line_buf_q;
    assign o_rsp_fault = done_fault_q;
    assign o_rsp_addr  = done_addr_q;

    // ── Sequential state ─────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state_q        <= M_IDLE;
            word_idx_q     <= '0;
            fault_q        <= 1'b0;
            base_addr_q    <= '0;
            line_buf_q     <= '0;
            is_line_q      <= 1'b0;
            byte_en_q      <= '0;
            pending_q         <= 1'b0;
            pending_addr_q    <= '0;
            pending_wdata_q   <= '0;
            pending_byte_en_q <= '0;
            done_q         <= 1'b0;
            done_addr_q    <= '0;
            done_fault_q   <= 1'b0;
        end else begin
            state_q <= state_d;

            if (start_pending) begin
                base_addr_q   <= pending_addr_q;
                word_idx_q    <= '0;
                fault_q       <= 1'b0;
                is_line_q     <= 1'b0;
                byte_en_q     <= pending_byte_en_q;
                line_buf_q[0] <= pending_wdata_q;
            end else if (start_taken) begin
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

            if (hold_taken) begin
                pending_q         <= 1'b1;
                pending_addr_q    <= i_req_addr;
                pending_wdata_q   <= i_req_wline[0];
                pending_byte_en_q <= i_req_byte_en;
            end else if (start_pending) begin
                pending_q <= 1'b0;
            end

            done_q <= ending;
            if (ending) begin
                done_addr_q  <= base_addr_q;
                done_fault_q <= fault_q | i_bus_fault;
            end
        end
    end

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst) begin
            assert (!(take_behind && !(i_req_we && !i_req_line)))
                else $error("penumbra3_bus_master: a read or a line taken behind a write");
            assert (!(pending_q && !(state_q == M_WRITE && !is_line_q)))
                else $error("penumbra3_bus_master: a write pending with no single-beat write on the bus");
            assert (!(start_pending && start_taken))
                else $error("penumbra3_bus_master: a request taken as the pending write starts");
        end
    end

endmodule
