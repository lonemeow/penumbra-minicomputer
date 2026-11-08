// penumbra3_l1 -- one gen3 L1 cache: storage, the hit verdict, and the
// side's access to the uncore.
//
// penumbra3_l1_store's arrays and penumbra3_l1_match's compare, plus what
// makes them a cache: the store-hit write, replacement, the enable, the
// cache device's sysreg registers, and penumbra3_requester, through which
// the cache carries everything that goes past it on its side of the
// core-to-uncore port. Its client sees lookups, stores and accesses
// (doc/internals/penumbra3/l1-cache.md); which way a line goes to, whether
// it goes in at all, and the enable stay in here. One module serves both
// sides; the I-side ties the store and the write fields off.
//
// Lookups. A lookup takes two cycles. In the first, i_launch reads the set
// the page offset indexes, in every way. In the second the MMU's candidate
// frames arrive and the outputs give the verdict, combinationally: the hit
// per way, each way's word, and the hit way's word. The outputs go on
// belonging to that lookup, the current lookup, until the next launch, and
// its set is read again every cycle: a frozen client keeps a verdict that
// follows the cache, through an invalidate, a store or a line installed
// under it.
//
// Stores. i_store marks a committing store whose lookup is the current
// one. On a hit the cache merges the store's bytes over the hit way's word
// and writes the whole word in the next cycle, from registers: the hit
// reaches a register's D and never a block RAM's write enable. A lookup of
// that word is given it from the cycle of the write on, ahead of the block
// RAM's own output (penumbra3_l1_store), so a load directly behind a store
// needs no stall. A miss allocates nothing (write-no-allocate); the write
// to memory is the store's access. A store's hit is not qualified by the
// page's cacheability: a line can be resident for an uncacheable mapping
// only if software changed the page's cacheability without invalidating
// the cache.
//
// Accesses. The client requests one (i_access_req) in the resolve cycle of
// its own lookup, or with no lookup at all, and the cache carries it as the
// port transaction its write bit and cacheability name. A line read's line
// goes in as the read completes. o_busy is high from the cycle after the
// request until the cache's lookups show what the access did, and the
// client launches, requests and stores nothing meanwhile. One access, cycle
// by cycle; q is the request, r the uncore's done:
//
//   q       i_access_req: the access is latched
//   q+1..r  o_busy; the request is up on the port
//   r       a line read's line is installed
//   r+1     o_access_done; o_access_word and o_access_fault hold the
//           result until the next done. o_busy is low, unless a held
//           lookup reads the set the install wrote: then it is low from r+2
//
// A held lookup is one the client launched and has neither discarded
// (i_discard) nor requested an access from. The arrays give the way a fill
// writes as it was for one cycle after the fill, so a held lookup of the
// filled set needs that cycle, and o_busy covers it.
//
// Replacement: an invalid way first, lowest index first, else tree-PLRU.
// Each tree bit names the less recently used side below it, and a touch
// only writes the bits on its way's path, never reading the old ones. A
// touch lands two cycles after the hit or install it records, off the hit
// path. The victim is read from the current lookup's set, so an access
// latches the way its own lookup would fill.
//
// Disabled (CTRL.ENABLE=0, the reset state): a lookup reads every way
// invalid, so nothing hits and no store is absorbed. The enable folds into
// the valid bits as a lookup launches, not into the compare. Lines installed
// before a disable survive it; software pairs an enable change with
// INVAL_ALL. A disabled cache still reads a cacheable page's line, and
// installs nothing (doc/internals/penumbra3/uncore-port.md, "The four
// transactions").
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

    // ── Lookup: launch ───────────────────────────────────────────
    input  logic                         i_launch,     // a real access, not while busy
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [11:0]                  i_offset,     // the access's page offset; its byte bits index nothing
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic                         i_discard,    // the current lookup is flushed

    // ── Lookup: resolve ──────────────────────────────────────────
    input  logic [FRAMES-1:0][19:0]      i_frame,      // the MMU's candidate frames
    input  logic [FRAMES-1:0]            i_frame_sel,  // one-hot; empty on a TLB miss
    output logic [WAYS-1:0]              o_way_hit,    // one-hot
    output logic [WAYS-1:0][31:0]        o_way_word,
    output logic                         o_hit,
    output logic [31:0]                  o_word,       // the hit way's word

    // ── Store: a committing store resolving this cycle ───────────
    input  logic                         i_store,
    input  logic [31:0]                  i_store_wdata,
    input  logic [3:0]                   i_store_byte_en,

    // ── Counting: once per cacheable access, in a resolve cycle ──
    input  logic                         i_count_read,
    input  logic                         i_count_write,

    // ── Access: what goes past the cache ─────────────────────────
    input  logic                         i_access_req,       // one cycle, not while busy
    input  logic                         i_access_we,        // a write
    input  logic [31:0]                  i_access_addr,      // physical
    input  logic                         i_access_cacheable, // the translation's verdict
    input  logic [3:0]                   i_access_byte_en,
    input  logic [31:0]                  i_access_wdata,     // each byte in its lane
    output logic                         o_busy,
    output logic                         o_access_done,      // one cycle
    output logic [31:0]                  o_access_word,      // held to the next done
    output logic                         o_access_fault,     // the bus faulted the access

    // ── Core-to-uncore port, this side ───────────────────────────
    output logic                         o_req_valid,
    output logic                         o_req_cacheable,
    output logic [31:0]                  o_req_addr,
    output logic                         o_req_we,
    output logic [3:0]                   o_req_byte_en,
    output logic [31:0]                  o_req_wdata,
    input  logic                         i_rsp_done,   // one cycle, while o_req_valid
    input  logic                         i_rsp_fault,  // valid with i_rsp_done
    input  logic [LINE_WORDS-1:0][31:0]  i_rsp_line,

    // ── Sysreg interface (this cache's device) ───────────────────
    input  logic [3:0]                   i_sys_reg,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0]                  i_sys_wdata,  // CTRL holds only the enable
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic                         i_sys_we,
    input  logic                         i_sys_re,
    output logic [31:0]                  o_sys_rdata
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

    // The install, from the access below.
    logic                  fill_we;
    logic [WAY_BITS-1:0]   fill_way;
    // fill_paddr is the access's address, whose offset bits index nothing.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [31:0]           fill_paddr;
    /* verilator lint_on UNUSEDSIGNAL */
    logic [SET_BITS-1:0]   fill_set;
    assign fill_set = fill_paddr[OFFSET_BITS +: SET_BITS];

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
        .i_fill_we     (fill_we),
        .i_fill_set    (fill_set),
        .i_fill_way    (fill_way),
        .i_fill_tag    (fill_paddr[31:12]),
        .i_fill_line   (i_rsp_line),
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
            touch_q       <= fill_we || (first_cycle_q && o_hit);
        end
        touch_set_q <= fill_we ? fill_set : rd_set;
        touch_way_q <= fill_we ? fill_way : hit_way;
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

    // victim_way: where a line the current lookup missed would go.
    logic [WAY_BITS-1:0] victim_way;
    always_comb begin
        victim_way = lru_way;
        for (int w = WAYS - 1; w >= 0; w--)
            if (!rd_valid[w]) victim_way = WAY_BITS'(w);
    end

    // ── The access ───────────────────────────────────────────────
    // The request is the access's own lookup's, so the victim the
    // requester latches with it is the way that lookup's line would fill.
    penumbra3_requester #(
        .WAYS       (WAYS),
        .LINE_WORDS (LINE_WORDS)
    ) u_request (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_issue         (i_access_req),
        .i_we            (i_access_we),
        .i_addr          (i_access_addr),
        .i_cacheable     (i_access_cacheable),
        .i_byte_en       (i_access_byte_en),
        .i_wdata         (i_access_wdata),
        .i_victim_way    (victim_way),
        .i_enabled       (cache_en_q),
        .o_req_valid     (o_req_valid),
        .o_req_cacheable (o_req_cacheable),
        .o_req_addr      (o_req_addr),
        .o_req_we        (o_req_we),
        .o_req_byte_en   (o_req_byte_en),
        .o_req_wdata     (o_req_wdata),
        .i_rsp_done      (i_rsp_done),
        .i_rsp_fault     (i_rsp_fault),
        .i_rsp_line      (i_rsp_line),
        .o_fill_we       (fill_we),
        .o_fill_way      (fill_way),
        .o_fill_paddr    (fill_paddr),
        .o_rsp_word      (o_access_word),
        .o_rsp_fault     (o_access_fault)
    );

    // ── Busy and done ────────────────────────────────────────────
    // held_q: the current lookup is held -- launched, and since then
    // neither discarded nor requested from. A launch in the request's
    // cycle is the next lookup's, which is held. An access with no lookup
    // releases the current one too: a client holds no lookup across one.
    logic held_q;
    always_ff @(posedge i_clk)
        if (i_rst)                         held_q <= 1'b0;
        else if (i_launch)                 held_q <= 1'b1;
        else if (i_discard || i_access_req) held_q <= 1'b0;

    // held_late: this cycle's install lands in the set a held lookup
    // reads, which shows the filled way as it was for one cycle more
    // (penumbra3_l1_store). No lookup launches while the cache is busy,
    // so rd_set is the held lookup's.
    logic held_late;
    assign held_late = fill_we && fill_set == rd_set && held_q;

    // busy_q: high from q+1 through r, or through r+1 when held_late was
    // high in r. late_q: r+1 of such an access.
    logic busy_q, late_q, done_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            busy_q <= 1'b0;
            late_q <= 1'b0;
            done_q <= 1'b0;
        end else begin
            late_q <= held_late;
            done_q <= i_rsp_done;

            if (i_access_req)                  busy_q <= 1'b1;
            else if (i_rsp_done && !held_late) busy_q <= 1'b0;
            else if (late_q)                   busy_q <= 1'b0;
        end
    end
    assign o_busy        = busy_q;
    assign o_access_done = done_q;

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
    // The client launches, requests and stores nothing while the cache is
    // busy: an install never meets a launch or a store, and one access is
    // in flight at a time.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_busy && (i_launch || i_access_req || i_store)))
        else $error("penumbra3_l1: a launch, an access or a store while busy");

endmodule
