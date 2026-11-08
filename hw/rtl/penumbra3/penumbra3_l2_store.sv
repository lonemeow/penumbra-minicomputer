// penumbra3_l2_store -- storage for the gen3 L2 (sync-read block RAM).
//
// The four arrays doc/internals/penumbra3/uncore.md ("Geometry") lists:
// each way's tags and data, a set's valid bits in one row, and the
// replacement tree, a bit of it to an array. Like penumbra3_l1_store it
// holds no policy: which way a line goes to, what the bits mean and when
// anything is written belong to the uncore around it.
//
// Data is kept in word columns, one block RAM per word of each way's line:
// an install writes every column of its way, and a write hit one column,
// with the whole word, which the uncore merges from the word its own
// lookup read. That shape maps the same in yosys and Vivado, where a
// line-wide row written a word at a time does not.
//
// Reads happen only when asked for, and each output holds the last read
// until the next. Every array gates its reads off in a cycle that writes
// it, so no block RAM carries a read-during-write bypass. The uncore never
// asks for a read in such a cycle (asserted), so the gate never costs a
// read it wanted.
module penumbra3_l2_store #(
    parameter  int SETS       = 1024,
    parameter  int WAYS       = 4,
    parameter  int LINE_BYTES = 16,
    localparam int LINE_WORDS = LINE_BYTES / 4,
    localparam int WORD_BITS  = $clog2(LINE_WORDS),
    localparam int SET_BITS   = $clog2(SETS),
    localparam int WAY_BITS   = $clog2(WAYS),
    localparam int TAG_BITS   = 32 - SET_BITS - $clog2(LINE_BYTES),
    localparam int TREE_BITS  = WAYS - 1
) (
    input  logic                                   i_clk,
    input  logic                                   i_rst,

    // ── Lookup read: every way's tag and line, and the set's valid row ─
    input  logic                                   i_rd_en,
    input  logic [SET_BITS-1:0]                    i_rd_set,
    output logic [WAYS-1:0][TAG_BITS-1:0]          o_rd_tag,
    output logic [WAYS-1:0][LINE_WORDS-1:0][31:0]  o_rd_line,
    output logic [WAYS-1:0]                        o_rd_valid,

    // ── Install: one way's tag and line ──────────────────────────
    input  logic                                   i_fill_we,
    input  logic [SET_BITS-1:0]                    i_fill_set,
    input  logic [WAY_BITS-1:0]                    i_fill_way,
    input  logic [TAG_BITS-1:0]                    i_fill_tag,
    input  logic [LINE_WORDS-1:0][31:0]            i_fill_line,

    // ── Word write: one whole word of one way ────────────────────
    input  logic                                   i_word_we,
    input  logic [SET_BITS-1:0]                    i_word_set,
    input  logic [WAY_BITS-1:0]                    i_word_way,
    input  logic [WORD_BITS-1:0]                   i_word_idx,
    input  logic [31:0]                            i_word_data,

    // ── Valid row write: a set's bits, whole ─────────────────────
    input  logic                                   i_valid_we,
    input  logic [SET_BITS-1:0]                    i_valid_set,
    input  logic [WAYS-1:0]                        i_valid_row,

    // ── Replacement tree: read whole, written by bit ─────────────
    input  logic                                   i_tree_rd_en,
    input  logic [SET_BITS-1:0]                    i_tree_rd_set,
    output logic [TREE_BITS-1:0]                   o_tree,
    input  logic [TREE_BITS-1:0]                   i_tree_we,
    input  logic [SET_BITS-1:0]                    i_tree_wr_set,
    input  logic [TREE_BITS-1:0]                   i_tree_wr_bits
);

    build_check #(
        .COND (WAYS >= 2 && (1 << WAY_BITS) == WAYS && (1 << SET_BITS) == SETS),
        .MSG  ("penumbra3_l2_store: SETS, WAYS powers of two; WAYS at least two")
    ) u_check_geometry ();

    build_check #(
        .COND (LINE_WORDS >= 2 && (1 << WORD_BITS) == LINE_WORDS),
        .MSG  ("penumbra3_l2_store: a line must be two or more words, a power of two")
    ) u_check_line ();

    typedef logic [WAY_BITS-1:0]  way_t;
    typedef logic [WORD_BITS-1:0] word_t;

    // ── Per-way arrays: one block RAM for the tags, one per data column ─
    for (genvar gw = 0; gw < WAYS; gw++) begin : g_way
        logic fill_here;
        assign fill_here = i_fill_we && (i_fill_way == way_t'(gw));

        (* ram_style = "block" *) logic [TAG_BITS-1:0] tag_mem [SETS];
        logic [TAG_BITS-1:0] tag_q;

        always_ff @(posedge i_clk) begin
            if (i_rd_en && !fill_here) tag_q <= tag_mem[i_rd_set];
            if (fill_here)             tag_mem[i_fill_set] <= i_fill_tag;
        end
        assign o_rd_tag[gw] = tag_q;

        for (genvar gc = 0; gc < LINE_WORDS; gc++) begin : g_col
            (* ram_style = "block" *) logic [31:0] data_mem [SETS];
            logic [31:0] col_q;

            // One write port, shared: an install or a word write, never
            // both (asserted).
            logic                wr_en;
            logic [SET_BITS-1:0] wr_set;
            logic [31:0]         wr_data;
            always_comb begin
                wr_en = fill_here
                     || (i_word_we && (i_word_way == way_t'(gw))
                                   && (i_word_idx == word_t'(gc)));
                if (fill_here) begin
                    wr_set  = i_fill_set;
                    wr_data = i_fill_line[gc];
                end else begin
                    wr_set  = i_word_set;
                    wr_data = i_word_data;
                end
            end

            always_ff @(posedge i_clk) begin
                if (i_rd_en && !wr_en) col_q <= data_mem[i_rd_set];
                if (wr_en)             data_mem[wr_set] <= wr_data;
            end
            assign o_rd_line[gw][gc] = col_q;
        end
    end

    // ── Valid rows ───────────────────────────────────────────────
    (* ram_style = "block" *) logic [WAYS-1:0] valid_mem [SETS];
    logic [WAYS-1:0] valid_q;

    always_ff @(posedge i_clk) begin
        if (i_rd_en && !i_valid_we) valid_q <= valid_mem[i_rd_set];
        if (i_valid_we)             valid_mem[i_valid_set] <= i_valid_row;
    end
    assign o_rd_valid = valid_q;

    // ── Replacement tree: an array a bit ─────────────────────────
    for (genvar gb = 0; gb < TREE_BITS; gb++) begin : g_tree
        (* ram_style = "block" *) logic tree_mem [SETS];
        logic tree_q;

        always_ff @(posedge i_clk) begin
            if (i_tree_rd_en && !i_tree_we[gb])
                tree_q <= tree_mem[i_tree_rd_set];
            if (i_tree_we[gb])
                tree_mem[i_tree_wr_set] <= i_tree_wr_bits[gb];
        end
        assign o_tree[gb] = tree_q;
    end

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // The gates above never suppress a read the uncore asked for.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_rd_en && (i_fill_we || i_word_we || i_valid_we)))
        else $error("penumbra3_l2_store: a lookup read met a write");

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_tree_rd_en && (|i_tree_we)))
        else $error("penumbra3_l2_store: a replacement read met a touch");

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_fill_we && i_word_we))
        else $error("penumbra3_l2_store: a word write met an install");

endmodule
