// penumbra3_uncore -- the gen3 uncore: both L1s' requests through the L2 and
// onto the external bus.
//
// doc/internals/penumbra3/uncore.md specifies it: the select, the L2's
// lookup and penumbra3_bus_master as one pipeline, with one posted write
// beside it. The arrays are penumbra3_l2_store's; this module is the
// pipeline's control, the posted write, the record of the bus transaction,
// the L2's cache device (sysreg device 9) and its counters.
//
// For an operation issuing in cycle i:
//
//   i     issue    the select takes a request; a lookup reads its set
//   i+1   verdict  the tag compare; the operation completes, posts, takes
//                  the bus, or is dropped and its side issues it again
//   i+2   update   the requester's done; a write hit's word and the
//                  replacement touch, from registers
//
// A bus transaction goes from the verdict straight into the bus master, and
// is in flight from the next cycle through its done, which comes a cycle
// after the bus master completes and is the cycle its line installs.
module penumbra3_uncore
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
#(
    parameter  int SETS        = 1024,
    parameter  int WAYS        = 4,
    localparam int LINE_WORDS  = UNCORE_LINE_WORDS,
    localparam int LINE_BYTES  = LINE_WORDS * 4,
    localparam int OFFSET_BITS = $clog2(LINE_BYTES),
    localparam int WORD_BITS   = $clog2(LINE_WORDS),
    localparam int SET_BITS    = $clog2(SETS),
    localparam int WAY_BITS    = $clog2(WAYS),
    localparam int TAG_BITS    = 32 - SET_BITS - OFFSET_BITS,
    localparam int TREE_BITS   = WAYS - 1
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

    // -- The response both sides share -----------------------------
    output logic                         o_rsp_fault,        // valid with either done
    output logic [LINE_WORDS-1:0][31:0]  o_rsp_line,         // valid with a read's done
    output logic                         o_store_fault,      // the posted write faulted
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
    logic enable_q, inval_pending_q, walking_q;

    logic can_issue, data_ready, fetch_ready, issue, issue_data;
    assign can_issue   = !bt_busy_q && !upd_write_q && !inval_pending_q;
    assign data_ready  = i_dreq_valid && !data_inpipe_q;
    assign fetch_ready = i_ireq_valid && !fetch_inpipe_q;
    assign issue       = can_issue && (data_ready || fetch_ready);
    assign issue_data  = can_issue && data_ready;          // the data side first

    // The chosen request. A fetch reads whole words.
    logic        req_cacheable, req_we, req_lookup;
    logic [31:0] req_addr, req_wdata;
    logic [3:0]  req_byte_en;
    always_comb begin
        if (data_ready) begin
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
    logic        s1_valid_q, s1_dside_q, s1_cacheable_q, s1_we_q, s1_lookup_q;
    logic [31:0] s1_addr_q, s1_wdata_q;
    logic [3:0]  s1_byte_en_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) s1_valid_q <= 1'b0;
        else       s1_valid_q <= issue;
    end

    always_ff @(posedge i_clk)
        if (issue) begin
            s1_dside_q     <= issue_data;
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
    // bus; a cacheable write posts.
    logic s1_line_read, s1_cwrite, s1_bus_op;
    assign s1_line_read = s1_cacheable_q && !s1_we_q;
    assign s1_cwrite    = s1_cacheable_q &&  s1_we_q;
    assign s1_bus_op    = !s1_cwrite && !(s1_line_read && hit);

    // Nothing at the verdict acts while a bus transaction is in flight.
    logic post_valid_q, bm_ready;
    logic s1_live, s1_read_hit, s1_post, s1_take_bus, s1_complete, s1_drop;
    assign s1_live     = s1_valid_q && !bt_busy_q;
    assign s1_read_hit = s1_live && s1_line_read && hit;
    assign s1_post     = s1_live && s1_cwrite && !post_valid_q;
    assign s1_take_bus = s1_live && s1_bus_op && !post_valid_q && bm_ready;
    assign s1_complete = s1_read_hit || s1_post;
    assign s1_drop     = s1_valid_q && !s1_complete && !s1_take_bus;

    // ── In the pipeline: from issue through the done, or to a drop ─
    logic ddone_q, idone_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            data_inpipe_q  <= 1'b0;
            fetch_inpipe_q <= 1'b0;
        end else begin
            if (issue_data)                               data_inpipe_q  <= 1'b1;
            else if (ddone_q || (s1_drop && s1_dside_q))  data_inpipe_q  <= 1'b0;
            if (issue && !issue_data)                     fetch_inpipe_q <= 1'b1;
            else if (idone_q || (s1_drop && !s1_dside_q)) fetch_inpipe_q <= 1'b0;
        end
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
    // The posted write and the bus master
    // ══════════════════════════════════════════════════════════
    logic        post_sent_q;      // the bus master has taken the posted write
    logic [31:0] post_addr_q, post_wdata_q;
    logic [3:0]  post_byte_en_q;
    logic        post_req;
    assign post_req = post_valid_q && !post_sent_q;

    // The posted write goes first: it is older than the verdict's
    // operation, which waits for it.
    logic                        bm_req_valid, bm_we, bm_line_req, bm_done, bm_fault;
    logic [31:0]                 bm_addr;
    logic [3:0]                  bm_byte_en;
    logic [LINE_WORDS-1:0][31:0] bm_wline;
    line_t                       bm_line;
    always_comb begin
        bm_wline = '0;
        if (post_req) begin
            bm_req_valid = bm_ready;
            bm_we        = 1'b1;
            bm_line_req  = 1'b0;
            bm_addr      = post_addr_q;
            bm_byte_en   = post_byte_en_q;
            bm_wline[0]  = post_wdata_q;
        end else begin
            bm_req_valid = s1_take_bus;
            bm_we        = s1_we_q;
            bm_line_req  = s1_line_read;
            if (s1_line_read) bm_addr = {s1_addr_q[31:OFFSET_BITS], {OFFSET_BITS{1'b0}}};
            else              bm_addr = s1_addr_q;
            bm_byte_en   = s1_byte_en_q;
            bm_wline[0]  = s1_wdata_q;
        end
    end

    penumbra3_bus_master #(
        .LINE_BYTES (LINE_BYTES)
    ) u_bus_master (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_req_valid   (bm_req_valid),
        .o_req_ready   (bm_ready),
        .i_req_we      (bm_we),
        .i_req_line    (bm_line_req),
        .i_req_addr    (bm_addr),
        .i_req_byte_en (bm_byte_en),
        .i_req_wline   (bm_wline),
        .o_rsp_valid   (bm_done),
        .o_rsp_rline   (bm_line),
        .o_rsp_fault   (bm_fault),
        .o_bus_addr    (o_bus_addr),
        .o_bus_wdata   (o_bus_wdata),
        .o_bus_byte_en (o_bus_byte_en),
        .o_bus_re      (o_bus_re),
        .o_bus_we      (o_bus_we),
        .i_bus_rdata   (i_bus_rdata),
        .i_bus_ack     (i_bus_ack),
        .i_bus_fault   (i_bus_fault)
    );

    // The bus master runs the posted write and a bus transaction never at
    // once, so its completion is the transaction's while one is in flight.
    logic bt_done, post_done;
    assign bt_done   = bm_done &&  bt_busy_q;
    assign post_done = bm_done && !bt_busy_q;

    logic store_fault_q;
    logic [31:0] store_fault_addr_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            post_valid_q  <= 1'b0;
            post_sent_q   <= 1'b0;
            store_fault_q <= 1'b0;
        end else begin
            if (s1_post) begin
                post_valid_q <= 1'b1;
                post_sent_q  <= 1'b0;
            end else begin
                if (post_done)                 post_valid_q <= 1'b0;
                if (post_req && bm_req_valid)  post_sent_q  <= 1'b1;
            end
            store_fault_q <= post_done && bm_fault;
        end
    end

    always_ff @(posedge i_clk) begin
        if (s1_post) begin
            post_addr_q    <= s1_addr_q;
            post_wdata_q   <= s1_wdata_q;
            post_byte_en_q <= s1_byte_en_q;
        end
        if (post_done) store_fault_addr_q <= post_addr_q;
    end

    // ══════════════════════════════════════════════════════════
    // The bus transaction
    // ══════════════════════════════════════════════════════════
    // bt_install_q: a line read that looked up, so its line installs.
    logic bt_dside_q, bt_install_q, bt_done_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            bt_busy_q   <= 1'b0;
            bt_done_q   <= 1'b0;
            victim_rd_q <= 1'b0;
            fill_q      <= 1'b0;
        end else begin
            if (s1_take_bus)    bt_busy_q <= 1'b1;
            else if (bt_done_q) bt_busy_q <= 1'b0;
            bt_done_q   <= bt_done;
            victim_rd_q <= s1_take_bus && s1_lookup_q && s1_line_read;
            fill_q      <= bt_done && bt_install_q && !bm_fault;
        end
    end

    always_ff @(posedge i_clk)
        if (s1_take_bus) begin
            bt_dside_q   <= s1_dside_q;
            bt_install_q <= s1_lookup_q && s1_line_read;
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
    // The response
    // ══════════════════════════════════════════════════════════
    logic rsp_fault_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ddone_q <= 1'b0;
            idone_q <= 1'b0;
        end else begin
            ddone_q <= (s1_complete && s1_dside_q)  || (bt_done && bt_dside_q);
            idone_q <= (s1_complete && !s1_dside_q) || (bt_done && !bt_dside_q);
        end
    end

    // A write hit's line too: the update stage merges into its word.
    always_ff @(posedge i_clk) begin
        if (s1_complete) begin
            rsp_fault_q <= 1'b0;
            if (hit) line_q <= hit_line;
        end else if (bt_done) begin
            rsp_fault_q <= bm_fault;
            line_q      <= bm_line;
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
    logic walk_start;
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

    // ── Counters: each lookup once, where it completes or takes the bus ─
    logic ev_read_hit_q, ev_read_miss_q, ev_write_hit_q, ev_write_miss_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ev_read_hit_q   <= 1'b0;
            ev_read_miss_q  <= 1'b0;
            ev_write_hit_q  <= 1'b0;
            ev_write_miss_q <= 1'b0;
        end else begin
            ev_read_hit_q   <= s1_read_hit;
            ev_read_miss_q  <= s1_take_bus && s1_lookup_q && s1_line_read;
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
        !(s1_take_bus && (post_valid_q || !bm_ready)))
        else $error("penumbra3_uncore: a bus transaction ahead of the posted write");

endmodule
