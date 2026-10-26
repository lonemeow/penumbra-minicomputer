// penumbra3_l1_store -- storage for one gen3 L1 cache (sync-read BRAM).
//
// Tags, data and valid bits for a VIPT, physically tagged L1 whose ways are
// each one page: the set and word index sit inside the page offset, where
// the virtual and physical addresses agree, and a tag is exactly the frame
// number the MMU resolves. A lookup reads the indexed set of every way; the
// registered tags, words and valid bits land the next cycle for
// penumbra3_l1_match. Like penumbra3_tlb_store it holds no policy:
// replacement, the enable, miss handling and the store path's ordering
// belong to the cache around it.
//
// Data is kept in word columns: each way has one block RAM per word of the
// line, so a fill installs a whole line -- tag, every word, valid -- in one
// write, and a store writes the one column its word lives in. A lookup
// reads every column and the registered word index picks one, a mux that
// runs beside the tag compare rather than after it. Byte lanes read a word
// at a time would put that select inside the block RAM, but a store writes
// one word of a lane's line-wide row, which Vivado cannot build as a block
// RAM; columns map the same in both tools. Every write is a whole word: the
// cache merges a store's bytes into the word its own lookup read, so the
// write port needs no byte enables.
//
// The arrays are read on every clock edge. A launch reads its own address
// and makes its lookup the current one; every edge without a launch reads
// the current lookup's address again. So the outputs always belong to the
// current lookup and show the arrays as the last edge found them: a lookup
// its consumer holds follows an invalidate or a line installed under it,
// with no strobe to ask for it.
//
// The cache writes a store hit a cycle after the store's own lookup, and a
// written word reaches a block RAM's output two cycles after the cycle of
// its write. A lookup of that word that is current before then is given it
// by a mux after the column select: from the store port in the cycle of
// the write, and from a register one cycle behind the port in the next.
// The mux's select compares registers, so it settles before the RAM output
// arrives, and the tag compare outlasts the mux.
//
// No array is read and written at one address in one cycle, so synthesis
// adds no read-during-write bypass after the EBRs. A fill gates off every
// read of the way it writes, at any address, and no lookup launches on a
// fill (asserted). What that costs is one cycle: in the cycle after a fill
// the filled way's outputs are out of date, the cache tells its consumer
// so, and the line shows from the cycle after that. A store meets reads
// all the time; only the read of the very word the store is writing is
// suppressed, and the register behind the store port stands in for it.
module penumbra3_l1_store #(
    parameter  int WAYS       = 2,
    parameter  int LINE_BYTES = 16,
    // Derived; not overridable. A way is one 4 KB page, and the tag is
    // the 20-bit frame number.
    localparam int LINE_WORDS = LINE_BYTES / 4,
    localparam int WORD_BITS  = $clog2(LINE_WORDS),
    localparam int SETS       = 4096 / LINE_BYTES,
    localparam int SET_BITS   = $clog2(SETS),
    localparam int WAY_BITS   = (WAYS > 1) ? $clog2(WAYS) : 1,
    localparam int TAG_BITS   = 20
) (
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── Launch: a new lookup, of the indexed set of every way ────
    input  logic                  i_launch,     // a real access, pipe not frozen
    input  logic [SET_BITS-1:0]   i_set,
    input  logic [WORD_BITS-1:0]  i_word,
    input  logic                  i_lookup_en,  // 0: every way reads invalid

    // ── Resolve: registered; the current lookup's, read every cycle ─
    output logic [SET_BITS-1:0]           o_rd_set,      // the current lookup's set and word
    output logic [WORD_BITS-1:0]          o_rd_word_idx,
    output logic [WAYS-1:0]               o_rd_valid,
    output logic [WAYS-1:0][TAG_BITS-1:0] o_rd_tag,
    output logic [WAYS-1:0][31:0]         o_rd_word,     // per way; a store's word from the cycle of its write

    // ── Line install: tag, every word and valid, one cycle ───────
    input  logic                         i_fill_we,
    input  logic [SET_BITS-1:0]          i_fill_set,
    input  logic [WAY_BITS-1:0]          i_fill_way,
    input  logic [TAG_BITS-1:0]          i_fill_tag,
    input  logic [LINE_WORDS-1:0][31:0]  i_fill_line,

    // ── Store write: one whole word of one way ───────────────────
    // i_store_we is high for one cycle per store, and the write takes the
    // clock edge that ends it. The other four are meaningful in that cycle
    // only.
    input  logic                  i_store_we,
    input  logic [SET_BITS-1:0]   i_store_set,
    input  logic [WORD_BITS-1:0]  i_store_word,
    input  logic [WAY_BITS-1:0]   i_store_way,
    input  logic [31:0]           i_store_wdata,

    input  logic                  i_inval_all   // clear every valid bit
);

    build_check #(
        .COND (LINE_WORDS >= 2 && (1 << WORD_BITS) == LINE_WORDS),
        .MSG  ("penumbra3_l1_store: LINE_BYTES must be a power-of-two number of words, at least two")
    ) u_check_line ();

    // ── The read ─────────────────────────────────────────────────
    // rd_set / rd_word is the address this cycle's clock edge reads: a
    // launch's own, else the current lookup's again. cur_set_q /
    // cur_word_q is the address the last edge read, so the current
    // lookup's, which the outputs belong to.
    logic [SET_BITS-1:0]  cur_set_q,  rd_set;
    logic [WORD_BITS-1:0] cur_word_q, rd_word;
    assign rd_set  = i_launch ? i_set  : cur_set_q;
    assign rd_word = i_launch ? i_word : cur_word_q;

    always_ff @(posedge i_clk)
        if (i_launch) begin
            cur_set_q  <= i_set;
            cur_word_q <= i_word;
        end
    assign o_rd_set      = cur_set_q;
    assign o_rd_word_idx = cur_word_q;

    // ── Tag-array exclusivity: only a fill writes the tags ───────
    logic [WAYS-1:0] tag_we;
    always_comb
        for (int w = 0; w < WAYS; w++)
            tag_we[w] = i_fill_we && (i_fill_way == WAY_BITS'(w));

    // ── The store write, by way and word ─────────────────────────
    logic [WAYS-1:0][LINE_WORDS-1:0] store_we;
    always_comb
        for (int w = 0; w < WAYS; w++)
            for (int c = 0; c < LINE_WORDS; c++)
                store_we[w][c] = i_store_we && (i_store_way == WAY_BITS'(w))
                              && (i_store_word == WORD_BITS'(c));

    // data_rd_block[w][c]: this cycle's store writes word c of way w at the
    // read's set, so the read of that word is suppressed.
    logic [WAYS-1:0][LINE_WORDS-1:0] data_rd_block;
    always_comb
        for (int w = 0; w < WAYS; w++)
            for (int c = 0; c < LINE_WORDS; c++)
                data_rd_block[w][c] = store_we[w][c] && i_store_set == rd_set;

    // ── A store's word, ahead of the RAM ─────────────────────────
    // A store write has its word W on the store port for one cycle, c,
    // and the RAM takes it at the edge that ends c. A read of that word
    // at that same edge is the suppressed one, so the first read to
    // return W is an edge later, and W is on the RAM's output from c+2.
    // A lookup of that word that is current in c or in c+1 is given W by
    // the output mux:
    //
    //   cycle              c               c+1             c+2
    //   store port         W
    //   the RAM            W written       W read          W on its output
    //   W comes from       i_store_wdata   fwd_word_q      the RAM
    //   for way w when     store_now[w]    fwd_valid_q[w]
    //
    //   store_now[w]   The write is to way w of the current lookup's word:
    //                  this cycle's outputs come from a read before it.
    //   store_next[w]  The write is to way w of the word this edge reads,
    //                  which is the current lookup's word in the next
    //                  cycle, whether that is the same lookup or one
    //                  launching now. That read is the suppressed one.
    logic [WAYS-1:0] store_now, store_next;
    always_comb
        for (int w = 0; w < WAYS; w++) begin
            store_now[w]  = i_store_we && (i_store_way == WAY_BITS'(w))
                         && i_store_set == cur_set_q && i_store_word == cur_word_q;
            store_next[w] = data_rd_block[w][rd_word];
        end

    // fwd_valid_q[w]: the last cycle's store wrote way w of the current
    // lookup's word, and fwd_word_q is the word it wrote. Both follow the
    // store port by one cycle and say nothing about any earlier one, so
    // one word serves every way (a cycle has one store write) and neither
    // needs a reset.
    logic [WAYS-1:0] fwd_valid_q;
    logic [31:0]     fwd_word_q;
    always_ff @(posedge i_clk) begin
        fwd_valid_q <= store_next;
        fwd_word_q  <= i_store_wdata;
    end

    // ── Valid bits: resettable flops (BRAM is indeterminate at reset) ─
    // The invalidate is last so it wins over a same-cycle install.
    logic [WAYS-1:0][SETS-1:0] valid_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            valid_q <= '0;
        end else begin
            for (int w = 0; w < WAYS; w++)
                if (tag_we[w]) valid_q[w][i_fill_set] <= 1'b1;
            if (i_inval_all) valid_q <= '0;
        end
    end

    always_ff @(posedge i_clk) begin
        for (int w = 0; w < WAYS; w++)
            o_rd_valid[w] <= valid_q[w][rd_set] && i_lookup_en;
        if (i_rst) o_rd_valid <= '0;
    end

    // ── Per-way arrays: one block RAM for the tags, one per data column ─
    for (genvar gw = 0; gw < WAYS; gw++) begin : g_way
        (* ram_style = "block" *) logic [TAG_BITS-1:0] tag_mem [SETS];
        logic [TAG_BITS-1:0] tag_q;

        always_ff @(posedge i_clk) begin
            if (!tag_we[gw]) tag_q <= tag_mem[rd_set];
            if (tag_we[gw])  tag_mem[i_fill_set] <= i_fill_tag;
        end
        assign o_rd_tag[gw] = tag_q;

        // The store's read suppression compares addresses, which yosys
        // cannot credit: it proves exclusivity only between enables, and
        // would otherwise emulate read-first with a delayed write and a mux
        // on the EBR output. no_rw_check tells it the collision cannot
        // happen; the assertion in each column checks exactly that, on the
        // same enables and addresses.
        logic [LINE_WORDS-1:0][31:0] col_word;
        for (genvar gc = 0; gc < LINE_WORDS; gc++) begin : g_col
            (* ram_style = "block", no_rw_check *)
            logic [31:0] data_mem [SETS];
            logic [31:0] col_q;

            // One write port, shared: a fill or a store, never both
            // (asserted), each a whole word.
            logic                data_rd_en, wr_en;
            logic [SET_BITS-1:0] wr_set;
            logic [31:0]         wr_data;
            assign data_rd_en = !tag_we[gw] && !data_rd_block[gw][gc];
            assign wr_en      = tag_we[gw] || store_we[gw][gc];
            assign wr_set     = tag_we[gw] ? i_fill_set     : i_store_set;
            assign wr_data    = tag_we[gw] ? i_fill_line[gc] : i_store_wdata;

            always_ff @(posedge i_clk) begin
                if (data_rd_en) col_q <= data_mem[rd_set];
                if (wr_en)      data_mem[wr_set] <= wr_data;
            end
            assign col_word[gc] = col_q;

            // The invariant no_rw_check assumes: the column is never read
            // and written at one set in one cycle.
            always_comb
                assert (i_rst || !(data_rd_en && wr_en && wr_set == rd_set))
                    else $error("penumbra3_l1_store: a data read met a write at one address");
        end

        // store_now goes first: with both set, two stores wrote this
        // word in consecutive cycles, and the store port has the later
        // one. store_now compares the store port, which the cache drives
        // from registers, against registers, so both selects settle
        // before the RAM's output arrives.
        always_comb begin
            if (store_now[gw])        o_rd_word[gw] = i_store_wdata;
            else if (fwd_valid_q[gw]) o_rd_word[gw] = fwd_word_q;
            else                      o_rd_word[gw] = col_word[cur_word_q];
        end
    end

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // The cache freezes its pipe while a fill installs, so no lookup
    // launches on a fill's edge.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_fill_we && i_launch))
        else $error("penumbra3_l1_store: lookup launched on a fill install");

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_fill_we && i_store_we))
        else $error("penumbra3_l1_store: store write collided with a fill");

endmodule
