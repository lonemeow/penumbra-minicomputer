// Penumbra L2 unified cache — phase 1
//
// Sits between cpu_core.o_mem_* and the shared system bus when
// HAS_L2=1 (the machine_sim / ulx3s_top generate selector).  When
// CTRL.enable=0 (default at reset) the module is a transparent
// pass-through; software must explicitly enable L2 after the
// caches and MMU are configured, matching how the existing L1
// caches are brought up.
//
// Geometry: 64 KiB, 4-way set-associative, 16-byte lines, PIPT
// (addresses are already physical past the MMU + L1).  Hits go
// through a 2-stage pipeline (BRAM read launched in cycle 0,
// tag compare + way mux + output drive in cycle 1) — this adds
// 1 cycle to every cached access vs L1, which is the price of
// using EBR-resident storage at 25 MHz.
//
// Write policy is **write-invalidate-on-hit**: cached writes are
// passed through to memory (write-through) and the L2 line is
// dropped if present.  This keeps the BRAM data port read-only
// past initialisation, which is materially simpler than full
// write-through-write-no-allocate's byte-enable update path; a
// follow-up phase can upgrade to WT-WNA once benchmarks show
// it's worth the bytes.
//
// Replacement: tree-PLRU, 3 bits/set.  At 4-way the miss rate is
// within noise of true LRU, and the update logic is 3 bit flips
// per access vs LRU's 6 pairwise relations.
//
// Uncacheable accesses (i_cacheable=0) skip the tag/data lookup
// entirely and are wired straight to the memory bus — same
// timing as the HAS_L2=0 build.  This is the contract the doc
// promises: "L2 introduces no extra cycle vs. the no-L2 build"
// for pass-through traffic.
//
// Sysreg device 9 (SYSDEV_L2) — same layout as DCACHE/ICACHE:
//   reg 0 INFO       (R)  unified cache INFO encoding (see penumbra_pkg.sv);
//                          INFO=0 means "no L2 present" (HAS_L2=0)
//   reg 1 CTRL       (RW) {31'b0, enable}; reset value 0 (disabled)
//   reg 2 INVAL_ALL  (W)  write triggers multi-cycle valid-bit walk
//   reg 6 STATUS     (R)  {31'b0, busy}; busy=1 while INVAL_ALL walks
//
// Write-invalidate-on-hit is *write-through* from software's POV
// (memory is always up-to-date), so INFO advertises WB=0, WA=0.
// The phase-2 upgrade to true write-back will flip these.
//
// See `doc/internals/l2-cache.md` for the full design plan
// (subsequent phases will add write-back, FLUSH ops, perfctrs).

// verilator lint_off UNUSEDSIGNAL

module l2_cache
    import penumbra_pkg::*;
