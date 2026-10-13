// penumbra3_l1 -- one gen3 L1 cache: storage, the hit verdict, and the
// cache device around them.
//
// penumbra3_l1_store's arrays and penumbra3_l1_match's compare, plus what
// makes them a cache: the store-hit write, replacement, the enable, and
// the cache device's sysreg registers. It has no access FSM and no request
// port. A lookup reports a verdict and nothing more; its consumer (MEM2's
// completion unit on the D-side, IF2's replay on the I-side) registers a
// miss, runs the fill and installs the line through the fill port, so no
// miss reaches the pipeline except through the consumer's registers. One
// module serves both sides; the I-side ties the store off.
//
// Timing. A lookup takes two cycles. In the first, i_launch (a real
// access, pipe not frozen) reads the set the page offset indexes, in every
// way. In the second the MMU's candidate frames arrive and the outputs
// give the verdict, combinationally: the hit per way, each way's word, the
// hit way's word, and the way a miss would fill. The outputs go on
// belonging to that lookup, the current lookup, until the next launch, and
// its set is read again every cycle: a frozen consumer keeps a verdict
// that follows the cache, through an invalidate or a line installed under
// it. A line shows two cycles after its i_fill_we; the cycle between still
// gives the filled way as it was, so a consumer that froze a lookup across
// a fill resolves it no earlier than the second cycle.
//
// Stores. i_store marks a committing store whose lookup is the current
// one. On a hit the cache merges the store's bytes over the hit way's word
// and writes the whole word in the next cycle, from registers: the hit
// reaches a register's D and never a block RAM's write enable. A lookup of
// that word is given it from the cycle of the write on, ahead of the block
// RAM's own output (penumbra3_l1_store), so a load directly behind a store
// needs no stall. A miss allocates nothing (write-no-allocate). Writing
// through to memory is the consumer's. A store's hit is not qualified by
// the page's cacheability: a line can be resident for an uncacheable
// mapping only if software changed the page's cacheability without
// invalidating the cache.
//
// Replacement: an invalid way first, lowest index first, else tree-PLRU.
// Each tree bit names the less recently used side below it, and a touch
// only writes the bits on its way's path, never reading the old ones. A
// touch lands two cycles after the hit or install it records, off the hit
// path. The victim is read from the current lookup's set, for the
// consumer to park with a miss.
//
// Disabled (CTRL.ENABLE=0, the reset state): a lookup reads every way
// invalid, so nothing hits and no store is absorbed. The enable folds into
// the valid bits as a lookup launches, not into the compare. Lines installed
// before a disable survive it; software pairs an enable change with
// INVAL_ALL. A disabled access on a cacheable page still brings a line
// back (doc/internals/penumbra3/uncore-port.md, "The four transactions");
// o_enabled tells the consumer not to install it (asserted).
module penumbra3_l1
    import penumbra_pkg::*;
