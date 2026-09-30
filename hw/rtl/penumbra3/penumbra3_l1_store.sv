// penumbra3_l1_store -- storage for one gen3 L1 cache (sync-read BRAM).
//
// Tags, data and valid bits for a VIPT, physically tagged L1 whose ways are
// each one page: the set and word index sit inside the page offset, where
// the virtual and physical addresses agree, and a tag is exactly the frame
// number the MMU resolves. A launch reads the indexed set of every way; the
// registered tags, words and valid bits land the next cycle for
// penumbra3_l1_match. Like penumbra3_tlb_store it holds no policy:
// replacement, the enable, miss handling and the store path's ordering
// belong to the cache around it.
//
// Data is kept in word columns: each way has one block RAM per word of the
// line, so a fill installs a whole line -- tag, every word, valid -- in one
// write, and a store writes the one column its word lives in, byte lanes
// enabled. A lookup reads every column and the registered word index picks
// one, a mux that runs beside the tag compare rather than after it. Byte
// lanes read a word at a time would put that select inside the block RAM,
// but a store writes one word of a lane's line-wide row, which Vivado
// cannot build as a block RAM; columns map the same in both tools.
//
// Every array's RTL keeps its read and its write off one address in one
// cycle, so synthesis adds no read-during-write bypass after the EBRs. A
// fill never meets a launch: the pipe is frozen while one installs
// (asserted), so a fill-written array's read is simply suppressed on the
// write. A store meets launches all the time, since the cache registers a
// store hit and writes it a cycle later; only a launch reading the very
// word the store is writing is suppressed, and that way resolves invalid,
// so the lookup misses rather than returning the word from before the
// store.
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

    // ── Launch: read the indexed set of every way ────────────────
    input  logic                  i_launch,   // a real access, pipe not frozen
    input  logic [SET_BITS-1:0]   i_set,
    input  logic [WORD_BITS-1:0]  i_word,

    // ── Resolve: registered, held between launches ───────────────
    output logic [WAYS-1:0]              o_rd_valid,
    output logic [WAYS-1:0][TAG_BITS-1:0] o_rd_tag,
    output logic [WAYS-1:0][31:0]        o_rd_word,   // the launched word, per way

    // ── Line install: tag, every word and valid, one cycle ───────
    input  logic                         i_fill_we,
    input  logic [SET_BITS-1:0]          i_fill_set,
    input  logic [WAY_BITS-1:0]          i_fill_way,
    input  logic [TAG_BITS-1:0]          i_fill_tag,
    input  logic [LINE_WORDS-1:0][31:0]  i_fill_line,

    // ── Store write: one word of one way ─────────────────────────
    input  logic                  i_store_we,
    input  logic [SET_BITS-1:0]   i_store_set,
    input  logic [WORD_BITS-1:0]  i_store_word,
    input  logic [WAY_BITS-1:0]   i_store_way,
    input  logic [31:0]           i_store_wdata,
    input  logic [3:0]            i_store_byte_en,

    input  logic                  i_inval_all   // clear every valid bit
);

    build_check #(
        .COND (LINE_WORDS >= 2 && (1 << WORD_BITS) == LINE_WORDS),
        .MSG  ("penumbra3_l1_store: LINE_BYTES must be a power-of-two number of words, at least two")
    ) u_check_line ();

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
    // launch's set, so a launch reading that word is suppressed, and the
    // way resolves invalid instead of holding the word from before the
    // store.
    logic [WAYS-1:0][LINE_WORDS-1:0] data_rd_block;
    always_comb
        for (int w = 0; w < WAYS; w++)
            for (int c = 0; c < LINE_WORDS; c++)
                data_rd_block[w][c] = store_we[w][c] && i_store_set == i_set;

    // A way whose launched word could not be read resolves as invalid.
    logic [WAYS-1:0] word_blocked;
    always_comb
        for (int w = 0; w < WAYS; w++)
            word_blocked[w] = data_rd_block[w][i_word];

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

    logic [WORD_BITS-1:0] launch_word_q;
    always_ff @(posedge i_clk) begin
        if (i_launch) begin
            launch_word_q <= i_word;
            for (int w = 0; w < WAYS; w++)
                o_rd_valid[w] <= valid_q[w][i_set] && !word_blocked[w];
        end
        if (i_rst) o_rd_valid <= '0;
    end

    // ── Per-way arrays: one block RAM for the tags, one per data column ─
    for (genvar gw = 0; gw < WAYS; gw++) begin : g_way
        (* ram_style = "block" *) logic [TAG_BITS-1:0] tag_mem [SETS];
        logic [TAG_BITS-1:0] tag_q;

        always_ff @(posedge i_clk) begin
            if (i_launch && !tag_we[gw]) tag_q <= tag_mem[i_set];
            if (tag_we[gw])              tag_mem[i_fill_set] <= i_fill_tag;
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
            logic [31:0] word_q;

            // One write port, shared: the fill writes the whole word, a
            // store its enabled bytes. The two never coincide (asserted).
            logic                data_rd_en;
            logic [SET_BITS-1:0] wr_set;
            logic [31:0]         wr_data;
            logic [3:0]          wr_be;
            assign data_rd_en = i_launch && !tag_we[gw] && !data_rd_block[gw][gc];
            always_comb begin
                if (tag_we[gw]) begin
                    wr_set  = i_fill_set;
                    wr_data = i_fill_line[gc];
                    wr_be   = 4'b1111;
                end else begin
                    wr_set  = i_store_set;
                    wr_data = i_store_wdata;
                    wr_be   = store_we[gw][gc] ? i_store_byte_en : 4'b0000;
                end
            end

            always_ff @(posedge i_clk) begin
                if (data_rd_en)
                    word_q <= data_mem[i_set];
                if (wr_be[0]) data_mem[wr_set][ 7: 0] <= wr_data[ 7: 0];
                if (wr_be[1]) data_mem[wr_set][15: 8] <= wr_data[15: 8];
                if (wr_be[2]) data_mem[wr_set][23:16] <= wr_data[23:16];
                if (wr_be[3]) data_mem[wr_set][31:24] <= wr_data[31:24];
            end
            assign col_word[gc] = word_q;

            // The invariant no_rw_check assumes: the column is never read
            // and written at one set in one cycle.
            always_comb
                assert (i_rst || !(data_rd_en && (|wr_be) && wr_set == i_set))
                    else $error("penumbra3_l1_store: a data read met a write at one address");
        end

        assign o_rd_word[gw] = col_word[launch_word_q];
    end

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // The cache freezes its pipe while a fill installs, so no lookup is
    // launching to have its read suppressed.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_fill_we && i_launch))
        else $error("penumbra3_l1_store: lookup launched on a fill install");

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_fill_we && i_store_we))
        else $error("penumbra3_l1_store: store write collided with a fill");

endmodule