#(
    parameter int CACHE_BYTES = 65536,
    parameter int LINE_BYTES  = 16,
    parameter int NUM_WAYS    = 4
)
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── CPU side (upstream — from cpu_core.o_mem_*) ────────
    input  logic [31:0] i_addr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    input  logic        i_cacheable,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Bus side (downstream — to bus_devsel / devices) ─────
    output logic [31:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,
    output logic [3:0]  o_mem_byte_en,
    output logic        o_mem_we,
    output logic        o_mem_re,
    input  logic [31:0] i_mem_rdata,
    input  logic        i_mem_busy,

    // ── Sysreg device 9 ────────────────────────────────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    output logic [31:0] o_sys_rdata
);

    // ══════════════════════════════════════════════════════════
    // Geometry
    // ══════════════════════════════════════════════════════════
    localparam int LINE_WORDS   = LINE_BYTES / 4;
    localparam int WORD_BITS    = $clog2(LINE_WORDS);
    localparam int WORD_LSB     = 2;
    localparam int OFFSET_BITS  = $clog2(LINE_BYTES);
    localparam int NUM_SETS     = CACHE_BYTES / (LINE_BYTES * NUM_WAYS);
    localparam int SET_BITS     = $clog2(NUM_SETS);
    localparam int TAG_BITS     = 32 - SET_BITS - OFFSET_BITS;
    localparam int WAY_BITS     = $clog2(NUM_WAYS);

    // Address helpers
    function automatic logic [WORD_BITS-1:0] addr_word(logic [31:0] a);
        return a[WORD_LSB +: WORD_BITS];
    endfunction
    function automatic logic [SET_BITS-1:0] addr_set(logic [31:0] a);
        return a[OFFSET_BITS +: SET_BITS];
    endfunction
    function automatic logic [TAG_BITS-1:0] addr_tag(logic [31:0] a);
        return a[OFFSET_BITS + SET_BITS +: TAG_BITS];
    endfunction

    // ══════════════════════════════════════════════════════════
    // Sysreg state
    // ══════════════════════════════════════════════════════════
    logic l2_enable;
    logic inval_all_req;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            l2_enable     <= 1'b0;
            inval_all_req <= 1'b0;
        end else begin
            inval_all_req <= 1'b0;  // 1-cycle pulse
            if (i_sys_we) begin
                case (i_sys_reg)
                    SYSREG_CACHE_CTRL:      l2_enable     <= i_sys_wdata[0];
                    SYSREG_CACHE_INVAL_ALL: inval_all_req <= 1'b1;
                    default: ;
                endcase
            end
        end
    end

    // l2_active: combinational gate for "this access uses the
    // cache pipeline" (vs straight pass-through).
    function automatic logic l2_active(logic cacheable);
        return l2_enable & cacheable;
    endfunction

    // ══════════════════════════════════════════════════════════
    // Storage
    //
    // tags / data are large → BRAM.  valid / plru are small
    // (1 bit/way and 3 bits/set respectively) → distributed RAM
    // or registers, easier to clear-all in S_INVAL_ALL.
    // ══════════════════════════════════════════════════════════
    (* ram_style = "block" *)
    logic [TAG_BITS-1:0] tags     [NUM_WAYS][NUM_SETS];
    (* ram_style = "block" *)
    logic [31:0]         data     [NUM_WAYS][NUM_SETS][LINE_WORDS];

    logic                valid    [NUM_WAYS][NUM_SETS];
    logic [2:0]          plru     [NUM_SETS];

    // ══════════════════════════════════════════════════════════
    // BRAM read outputs (registered → available next cycle)
    // ══════════════════════════════════════════════════════════
    logic [TAG_BITS-1:0] tag_out  [NUM_WAYS];
    logic [31:0]         data_out [NUM_WAYS];
    logic                valid_out[NUM_WAYS];

    always_ff @(posedge i_clk) begin
        for (int w = 0; w < NUM_WAYS; w++) begin
            tag_out[w]   <= tags [w][addr_set(i_addr)];
            data_out[w]  <= data [w][addr_set(i_addr)][addr_word(i_addr)];
            valid_out[w] <= valid[w][addr_set(i_addr)];
        end
    end

    // ══════════════════════════════════════════════════════════
    // Stage-1 registered request
    // ══════════════════════════════════════════════════════════
    logic [31:0]            s1_addr;
    logic                   s1_re;
    logic                   s1_we;
    logic                   s1_cacheable;
    logic                   s1_valid;

    // ── Stage-1 hit logic (combinational on s1_* + BRAM out) ──
    logic [NUM_WAYS-1:0]    way_hit;
    generate
        for (genvar w = 0; w < NUM_WAYS; w++) begin : g_hit
            assign way_hit[w] = valid_out[w] &&
                                (tag_out[w] == addr_tag(s1_addr));
        end
    endgenerate
    logic hit;
    assign hit = |way_hit;

    logic [WAY_BITS-1:0] hit_way;
    always_comb begin
        hit_way = '0;
        for (int w = 0; w < NUM_WAYS; w++)
            if (way_hit[w]) hit_way = WAY_BITS'(w);
    end

    logic [31:0] hit_data;
    assign hit_data = data_out[hit_way];

    // ══════════════════════════════════════════════════════════
    // Tree-PLRU
    //
    //         bit 0
    //         /   \
    //       0      1
    //      / \    / \
    //   way0 1  way2 3
    //   bit 1   bit 2
    //
    // bit b0: 0 = LRU is in (way0, way1), 1 = LRU is in (way2, way3)
    // bit b1: 0 = (in pair 0,1) LRU is way 0, 1 = LRU is way 1
    // bit b2: 0 = (in pair 2,3) LRU is way 2, 1 = LRU is way 3
    //
    // On access, flip bits along the path AWAY from the accessed
    // way → that way becomes MRU and the LRU pointer migrates
    // toward the un-touched subtree.
    // ══════════════════════════════════════════════════════════
    function automatic logic [WAY_BITS-1:0] plru_victim(logic [2:0] p);
        if (p[0] == 1'b0) return p[1] ? 2'd1 : 2'd0;
        else              return p[2] ? 2'd3 : 2'd2;
    endfunction

    function automatic logic [2:0] plru_update(logic [2:0] p,
                                               logic [WAY_BITS-1:0] way);
        logic [2:0] r;
        r = p;
        case (way)
            2'd0: begin r[0] = 1'b1; r[1] = 1'b1; end
            2'd1: begin r[0] = 1'b1; r[1] = 1'b0; end
            2'd2: begin r[0] = 1'b0; r[2] = 1'b1; end
            2'd3: begin r[0] = 1'b0; r[2] = 1'b0; end
            default: ;
        endcase
        return r;
    endfunction

    // ══════════════════════════════════════════════════════════
    // FSM
    // ══════════════════════════════════════════════════════════
    typedef enum logic [1:0] {
        S_IDLE,
        S_FILL,
        S_INVAL_ALL
    } state_t;
    state_t state;

    // Fill state.  fill_word_idx is the line-word currently being
    // fetched; fill_in_flight tracks whether a request has been
    // driven to the downstream bus and is awaiting its response.
    // The split is needed because the downstream bus uses the
    // busy-based handshake (no req_accepted pulse like the upstream
    // arbiter), so we can't gate increment on a single edge — we
    // need to distinguish "have I issued this word yet" from "have
    // I captured its response."
    logic [31:0]            fill_base_addr;
    logic [TAG_BITS-1:0]    fill_tag;
    logic [SET_BITS-1:0]    fill_set;
    logic [WAY_BITS-1:0]    fill_way;
    logic [WORD_BITS:0]     fill_word_idx;
    logic                   fill_in_flight;

    // INVAL_ALL walker
    logic [SET_BITS-1:0]    inval_walk_idx;
    logic                   inval_walk_wrap;

    // ══════════════════════════════════════════════════════════
    // Memory port output mux
    // ══════════════════════════════════════════════════════════
    always_comb begin
        o_mem_addr    = i_addr;
        o_mem_wdata   = i_wdata;
        o_mem_byte_en = i_byte_en;
        o_mem_we      = 1'b0;
        o_mem_re      = 1'b0;

        case (state)
            S_IDLE: begin
                if (!l2_active(i_cacheable) && (i_re || i_we)) begin
                    // Uncached pass-through
                    o_mem_re = i_re;
                    o_mem_we = i_we;
                end else if (i_we) begin
                    // Cached write: write-through (memory always sees
                    // the store; cycle-1 invalidate handles the cached
                    // copy if it was present).
                    o_mem_we = i_we;
                end
                // Cached read in S_IDLE: don't drive memory.  Hit
                // serves from BRAM; miss transitions to S_FILL which
                // drives the burst.
            end
            S_FILL: begin
                // Address layout: [31:OFFSET_BITS]=tag+set,
                // [OFFSET_BITS-1:WORD_LSB]=word_idx within line,
                // [WORD_LSB-1:0]=byte within word.
                o_mem_addr = {fill_base_addr[31:OFFSET_BITS],
                              fill_word_idx[WORD_BITS-1:0],
                              {WORD_LSB{1'b0}}};
                // Hold re=1 until we've issued the current word's
                // request.  Once in_flight, we no longer drive re
                // (so the slave doesn't re-capture the same addr
                // while we wait for its response).
                o_mem_re   = (fill_word_idx < (WORD_BITS+1)'(LINE_WORDS))
                             && !fill_in_flight;
            end
            default: ;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Output rdata
    //
    // On a cached read hit, drive from BRAM.  Otherwise pass
    // i_mem_rdata through — covers uncached reads, write-data
    // return (always i_mem_rdata, even though unused), and the
    // S_FILL response stream (cache_vipt above us captures live).
    // ══════════════════════════════════════════════════════════
    always_comb begin
        if (state == S_IDLE && s1_valid && l2_active(s1_cacheable)
            && s1_re && hit)
            o_rdata = hit_data;
        else
            o_rdata = i_mem_rdata;
    end

    // ══════════════════════════════════════════════════════════
    // Output busy
    //
    // S_IDLE:
    //   - Uncached/pass-through: mirror i_mem_busy
    //   - Cached write: mirror i_mem_busy (memory drives completion)
    //   - Cached read stage 0 (s1_valid=0, request live): busy=1
    //     while we launch the BRAM read
    //   - Cached read stage 1 (s1_valid=1): hit → 0, miss → 1 (will
    //     transition to S_FILL at this edge)
    // S_FILL: busy=1 throughout the burst
    // S_INVAL_ALL: busy=1 — block memory accesses until the walk
    //   finishes; software polls STATUS.busy via sysreg in the
    //   meantime, which uses a separate (zero-latency) channel.
    // ══════════════════════════════════════════════════════════
    always_comb begin
        case (state)
            S_IDLE: begin
                if (!l2_active(i_cacheable) && (i_re || i_we)) begin
                    o_busy = i_mem_busy;
                end else if (l2_active(i_cacheable) && i_we) begin
                    o_busy = i_mem_busy;
                end else if (l2_active(i_cacheable) && i_re) begin
                    if (!s1_valid)      o_busy = 1'b1;
                    else if (hit)        o_busy = 1'b0;
                    else                 o_busy = 1'b1;
                end else begin
                    o_busy = 1'b0;
                end
            end
            S_FILL:      o_busy = 1'b1;
            S_INVAL_ALL: o_busy = 1'b1;
            default:     o_busy = 1'b0;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Sysreg read mux
    // ══════════════════════════════════════════════════════════
    // Unified cache INFO encoding (matches cache_vipt.sv / cache.sv).
    // L2 is PIPT (we see post-translation addresses) and currently
    // write-through, write-no-allocate at the software-visible level.
    localparam logic [31:0] INFO_VALUE = {
        2'b0,                       // [31:30] reserved
        1'b0,                       // [29]    WRITE_ALLOC
        1'b0,                       // [28]    WRITE_BACK
        CACHE_ADDR_PIPT,            // [27:26] ADDRESSING
        5'(NUM_WAYS),               // [25:21] ways
        15'(NUM_SETS),              // [20:6]  sets
        6'(LINE_WORDS)              // [5:0]   line_words
    };

    always_comb begin
        o_sys_rdata = 32'b0;
        case (i_sys_reg)
            SYSREG_CACHE_INFO:   o_sys_rdata = INFO_VALUE;
            SYSREG_CACHE_CTRL:   o_sys_rdata = {31'b0, l2_enable};
            SYSREG_CACHE_STATUS: o_sys_rdata = {31'b0, (state == S_INVAL_ALL)};
            default:             o_sys_rdata = 32'b0;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Sequential — state, stage-1 registers, BRAM writes, PLRU
    // updates, INVAL_ALL walker
    // ══════════════════════════════════════════════════════════
    integer iw, is;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state           <= S_IDLE;
            s1_valid        <= 1'b0;
            s1_addr         <= 32'b0;
            s1_re           <= 1'b0;
            s1_we           <= 1'b0;
            s1_cacheable    <= 1'b0;
            fill_base_addr  <= 32'b0;
            fill_tag        <= '0;
            fill_set        <= '0;
            fill_way        <= '0;
            fill_word_idx   <= '0;
            fill_in_flight  <= 1'b0;
            inval_walk_idx  <= '0;
            inval_walk_wrap <= 1'b0;
            for (iw = 0; iw < NUM_WAYS; iw++)
                for (is = 0; is < NUM_SETS; is++)
                    valid[iw][is] <= 1'b0;
            for (is = 0; is < NUM_SETS; is++)
                plru[is] <= 3'b000;
        end else begin
            case (state)
                S_IDLE: begin
                    // ── Stage 0: latch a new cached request ──
                    // Only if not already holding stage 1, and the
                    // memory port is idle (so we don't re-latch the
                    // same in-flight cached write across multiple
                    // cycles while memory chews on it).
                    if (!s1_valid && !i_mem_busy
                        && l2_active(i_cacheable) && (i_re || i_we)) begin
                        s1_valid     <= 1'b1;
                        s1_addr      <= i_addr;
                        s1_re        <= i_re;
                        s1_we        <= i_we;
                        s1_cacheable <= i_cacheable;
                    end

                    // ── Stage 1: process whatever's pending ──
                    if (s1_valid) begin
                        s1_valid <= 1'b0;
                        if (s1_re && hit) begin
                            // Read hit: PLRU update; busy/rdata
                            // already driven combinationally above.
                            plru[addr_set(s1_addr)] <=
                                plru_update(plru[addr_set(s1_addr)],
                                            hit_way);
                        end else if (s1_re && !hit) begin
                            // Read miss → start fill.  Pick victim
                            // via PLRU.  fill_* takes over from s1;
                            // s1_valid was cleared at the top of
                            // this stage-1 block.
                            state          <= S_FILL;
                            fill_base_addr <= s1_addr;
                            fill_tag       <= addr_tag(s1_addr);
                            fill_set       <= addr_set(s1_addr);
                            fill_way       <= plru_victim(plru[addr_set(s1_addr)]);
                            fill_word_idx  <= '0;
                            fill_in_flight <= 1'b0;
                            // Drop the victim's valid bit immediately
                            // so any spurious hit during the fill
                            // window can't fire.
                            valid[plru_victim(plru[addr_set(s1_addr)])]
                                 [addr_set(s1_addr)] <= 1'b0;
                        end else if (s1_we && hit) begin
                            // Write hit: invalidate the cached copy.
                            // The memory write is already in flight
                            // (drove o_mem_we in cycle 0).
                            valid[hit_way][addr_set(s1_addr)] <= 1'b0;
                        end
                    end

                    // ── Handle INVAL_ALL trigger from sysreg ──
                    if (inval_all_req && !s1_valid) begin
                        state           <= S_INVAL_ALL;
                        inval_walk_idx  <= '0;
                        inval_walk_wrap <= 1'b0;
                    end
                end

                S_FILL: begin
                    // Two phases per word:
                    //   - !in_flight && word<LINE_WORDS: the bus
                    //     side is driving re=1 with the current
                    //     word's addr.  Slave captures this cycle
                    //     (it sees re=1 while idle); set
                    //     in_flight=1 to remember.
                    //   - in_flight && !i_mem_busy: response just
                    //     arrived.  Capture into BRAM, advance
                    //     word_idx (or install on last word).
                    if (!fill_in_flight
                        && fill_word_idx < (WORD_BITS+1)'(LINE_WORDS)) begin
                        fill_in_flight <= 1'b1;
                    end else if (fill_in_flight && !i_mem_busy) begin
                        data[fill_way][fill_set]
                            [fill_word_idx[WORD_BITS-1:0]]
                            <= i_mem_rdata;
                        fill_in_flight <= 1'b0;
                        if (fill_word_idx ==
                            (WORD_BITS+1)'(LINE_WORDS - 1)) begin
                            // Last word — install tag/valid, update
                            // PLRU to make this way MRU, return to
                            // S_IDLE.  The CPU's request is still
                            // pending; the pipeline will re-serve
                            // it as a hit in the next 2 cycles.
                            tags [fill_way][fill_set] <= fill_tag;
                            valid[fill_way][fill_set] <= 1'b1;
                            plru[fill_set] <=
                                plru_update(plru[fill_set], fill_way);
                            state <= S_IDLE;
                        end else begin
                            fill_word_idx <= fill_word_idx + 1'b1;
                        end
                    end
                end

                S_INVAL_ALL: begin
                    // Clear all valid bits across all sets/ways
                    // one set per cycle.
                    for (iw = 0; iw < NUM_WAYS; iw++)
                        valid[iw][inval_walk_idx] <= 1'b0;
                    if (inval_walk_idx == (SET_BITS)'(NUM_SETS - 1)) begin
                        inval_walk_wrap <= 1'b1;
                        state <= S_IDLE;
                    end else begin
                        inval_walk_idx <= inval_walk_idx + 1'b1;
                    end
                end

                default: state <= S_IDLE;
            endcase
        end
    end

    // ══════════════════════════════════════════════════════════
    // Simulation assertions
    // ══════════════════════════════════════════════════════════

    // The downstream bus must be quiet outside S_FILL and the
    // pass-through paths.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_INVAL_ALL) |-> (!o_mem_re && !o_mem_we))
        else $error("l2_cache: bus driven during INVAL_ALL walk");

    // fill_word_idx never overruns LINE_WORDS in S_FILL — the
    // last-word capture transitions state out of S_FILL before
    // we'd attempt to increment past the line size.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_FILL) |->
            (fill_word_idx <= (WORD_BITS+1)'(LINE_WORDS - 1)))
        else $error("l2_cache: fill_word_idx overran LINE_WORDS");

endmodule

// verilator lint_on UNUSEDSIGNAL