#(
    parameter  int WAYS        = 2,
    parameter  int LINE_BYTES  = 16,
    parameter  int FRAMES      = 3,     // the MMU's candidate frames
    localparam int LINE_WORDS  = LINE_BYTES / 4,
    localparam int WORD_BITS   = $clog2(LINE_WORDS),
    localparam int OFFSET_BITS = $clog2(LINE_BYTES),
    localparam int SETS        = 4096 / LINE_BYTES,
    localparam int SET_BITS    = $clog2(SETS),
    localparam int WAY_BITS    = $clog2(WAYS)
) (
    input  logic                         i_clk,
    input  logic                         i_rst,

    // ── Launch ───────────────────────────────────────────────────
    input  logic                         i_launch,     // a real access, pipe not frozen
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [11:0]                  i_offset,     // the access's page offset; its byte bits index nothing
    /* verilator lint_on UNUSEDSIGNAL */

    // ── Resolve ──────────────────────────────────────────────────
    input  logic [FRAMES-1:0][19:0]      i_frame,      // the MMU's candidate frames
    input  logic [FRAMES-1:0]            i_frame_sel,  // one-hot; empty on a TLB miss
    output logic [WAYS-1:0]              o_way_hit,    // one-hot
    output logic [WAYS-1:0][31:0]        o_way_word,
    output logic                         o_hit,
    output logic [31:0]                  o_word,       // the hit way's word
    output logic [WAY_BITS-1:0]          o_victim_way, // where a miss would fill

    // ── Store: a committing store resolving this cycle ───────────
    input  logic                         i_store,
    input  logic [31:0]                  i_store_wdata,
    input  logic [3:0]                   i_store_byte_en,

    // ── Counting: once per cacheable access, in a resolve cycle ──
    input  logic                         i_count_read,
    input  logic                         i_count_write,

    // ── Line install ─────────────────────────────────────────────
    input  logic                         i_fill_we,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0]                  i_fill_paddr, // any address in the line
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [WAY_BITS-1:0]          i_fill_way,
    input  logic [LINE_WORDS-1:0][31:0]  i_fill_line,

    // ── Sysreg interface (this cache's device) ───────────────────
    input  logic [3:0]                   i_sys_reg,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0]                  i_sys_wdata,  // CTRL holds only the enable
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic                         i_sys_we,
    input  logic                         i_sys_re,
    output logic [31:0]                  o_sys_rdata,
    output logic                         o_enabled
);

    build_check #(
        .COND (WAYS == 2 || WAYS == 4),
        .MSG  ("penumbra3_l1: the PLRU tree is built for 2 or 4 ways")
    ) u_check_ways ();

    // ── Control: the enable, invalidate-all, the read selector ───
    // The invalidate is registered so the clear of every valid bit fans
    // out from one flop rather than from the sysreg decode. The read
    // selector is held because an RDSYS launches in MEM1 and reads the
    // response in MEM2.
    logic       cache_en_q, inval_q;
    logic [3:0] rd_reg_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            cache_en_q <= 1'b0;
            inval_q    <= 1'b0;
            rd_reg_q   <= 4'b0;
        end else begin
            inval_q <= i_sys_we && i_sys_reg == SYSREG_CACHE_INVAL_ALL;
            if (i_sys_we && i_sys_reg == SYSREG_CACHE_CTRL)
                cache_en_q <= i_sys_wdata[0];
            if (i_sys_re)
                rd_reg_q <= i_sys_reg;
        end
    end
    assign o_enabled = cache_en_q;

    // ── Storage and the compare ──────────────────────────────────
    logic [SET_BITS-1:0]   rd_set;
    logic [WORD_BITS-1:0]  rd_word_idx;
    logic [WAYS-1:0]       rd_valid;
    logic [WAYS-1:0][19:0] rd_tag;

    logic                  st_we_q;
    logic [SET_BITS-1:0]   st_set_q;
    logic [WORD_BITS-1:0]  st_word_q;
    logic [WAY_BITS-1:0]   st_way_q;
    logic [31:0]           st_wdata_q;

    logic [SET_BITS-1:0]   fill_set;
    assign fill_set = i_fill_paddr[OFFSET_BITS +: SET_BITS];

    penumbra3_l1_store #(
        .WAYS       (WAYS),
        .LINE_BYTES (LINE_BYTES)
    ) u_store (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_launch      (i_launch),
        .i_set         (i_offset[OFFSET_BITS +: SET_BITS]),
        .i_word        (i_offset[2 +: WORD_BITS]),
        .i_lookup_en   (cache_en_q),
        .o_rd_set      (rd_set),
        .o_rd_word_idx (rd_word_idx),
        .o_rd_valid    (rd_valid),
        .o_rd_tag      (rd_tag),
        .o_rd_word     (o_way_word),
        .i_fill_we     (i_fill_we),
        .i_fill_set    (fill_set),
        .i_fill_way    (i_fill_way),
        .i_fill_tag    (i_fill_paddr[31:12]),
        .i_fill_line   (i_fill_line),
        .i_store_we    (st_we_q),
        .i_store_set   (st_set_q),
        .i_store_word  (st_word_q),
        .i_store_way   (st_way_q),
        .i_store_wdata (st_wdata_q),
        .i_inval_all   (inval_q)
    );

    penumbra3_l1_match #(
        .WAYS   (WAYS),
        .FRAMES (FRAMES)
    ) u_match (
        .i_valid     (rd_valid),
        .i_tag       (rd_tag),
        .i_word      (o_way_word),
        .i_frame     (i_frame),
        .i_frame_sel (i_frame_sel),
        .o_way_hit   (o_way_hit),
        .o_hit       (o_hit),
        .o_word      (o_word)
    );

    // The hit way, encoded (o_way_hit is one-hot).
    logic [WAY_BITS-1:0] hit_way;
    always_comb begin
        hit_way = '0;
        for (int w = 0; w < WAYS; w++)
            if (o_way_hit[w]) hit_way |= WAY_BITS'(w);
    end

    // ── The store-hit write ──────────────────────────────────────
    logic [31:0] store_word;
    always_comb
        for (int b = 0; b < 4; b++)
            store_word[8*b +: 8] = i_store_byte_en[b] ? i_store_wdata[8*b +: 8]
                                                      : o_word[8*b +: 8];

    // Only the write enable carries the hit; the rest load every cycle.
    always_ff @(posedge i_clk) begin
        if (i_rst) st_we_q <= 1'b0;
        else       st_we_q <= i_store && o_hit;
        st_set_q   <= rd_set;
        st_word_q  <= rd_word_idx;
        st_way_q   <= hit_way;
        st_wdata_q <= store_word;
    end

    // ── Replacement: invalid first, else tree-PLRU ───────────────
    // first_cycle_q: the current lookup's first cycle on the outputs. A
    // hit touches its way then, so once however long the lookup stays.
    logic                first_cycle_q;
    logic                touch_q;
    logic [SET_BITS-1:0] touch_set_q;
    logic [WAY_BITS-1:0] touch_way_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            first_cycle_q <= 1'b0;
            touch_q       <= 1'b0;
        end else begin
            first_cycle_q <= i_launch;
            touch_q       <= i_fill_we || (first_cycle_q && o_hit);
        end
        touch_set_q <= i_fill_we ? fill_set   : rd_set;
        touch_way_q <= i_fill_we ? i_fill_way : hit_way;
    end

    // pair0 picks within ways 0-1; with four ways, root picks the pair
    // and pair1 picks within ways 2-3. No reset: until a set is full the
    // invalid-first rule picks its victim, and filling it touches every
    // way, which writes every bit on the set's tree.
    logic [SETS-1:0]     plru_pair0_q;
    logic [WAY_BITS-1:0] lru_way;
    always_ff @(posedge i_clk)
        if (touch_q && (WAYS == 2 || !touch_way_q[WAY_BITS-1]))
            plru_pair0_q[touch_set_q] <= !touch_way_q[0];

    if (WAYS == 4) begin : g_plru4
        logic [SETS-1:0] plru_root_q, plru_pair1_q;
        always_ff @(posedge i_clk)
            if (touch_q) begin
                plru_root_q[touch_set_q] <= !touch_way_q[1];
                if (touch_way_q[1])
                    plru_pair1_q[touch_set_q] <= !touch_way_q[0];
            end
        assign lru_way = plru_root_q[rd_set] ? WAY_BITS'({1'b1, plru_pair1_q[rd_set]})
                                             : WAY_BITS'({1'b0, plru_pair0_q[rd_set]});
    end else begin : g_plru2
        assign lru_way = WAY_BITS'(plru_pair0_q[rd_set]);
    end

    always_comb begin
        o_victim_way = lru_way;
        for (int w = WAYS - 1; w >= 0; w--)
            if (!rd_valid[w]) o_victim_way = WAY_BITS'(w);
    end

    // ── Counters ─────────────────────────────────────────────────
    // The events are registered, so the hit reaches four flops' D rather
    // than the counters' enables. A disabled cache counts nothing.
    logic ev_read_hit_q, ev_read_miss_q, ev_write_hit_q, ev_write_miss_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ev_read_hit_q   <= 1'b0;
            ev_read_miss_q  <= 1'b0;
            ev_write_hit_q  <= 1'b0;
            ev_write_miss_q <= 1'b0;
        end else begin
            ev_read_hit_q   <= cache_en_q && i_count_read  &&  o_hit;
            ev_read_miss_q  <= cache_en_q && i_count_read  && !o_hit;
            ev_write_hit_q  <= cache_en_q && i_count_write &&  o_hit;
            ev_write_miss_q <= cache_en_q && i_count_write && !o_hit;
        end
    end

    logic [31:0] perf_rdata;
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

    // ── Sysreg read response (one cycle after the RDSYS launch) ──
    // Write-through, write-no-allocate, VIPT.
    localparam logic [31:0] INFO = {
        2'b0,               // [31:30] reserved
        1'b0,               // [29]    write-allocate
        1'b0,               // [28]    write-back
        CACHE_ADDR_VIPT,    // [27:26] addressing
        5'(WAYS),           // [25:21] ways
        15'(SETS),          // [20:6]  sets
        6'(LINE_WORDS)      // [5:0]   line words
    };

    always_comb begin
        case (rd_reg_q)
            SYSREG_CACHE_INFO: o_sys_rdata = INFO;
            SYSREG_CACHE_CTRL: o_sys_rdata = {31'b0, cache_en_q};
            default:           o_sys_rdata = perf_rdata;
        endcase
    end

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_fill_we && !cache_en_q))
        else $error("penumbra3_l1: a line installed into a disabled cache");

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_store && i_fill_we))
        else $error("penumbra3_l1: a store resolved on a fill install");

endmodule
