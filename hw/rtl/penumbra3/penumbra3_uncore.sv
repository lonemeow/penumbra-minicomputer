// penumbra3_uncore -- the gen3 uncore: both L1s' requests through the L2 and
// onto the external bus.
//
// doc/internals/penumbra3/uncore.md specifies it: the select, the L2's
// lookup and penumbra3_bus_master as one pipeline. The arrays are
// penumbra3_l2_store's; this module is the pipeline's control, the record
// of the bus transaction, the L2's cache device (sysreg device 9) and its
// counters.
//
// Cacheable stores reach it through penumbra3_store_buffer, not a side's
// request: a store completes as it enters the buffer, and the buffer is
// the select's third requester. Its oldest queued store issues as a post,
// which writes its bytes into the L2 on a hit and leaves the store owing
// its bus write; the bus master takes the posted stores, oldest first,
// ahead of any bus transaction. The sides' operations keep the buffer's
// order at the verdict: a line read waits while a queued store holds its
// line, a word access until the buffer is empty, and either reaches the
// bus only once no posted store is left.
//
// For an operation issuing in cycle i:
//
//   i     issue    the select takes a request; a lookup reads its set
//   i+1   verdict  the tag compare; the operation completes, posts, takes
//                  the bus, or is dropped and its requester issues it again
//   i+2   update   the requester's done; a write hit's word and the
//                  replacement touch, from registers
//
// A bus transaction goes from the verdict straight into the bus master, and
// is in flight from the next cycle through its done, which comes a cycle
// after the bus master completes and is the cycle its line installs.
//
// penumbra3_stream_buffer sits beside the L2: a line read the L2 misses
// that a stream buffer holds is a transaction with no bus, done at i+3, a
// cycle after a hit's, and its line installs as a bus read's would.
// The buffers' prefetches run on the bus master only when the bus has
// nothing else to do, and are not transactions: the L2 serves hits and
// takes posts while one runs.
module penumbra3_uncore
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
#(
    parameter  int SETS           = 1024,
    parameter  int WAYS           = 4,
    localparam int LINE_WORDS     = UNCORE_LINE_WORDS,
    localparam int LINE_BYTES     = LINE_WORDS * 4,
    localparam int OFFSET_BITS    = $clog2(LINE_BYTES),
    localparam int LINE_ADDR_BITS = 32 - OFFSET_BITS,  // a line's address: its offset dropped
    localparam int WORD_BITS      = $clog2(LINE_WORDS),
    localparam int SET_BITS       = $clog2(SETS),
    localparam int WAY_BITS       = $clog2(WAYS),
    localparam int TAG_BITS       = 32 - SET_BITS - OFFSET_BITS,
    localparam int TREE_BITS      = WAYS - 1
) (
    input  logic                         i_clk,
    input  logic                         i_rst,

    // -- Fetch side: the L1-I's request ----------------------------
    input  logic                         i_ireq_valid,
    input  logic                         i_ireq_cacheable,
    input  logic [31:0]                  i_ireq_addr,
    output logic                         o_irsp_done,

    // -- Data side: the L1-D's request -----------------------------
    input  logic                         i_dreq_valid,
    input  logic                         i_dreq_cacheable,
    input  logic [31:0]                  i_dreq_addr,
    input  logic                         i_dreq_we,
    input  logic [3:0]                   i_dreq_byte_en,
    input  logic [31:0]                  i_dreq_wdata,
    output logic                         o_drsp_done,

    // -- Store side: a cacheable store leaving MEM2 ----------------
    input  logic                         i_store_enq,        // never while o_store_full
    input  logic [31:0]                  i_store_addr,       // physical
    input  logic [31:0]                  i_store_wdata,      // each byte in its lane
    input  logic [3:0]                   i_store_byte_en,
    output logic                         o_store_full,       // registered

    // -- The response both sides share -----------------------------
    output logic                         o_rsp_fault,        // valid with either done
    output logic [LINE_WORDS-1:0][31:0]  o_rsp_line,         // valid with a read's done
    output logic                         o_store_fault,      // a posted store's write faulted
    output logic [31:0]                  o_store_fault_addr, // its address

    // -- The L2's cache device (sysreg device 9) -------------------
    input  logic [3:0]                   i_sys_reg,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0]                  i_sys_wdata,        // CTRL holds only the enable
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic                         i_sys_we,
    input  logic                         i_sys_re,
    output logic [31:0]                  o_sys_rdata,        // a cycle after the read

    // -- External bus (master side) --------------------------------
    output logic [31:0]                  o_bus_addr,
    output logic [31:0]                  o_bus_wdata,
    output logic [3:0]                   o_bus_byte_en,
    output logic                         o_bus_re,
    output logic                         o_bus_we,
    input  logic [31:0]                  i_bus_rdata,
    input  logic                         i_bus_ack,
    input  logic                         i_bus_fault        // valid with i_bus_ack
);

    typedef logic [SET_BITS-1:0]         set_t;
    typedef logic [WAY_BITS-1:0]         way_t;
    typedef logic [TAG_BITS-1:0]         tag_t;
    typedef logic [WORD_BITS-1:0]        word_t;
    typedef logic [TREE_BITS:1]          tree_t;     // numbered as the tree's nodes
    typedef logic [LINE_WORDS-1:0][31:0] line_t;

    localparam set_t SET_LAST = set_t'(SETS - 1);

    // ── Replacement ──────────────────────────────────────────────
    // The tree is heap-ordered: node 1 is the root, node n's halves are
    // nodes 2n (lower) and 2n+1 (upper), and way w is leaf WAYS+w. Node n's
    // bit is tree bit n, 0 naming the lower half as less recently used and
    // 1 the upper.
    typedef struct packed {
        tree_t we;
        tree_t bits;
    } touch_t;

    // A touch of way w writes the bits on its path, each to name the half
    // w is not in.
    function automatic touch_t touch(way_t w);
        touch_t t;
        int     node;
        t    = '0;
        node = WAYS + int'(w);
        for (int l = 0; l < WAY_BITS; l++) begin
            t.we[node >> 1]   = 1'b1;
            t.bits[node >> 1] = !node[0];
            node = node >> 1;
        end
        return t;
    endfunction

    // The way a miss's line goes to: the lowest invalid way of the set,
    // else the way the tree's bits lead to from the root.
    function automatic way_t victim(logic [WAYS-1:0] valid, tree_t tree);
        int node = 1;
        for (int w = 0; w < WAYS; w++) begin
            if (!valid[w]) return way_t'(w);
        end
        for (int l = 0; l < WAY_BITS; l++) begin
            if (tree[node])
                node = 2 * node + 1;
            else
                node = 2 * node;
        end
        return way_t'(node - WAYS);
    endfunction

    // ══════════════════════════════════════════════════════════
    // Issue
    // ══════════════════════════════════════════════════════════
    logic data_inpipe_q, fetch_inpipe_q;  // the side's request is in the pipeline
    logic bt_busy_q;               // a bus transaction is in flight
    logic upd_write_q;             // the update stage writes a write hit's word
    logic enable_q, inval_pending_q, walking_q, walk_start;
    logic yield_q;                 // a side waits on the buffer's order: posts go first

    // The store buffer's ports; it sits with the bus master, below.
    logic        sb_post_valid, sb_bus_valid, sb_bus_taken, sb_posted;
    logic        sb_line_queued, sb_empty;
    logic [31:0] sb_post_addr, sb_post_wdata, sb_bus_addr, sb_bus_wdata;
    logic [3:0]  sb_post_byte_en, sb_bus_byte_en;

    // The verdict stage's operation, its source: a side's, or a post.
    logic s1_valid_q, s1_dside_q, s1_post_q;

    // Nothing issues while a post is at the verdict: an operation issued
    // then reads the L2 before the post's bytes reach it at the update
    // stage, and finds the store no longer queued.
    logic can_issue, data_ready, fetch_ready, post_ready, issue;
    assign can_issue   = !bt_busy_q && !upd_write_q && !inval_pending_q
                      && !(s1_valid_q && s1_post_q);
    assign data_ready  = i_dreq_valid && !data_inpipe_q;
    assign fetch_ready = i_ireq_valid && !fetch_inpipe_q;
    assign post_ready  = sb_post_valid;
    assign issue       = can_issue && (data_ready || fetch_ready || post_ready);

    // The data side first, then the fetch side, then the buffer's oldest
    // queued store; the store first while a side waits on the buffer's
    // order, which only posting the stores ahead of it ends.
    logic sel_post, sel_data, issue_data, issue_fetch;
    assign sel_post    = post_ready && (yield_q || !(data_ready || fetch_ready));
    assign sel_data    = !sel_post && data_ready;
    assign issue_data  = issue && sel_data;
    assign issue_fetch = issue && !sel_post && !sel_data;

    // The chosen request. A fetch reads whole words.
    logic        req_cacheable, req_we, req_lookup;
    logic [31:0] req_addr, req_wdata;
    logic [3:0]  req_byte_en;
    always_comb begin
        if (sel_post) begin
            req_cacheable = 1'b1;
            req_we        = 1'b1;
            req_addr      = sb_post_addr;
            req_byte_en   = sb_post_byte_en;
            req_wdata     = sb_post_wdata;
        end else if (sel_data) begin
            req_cacheable = i_dreq_cacheable;
            req_we        = i_dreq_we;
            req_addr      = i_dreq_addr;
            req_byte_en   = i_dreq_byte_en;
            req_wdata     = i_dreq_wdata;
        end else begin
            req_cacheable = i_ireq_cacheable;
            req_we        = 1'b0;
            req_addr      = i_ireq_addr;
            req_byte_en   = 4'hF;
            req_wdata     = 32'b0;
        end
        req_lookup = req_cacheable && enable_q && !walking_q;
    end

    // ── The verdict stage's operation ────────────────────────────
    logic        s1_cacheable_q, s1_we_q, s1_lookup_q;
    logic [31:0] s1_addr_q, s1_wdata_q;
    logic [3:0]  s1_byte_en_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) s1_valid_q <= 1'b0;
        else       s1_valid_q <= issue;
    end

    always_ff @(posedge i_clk)
        if (issue) begin
            s1_post_q      <= sel_post;
            s1_dside_q     <= sel_data;
            s1_cacheable_q <= req_cacheable;
            s1_we_q        <= req_we;
            s1_lookup_q    <= req_lookup;
            s1_addr_q      <= req_addr;
            s1_wdata_q     <= req_wdata;
            s1_byte_en_q   <= req_byte_en;
        end

    tag_t  s1_tag;
    set_t  s1_set;
    word_t s1_word;
    assign s1_tag  = s1_addr_q[31 -: TAG_BITS];
    assign s1_set  = s1_addr_q[OFFSET_BITS +: SET_BITS];
    assign s1_word = s1_addr_q[2 +: WORD_BITS];

    // ══════════════════════════════════════════════════════════
    // The arrays
    // ══════════════════════════════════════════════════════════
    logic [WAYS-1:0][TAG_BITS-1:0] rd_tag;
    line_t [WAYS-1:0]              rd_line;
    logic [WAYS-1:0]               rd_valid;
    tree_t                         rd_tree;

    logic        fill_q, upd_touch_q, victim_rd_q;
    way_t        fill_way, upd_way_q;
    set_t        bt_set_q, upd_set_q, walk_set_q;
    tag_t        bt_tag_q;
    word_t       upd_word_q;
    logic [WAYS-1:0] bt_valid_q, fill_valid_row;
    line_t       line_q;
    logic [31:0] upd_word_data;

    logic            valid_we;
    set_t            valid_set;
    logic [WAYS-1:0] valid_row;
    touch_t          tree_wr;
    set_t            tree_wr_set;

    penumbra3_l2_store #(
        .SETS       (SETS),
        .WAYS       (WAYS),
        .LINE_BYTES (LINE_BYTES)
    ) u_store (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_rd_en        (issue && req_lookup),
        .i_rd_set       (req_addr[OFFSET_BITS +: SET_BITS]),
        .o_rd_tag       (rd_tag),
        .o_rd_line      (rd_line),
        .o_rd_valid     (rd_valid),
        .i_fill_we      (fill_q),
        .i_fill_set     (bt_set_q),
        .i_fill_way     (fill_way),
        .i_fill_tag     (bt_tag_q),
        .i_fill_line    (line_q),
        .i_word_we      (upd_write_q),
        .i_word_set     (upd_set_q),
        .i_word_way     (upd_way_q),
        .i_word_idx     (upd_word_q),
        .i_word_data    (upd_word_data),
        .i_valid_we     (valid_we),
        .i_valid_set    (valid_set),
        .i_valid_row    (valid_row),
        .i_tree_rd_en   (victim_rd_q),
        .i_tree_rd_set  (bt_set_q),
        .o_tree         (rd_tree),
        .i_tree_we      (tree_wr.we),
        .i_tree_wr_set  (tree_wr_set),
        .i_tree_wr_bits (tree_wr.bits)
    );

    // ══════════════════════════════════════════════════════════
    // The verdict
    // ══════════════════════════════════════════════════════════
    logic [WAYS-1:0] way_hit;
    logic            hit;
    way_t            hit_way;
    line_t           hit_line;
    always_comb begin
        hit_way  = '0;
        hit_line = '0;
        for (int w = 0; w < WAYS; w++) begin
            way_hit[w] = s1_lookup_q && rd_valid[w] && (rd_tag[w] == s1_tag);
            if (way_hit[w]) hit_way = way_t'(w);
            hit_line = hit_line | (rd_line[w] & {(LINE_WORDS * 32){way_hit[w]}});
        end
        hit = |way_hit;
    end

    // A line read the L2 does not serve, and every word access, take the
    // bus; a post never does. Nor does a line read a stream buffer holds or
    // is fetching: the L2 or the buffer serves it, or it waits for the
    // buffer's fetch. That needs no tag compare, so it stays off the late
    // hit.
    logic        pf_hit, pf_pending;
    stream_buf_t pf_hit_buf;
    logic s1_line_read, s1_fside, s1_buffered, s1_bus_op;
    assign s1_line_read = s1_cacheable_q && !s1_we_q;
    assign s1_fside     = !s1_dside_q && !s1_post_q;
    assign s1_buffered  = s1_lookup_q && s1_line_read && (pf_hit || pf_pending);
    assign s1_bus_op    = !s1_post_q && !(s1_line_read && hit) && !s1_buffered;

    // The buffer's order: a line read waits while a queued store holds its
    // line, so the L2 has every older store to it; a word access waits for
    // the buffer to empty. Either takes the bus only with no posted store
    // left, which the bus master would otherwise take after it.
    logic s1_ordered;
    assign s1_ordered = s1_line_read ? !sb_line_queued : sb_empty;

    // Nothing at the verdict acts while a bus transaction is in flight. A
    // served read is a transaction with no bus: its line comes from the
    // buffer and installs as a bus read's would.
    logic bm_ready;
    logic s1_live, s1_read_hit, s1_post, s1_take_bus, s1_serve, s1_complete, s1_drop;
    assign s1_live     = s1_valid_q && !bt_busy_q;
    assign s1_read_hit = s1_live && s1_line_read && hit && s1_ordered;
    assign s1_post     = s1_live && s1_post_q;
    assign s1_take_bus = s1_live && s1_bus_op && s1_ordered && !sb_posted && bm_ready;
    assign s1_serve    = s1_live && s1_buffered && pf_hit && !hit && s1_ordered;
    assign s1_complete = s1_read_hit || s1_post;
    assign s1_drop     = s1_valid_q && !s1_complete && !s1_take_bus && !s1_serve;

    // ── In the pipeline: from issue through the done, or to a drop ─
    // A post is in the pipeline only at the verdict, where it blocks issue.
    logic ddone_q, idone_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            data_inpipe_q  <= 1'b0;
            fetch_inpipe_q <= 1'b0;
        end else begin
            if (issue_data)                              data_inpipe_q  <= 1'b1;
            else if (ddone_q || (s1_drop && s1_dside_q)) data_inpipe_q  <= 1'b0;
            if (issue_fetch)                             fetch_inpipe_q <= 1'b1;
            else if (idone_q || (s1_drop && s1_fside))   fetch_inpipe_q <= 1'b0;
        end
    end

    // ── Yield: a side the buffer's order dropped lets a post go first ─
    // Until a post completes, so a side never takes the slots the stores
    // ahead of it need, and a post never waits on a side that can go.
    always_ff @(posedge i_clk) begin
        if (i_rst)                                     yield_q <= 1'b0;
        else if (s1_post)                              yield_q <= 1'b0;
        else if (s1_drop && !s1_post_q && !s1_ordered) yield_q <= 1'b1;
    end

    // ══════════════════════════════════════════════════════════
    // The update stage
    // ══════════════════════════════════════════════════════════
    logic [31:0] upd_wdata_q;
    logic [3:0]  upd_byte_en_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            upd_write_q <= 1'b0;
            upd_touch_q <= 1'b0;
        end else begin
            upd_write_q <= s1_post && hit;
            upd_touch_q <= s1_complete && hit;
        end
    end

    always_ff @(posedge i_clk)
        if (s1_complete) begin
            upd_way_q     <= hit_way;
            upd_set_q     <= s1_set;
            upd_word_q    <= s1_word;
            upd_wdata_q   <= s1_wdata_q;
            upd_byte_en_q <= s1_byte_en_q;
        end

    // A write hit's bytes merged over the word its own lookup read, which
    // line_q holds from the verdict.
    always_comb begin
        upd_word_data = line_q[upd_word_q];
        for (int b = 0; b < 4; b++)
            if (upd_byte_en_q[b]) upd_word_data[8*b +: 8] = upd_wdata_q[8*b +: 8];
    end

    // ══════════════════════════════════════════════════════════
    // The store buffer and the bus master
    // ══════════════════════════════════════════════════════════
    logic bt_done, post_done;

    penumbra3_store_buffer #(
        .ENTRIES (STORE_BUFFER_ENTRIES)
    ) u_store_buffer (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_enq          (i_store_enq),
        .i_enq_addr     (i_store_addr),
        .i_enq_wdata    (i_store_wdata),
        .i_enq_byte_en  (i_store_byte_en),
        .o_full         (o_store_full),
        .o_post_valid   (sb_post_valid),
        .o_post_addr    (sb_post_addr),
        .o_post_wdata   (sb_post_wdata),
        .o_post_byte_en (sb_post_byte_en),
        .i_posted       (s1_post),
        .o_bus_valid    (sb_bus_valid),
        .o_bus_addr     (sb_bus_addr),
        .o_bus_wdata    (sb_bus_wdata),
        .o_bus_byte_en  (sb_bus_byte_en),
        .i_bus_taken    (sb_bus_taken),
        .i_bus_done     (post_done),
        .o_posted       (sb_posted),
        .i_line         (s1_addr_q[31:4]),
        .o_line_queued  (sb_line_queued),
        .o_empty        (sb_empty)
    );

    // What the bus master runs, recorded as it takes it, so its completion
    // goes to its owner. A store taken behind a store on the bus is a store
    // too, so taking it changes nothing here.
    typedef enum logic [1:0] {
        BM_STORE,        // a posted store's write
        BM_TRANSACTION,  // the verdict's bus transaction
        BM_PREFETCH      // a stream buffer's fetch
    } bm_owner_e;
    bm_owner_e bm_owner_q;

    // A posted store goes first: it is older than the verdict's operation,
    // which waits for it. It goes behind a store the master is writing, so
    // a run of stores streams with the strobe held, and otherwise when the
    // master is idle. A prefetch goes only when the bus has nothing else
    // to do: nothing at the verdict, no transaction in flight, and no store
    // posted, so it reads memory after every older store. Which of the
    // three goes is chosen on registers.
    logic                        bm_req_valid, bm_we, bm_line_req, bm_done, bm_fault;
    logic                        bm_behind_ready, store_take;
    logic [31:0]                 bm_addr, bm_done_addr;
    logic [3:0]                  bm_byte_en;
    logic [LINE_WORDS-1:0][31:0] bm_wline;
    line_t                       bm_line;
    logic                        pf_valid, pf_take;
    logic [LINE_ADDR_BITS-1:0]   pf_addr;
    assign store_take = sb_bus_valid
                     && (bm_ready || (bm_behind_ready && bm_owner_q == BM_STORE));
    assign pf_take = !sb_bus_valid && !s1_valid_q && !bt_busy_q && !sb_posted
                  && pf_valid && bm_ready;
    always_comb begin
        bm_wline = '0;
        if (sb_bus_valid) begin
            bm_req_valid = store_take;
            bm_we        = 1'b1;
            bm_line_req  = 1'b0;
            bm_addr      = sb_bus_addr;
            bm_byte_en   = sb_bus_byte_en;
            bm_wline[0]  = sb_bus_wdata;
        end else if (s1_valid_q) begin
            bm_req_valid = s1_take_bus;
            bm_we        = s1_we_q;
            bm_line_req  = s1_line_read;
            if (s1_line_read) bm_addr = {s1_addr_q[31:OFFSET_BITS], {OFFSET_BITS{1'b0}}};
            else              bm_addr = s1_addr_q;
            bm_byte_en   = s1_byte_en_q;
            bm_wline[0]  = s1_wdata_q;
        end else begin
            bm_req_valid = pf_take;
            bm_we        = 1'b0;
            bm_line_req  = 1'b1;
            bm_addr      = {pf_addr, {OFFSET_BITS{1'b0}}};
            bm_byte_en   = 4'hF;
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst)
            bm_owner_q <= BM_STORE;
        else if (bm_req_valid && bm_ready) begin
            if (sb_bus_valid)    bm_owner_q <= BM_STORE;
            else if (s1_valid_q) bm_owner_q <= BM_TRANSACTION;
            else                 bm_owner_q <= BM_PREFETCH;
        end
    end

    penumbra3_bus_master #(
        .LINE_BYTES (LINE_BYTES)
    ) u_bus_master (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_req_valid   (bm_req_valid),
        .o_req_ready   (bm_ready),
        .o_behind_ready (bm_behind_ready),
        .i_req_we      (bm_we),
        .i_req_line    (bm_line_req),
        .i_req_addr    (bm_addr),
        .i_req_byte_en (bm_byte_en),
        .i_req_wline   (bm_wline),
        .o_rsp_valid   (bm_done),
        .o_rsp_rline   (bm_line),
        .o_rsp_fault   (bm_fault),
        .o_rsp_addr    (bm_done_addr),
        .o_bus_addr    (o_bus_addr),
        .o_bus_wdata   (o_bus_wdata),
        .o_bus_byte_en (o_bus_byte_en),
        .o_bus_re      (o_bus_re),
        .o_bus_we      (o_bus_we),
        .i_bus_rdata   (i_bus_rdata),
        .i_bus_ack     (i_bus_ack),
        .i_bus_fault   (i_bus_fault)
    );

    // The bus master's completion goes to what it ran. A served read has no
    // bus: it completes the cycle after its verdict. The master takes a
    // store while it still writes the one before, so a fault takes the
    // store's address from the master's completion.
    logic serve_done_q, pf_done;
    assign sb_bus_taken = store_take;
    assign post_done    = bm_done && bm_owner_q == BM_STORE;
    assign pf_done      = bm_done && bm_owner_q == BM_PREFETCH;
    assign bt_done      = (bm_done && bm_owner_q == BM_TRANSACTION) || serve_done_q;

    logic store_fault_q;
    logic [31:0] store_fault_addr_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) store_fault_q <= 1'b0;
        else       store_fault_q <= post_done && bm_fault;
        if (post_done) store_fault_addr_q <= bm_done_addr;
    end

    // ══════════════════════════════════════════════════════════
    // The bus transaction
    // ══════════════════════════════════════════════════════════
    // bt_install_q: a line read that looked up, so its line installs.
    // bt_serve_q: a read a stream buffer serves; bt_buf_q the buffer.
    logic        bt_dside_q, bt_install_q, bt_done_q, bt_serve_q, bt_start;
    stream_buf_t bt_buf_q;
    assign bt_start = s1_take_bus || s1_serve;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            bt_busy_q    <= 1'b0;
            bt_done_q    <= 1'b0;
            serve_done_q <= 1'b0;
            victim_rd_q  <= 1'b0;
            fill_q       <= 1'b0;
        end else begin
            if (bt_start)       bt_busy_q <= 1'b1;
            else if (bt_done_q) bt_busy_q <= 1'b0;
            bt_done_q    <= bt_done;
            serve_done_q <= s1_serve;
            victim_rd_q  <= bt_start && s1_lookup_q && s1_line_read;
            fill_q       <= bt_done && bt_install_q && (bt_serve_q || !bm_fault);
        end
    end

    always_ff @(posedge i_clk)
        if (bt_start) begin
            bt_dside_q   <= s1_dside_q;
            bt_install_q <= s1_lookup_q && s1_line_read;
            bt_serve_q   <= s1_serve;
            bt_buf_q     <= pf_hit_buf;
            bt_set_q     <= s1_set;
            bt_tag_q     <= s1_tag;
            bt_valid_q   <= rd_valid;
        end

    // The replacement bits, read the cycle after the read took the bus,
    // hold until the next miss reads them.
    assign fill_way = victim(bt_valid_q, rd_tree);
    always_comb begin
        fill_valid_row = bt_valid_q;
        fill_valid_row[fill_way] = 1'b1;
    end

    // ── Valid rows: an install's, or the walk's ──────────────────
    always_comb begin
        valid_we = fill_q || walking_q;
        if (fill_q) begin
            valid_set = bt_set_q;
            valid_row = fill_valid_row;
        end else begin
            valid_set = walk_set_q;
            valid_row = '0;
        end
    end

    // ── Replacement touches: an install's victim, or a hit's way ─
    always_comb begin
        tree_wr     = '0;
        tree_wr_set = upd_set_q;
        if (fill_q) begin
            tree_wr     = touch(fill_way);
            tree_wr_set = bt_set_q;
        end else if (upd_touch_q) begin
            tree_wr     = touch(upd_way_q);
        end
    end

    // ══════════════════════════════════════════════════════════
    // The stream buffers
    // ══════════════════════════════════════════════════════════
    // Each fill and each write reaches the buffers a cycle after its
    // verdict, from registers. That is in time for the next verdict: a fill
    // or an uncached write is a transaction, so nothing at the verdict acts
    // before its done, and nothing issues while a post is at the verdict.
    // A fill is a line read the L2 looked up and missed, served or not.
    logic                      pf_fill_q, pf_write_q;
    logic [LINE_ADDR_BITS-1:0] pf_event_addr_q;
    line_t                     pf_line;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            pf_fill_q  <= 1'b0;
            pf_write_q <= 1'b0;
        end else begin
            pf_fill_q  <= bt_start && s1_lookup_q && s1_line_read;
            pf_write_q <= s1_post || (s1_take_bus && s1_we_q);
        end
    end

    always_ff @(posedge i_clk)
        if (s1_valid_q) pf_event_addr_q <= s1_addr_q[31:OFFSET_BITS];

    penumbra3_stream_buffer #(
        .LINE_BYTES (LINE_BYTES)
    ) u_stream (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_lookup_addr (s1_addr_q[31:OFFSET_BITS]),
        .o_hit         (pf_hit),
        .o_hit_buf     (pf_hit_buf),
        .o_pending     (pf_pending),
        .i_read_buf    (bt_buf_q),
        .o_read_line   (pf_line),
        .i_fill        (pf_fill_q),
        .i_write       (pf_write_q),
        .i_event_addr  (pf_event_addr_q),
        .i_clear       (walk_start),
        .o_pf_valid    (pf_valid),
        .o_pf_addr     (pf_addr),
        .i_pf_taken    (pf_take),
        .i_pf_done     (pf_done),
        .i_pf_fault    (bm_fault),
        .i_pf_line     (bm_line)
    );

    // ══════════════════════════════════════════════════════════
    // The response
    // ══════════════════════════════════════════════════════════
    logic rsp_fault_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ddone_q <= 1'b0;
            idone_q <= 1'b0;
        end else begin
            ddone_q <= (s1_read_hit && s1_dside_q) || (bt_done &&  bt_dside_q);
            idone_q <= (s1_read_hit && s1_fside)   || (bt_done && !bt_dside_q);
        end
    end

    // A write hit's line too: the update stage merges into its word.
    always_ff @(posedge i_clk) begin
        if (s1_complete) begin
            rsp_fault_q <= 1'b0;
            if (hit) line_q <= hit_line;
        end else if (bt_done) begin
            rsp_fault_q <= !bt_serve_q && bm_fault;
            line_q      <= bt_serve_q ? pf_line : bm_line;
        end
    end

    assign o_drsp_done        = ddone_q;
    assign o_irsp_done        = idone_q;
    assign o_rsp_fault        = rsp_fault_q;
    assign o_rsp_line         = line_q;
    assign o_store_fault      = store_fault_q;
    assign o_store_fault_addr = store_fault_addr_q;

    // ══════════════════════════════════════════════════════════
    // The cache device
    // ══════════════════════════════════════════════════════════
    // The walk starts with nothing at the verdict and no bus transaction in
    // flight, so no install meets it; issue waits for it to start.
    assign walk_start = inval_pending_q && !walking_q && !s1_valid_q && !bt_busy_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            enable_q        <= 1'b0;
            inval_pending_q <= 1'b1;     // a reset leaves no line that hits
            walking_q       <= 1'b0;
        end else begin
            if (i_sys_we && i_sys_reg == SYSREG_CACHE_CTRL) enable_q <= i_sys_wdata[0];
            if (walk_start) begin
                inval_pending_q <= 1'b0;
                walking_q       <= 1'b1;
            end else if (walking_q && walk_set_q == SET_LAST) begin
                walking_q       <= 1'b0;
            end
            if (i_sys_we && i_sys_reg == SYSREG_CACHE_INVAL_ALL) inval_pending_q <= 1'b1;
        end
    end

    always_ff @(posedge i_clk) begin
        if (walk_start)     walk_set_q <= '0;
        else if (walking_q) walk_set_q <= walk_set_q + 1'b1;
    end

    // ── Counters: each lookup once, where it completes or its transaction starts ─
    // A read a stream buffer serves is a miss: the L2 did not hold its line.
    logic ev_read_hit_q, ev_read_miss_q, ev_write_hit_q, ev_write_miss_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ev_read_hit_q   <= 1'b0;
            ev_read_miss_q  <= 1'b0;
            ev_write_hit_q  <= 1'b0;
            ev_write_miss_q <= 1'b0;
        end else begin
            ev_read_hit_q   <= s1_read_hit;
            ev_read_miss_q  <= bt_start && s1_lookup_q && s1_line_read;
            ev_write_hit_q  <= s1_post && s1_lookup_q &&  hit;
            ev_write_miss_q <= s1_post && s1_lookup_q && !hit;
        end
    end

    logic [3:0]  rd_reg_q;
    logic [31:0] perf_rdata;
    always_ff @(posedge i_clk)
        if (i_sys_re) rd_reg_q <= i_sys_reg;

    cache_perfctr u_perfctr (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_event_read_hit   (ev_read_hit_q),
        .i_event_read_miss  (ev_read_miss_q),
        .i_event_write_hit  (ev_write_hit_q),
        .i_event_write_miss (ev_write_miss_q),
        .i_sys_reg          (rd_reg_q),
        .o_sys_rdata        (perf_rdata)
    );

    // Write-through, write-no-allocate, physically addressed.
    typedef logic [4:0]  info_ways_t;
    typedef logic [14:0] info_sets_t;
    typedef logic [5:0]  info_words_t;
    localparam logic [31:0] INFO = {
        2'b0,                         // [31:30] reserved
        1'b0,                         // [29]    write-allocate
        1'b0,                         // [28]    write-back
        CACHE_ADDR_PIPT,              // [27:26] addressing
        info_ways_t'(WAYS),           // [25:21] ways
        info_sets_t'(SETS),           // [20:6]  sets
        info_words_t'(LINE_WORDS)     // [5:0]   line words
    };

    always_comb begin
        case (rd_reg_q)
            SYSREG_CACHE_INFO:   o_sys_rdata = INFO;
            SYSREG_CACHE_CTRL:   o_sys_rdata = {31'b0, enable_q};
            SYSREG_CACHE_STATUS: o_sys_rdata = {31'b0, inval_pending_q || walking_q};
            default:             o_sys_rdata = perf_rdata;
        endcase
    end

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // A requester holds its request from its issue through its done
    // (doc/internals/penumbra3/uncore-port.md, "Handshake").
    assert property (@(posedge i_clk) disable iff (i_rst)
        data_inpipe_q |-> i_dreq_valid)
        else $error("penumbra3_uncore: a data request dropped before its done");
    assert property (@(posedge i_clk) disable iff (i_rst)
        fetch_inpipe_q |-> i_ireq_valid)
        else $error("penumbra3_uncore: a fetch request dropped before its done");

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(ddone_q && idone_q))
        else $error("penumbra3_uncore: both sides' done in one cycle");

    assert property (@(posedge i_clk) disable iff (i_rst)
        $onehot0(way_hit))
        else $error("penumbra3_uncore: a line in two ways");

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(fill_q && (walking_q || upd_write_q || upd_touch_q)))
        else $error("penumbra3_uncore: an install met the walk or the update stage");

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(s1_take_bus && (sb_posted || !bm_ready)))
        else $error("penumbra3_uncore: a bus transaction ahead of a posted store");

    // A posted store comes only from a post, which never completes while a
    // transaction is in flight; one that takes the bus starts with none.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(bt_busy_q && !bt_serve_q && sb_posted))
        else $error("penumbra3_uncore: a posted store while a bus transaction is in flight");

    // A prefetch takes the bus only with nothing else for it.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(pf_take && (sb_posted || s1_valid_q || bt_busy_q)))
        else $error("penumbra3_uncore: a prefetch ahead of a store or a transaction");

    // Cacheable writes come only from the store buffer.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_dreq_valid && i_dreq_cacheable && i_dreq_we))
        else $error("penumbra3_uncore: the data side requested a cacheable write");

endmodule
