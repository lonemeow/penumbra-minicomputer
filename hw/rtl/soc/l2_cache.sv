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

// No keep_hierarchy: experimentally, letting the placer interleave
// L2's cells with the CPU produces ~1 MHz better fmax than locking
// L2 as a cluster — the placer uses the freedom productively here,
// even though clustering helped while L2 was bloated (136 EBRs and
// 18k LUTs of valid-bit decode fanout).  After the per-way + valid-
// in-BRAM rework, L2 is small enough (77 EBRs, ~6k LUTs) that
// scatter doesn't hurt and the placer's flexibility helps.
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

    // Flat index into a per-way data memory.  Concatenating set
    // (high) with word (low) gives a single (SET_BITS+WORD_BITS)-bit
    // address — each per-way memory is NUM_SETS*LINE_WORDS deep
    // × 32 wide, mapping cleanly to ECP5 EBRs in 1K×18 mode.
    // See the g_data generate block below for the per-way memory.
    function automatic logic [SET_BITS+WORD_BITS-1:0]
            data_idx(logic [SET_BITS-1:0] s, logic [WORD_BITS-1:0] w);
        return {s, w};
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

    // ready: becomes 1 after the post-reset auto-INVAL walk
    // completes.  Required because valid bits live in BRAM
    // (undefined contents at power-on on real hardware), so we
    // must clear them all before any tag compare can be trusted.
    // Pass-through (l2_active==0) is unaffected and works
    // immediately after reset.
    logic ready;

    // l2_active: combinational gate for "this access uses the
    // cache pipeline" (vs straight pass-through).  Gated on
    // `ready` so post-reset accesses pass-through harmlessly
    // until the valid-bit BRAMs are initialised.
    function automatic logic l2_active(logic cacheable);
        return l2_enable & cacheable & ready;
    endfunction

    // ══════════════════════════════════════════════════════════
    // Storage
    //
    // tags / data are large → BRAM.  valid is per-way BRAM too
    // (originally flops, but at 4 ways × NUM_SETS bits the
    // per-bit CE-decode dominated LUT count and congested routing
    // enough to drag fmax 1-2 MHz below the L2-off baseline).
    // plru stays in flops — 3 bits × NUM_SETS, with a parallel
    // read+update pattern that doesn't fit BRAM nicely.
    // ══════════════════════════════════════════════════════════
    (* ram_style = "block" *)
    logic [TAG_BITS-1:0] tags     [NUM_WAYS][NUM_SETS];

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
        end
    end

    // ══════════════════════════════════════════════════════════
    // Per-way data memory — must be SEPARATE named variables.
    //
    // The parallel-way read pattern (4 ways reading the same
    // (set, word) address simultaneously) lets yosys flatten the
    // logical [NUM_WAYS][...] array into one combined memory of
    // (NUM_WAYS * 32)-bit width that produces all way values in
    // one access.  On ECP5, a 128-bit-wide × 4096-deep memory has
    // to be implemented as 128 separate 1-bit-wide DP16KDs
    // (16K×1 mode) — 4× the EBR cost of the natural per-way
    // layout, plus an explosion of LUT4s for the bit-fanout
    // address decode and output mux trees.  We measured 128
    // DP16KDs + ~18000 LUT4s for the data array alone in the
    // 2D-array form; per-way separation drops this to 32 DP16KDs
    // (4 ways × 8 EBRs in 1K×18 mode) with negligible glue.
    //
    // Using a generate block creates NUM_WAYS *distinct* memory
    // variables (g_data[w].mem) that yosys cannot merge because
    // they are textually different netlist objects, even though
    // their access pattern is symmetric.  Same pattern the TLB
    // uses (way0_*/way1_* in tlb.sv), generalised here.
    // ══════════════════════════════════════════════════════════
    generate
        for (genvar gw = 0; gw < NUM_WAYS; gw++) begin : g_data
            (* ram_style = "block" *)
            logic [31:0] mem [NUM_SETS * LINE_WORDS];

            always_ff @(posedge i_clk) begin
                data_out[gw] <= mem[data_idx(addr_set(i_addr),
                                             addr_word(i_addr))];
                if (state == S_FILL && fill_in_flight && !i_mem_busy
                    && fill_way == WAY_BITS'(gw)) begin
                    mem[data_idx(fill_set,
                                 fill_word_idx[WORD_BITS-1:0])]
                        <= i_mem_rdata;
                end
            end
        end
    endgenerate

    // ══════════════════════════════════════════════════════════
    // Per-way valid bits — also separate generated variables so
    // yosys keeps them as 4 independent BRAMs.  Without this they
    // collapsed into a tight flop+LUT4 fabric (~16k LUT4 of
    // CE-decode and address fanout) that congested routing and
    // cost ~1 MHz of fmax on the ULX3S.
    //
    // Write port arbitration (one write per cycle per way):
    //   1. S_INVAL_ALL walker: clear mem[inval_walk_idx] in every way.
    //   2. S_FILL last-word install: set mem[fill_set] in fill_way.
    //   3. S_IDLE miss kickoff: clear the PLRU victim's mem[set].
    //   4. S_IDLE write hit: clear hit_way's mem[set].
    // Conditions are mutually exclusive (different states; or
    // s1_re vs s1_we in S_IDLE), so the priority encoder collapses.
    // ══════════════════════════════════════════════════════════
    generate
        for (genvar gw = 0; gw < NUM_WAYS; gw++) begin : g_valid
            (* ram_style = "block" *)
            logic mem [NUM_SETS];

            always_ff @(posedge i_clk) begin
                valid_out[gw] <= mem[addr_set(i_addr)];

                if (state == S_INVAL_ALL) begin
                    mem[inval_walk_idx] <= 1'b0;
                end else if (state == S_FILL && fill_in_flight && !i_mem_busy
                             && fill_word_idx == (WORD_BITS+1)'(LINE_WORDS - 1)
                             && fill_way == WAY_BITS'(gw)) begin
                    mem[fill_set] <= 1'b1;
                end else if (state == S_IDLE && s1_valid && s1_re && !hit
                             && plru_victim(plru[addr_set(s1_addr)])
                                == WAY_BITS'(gw)) begin
                    mem[addr_set(s1_addr)] <= 1'b0;
                end else if (state == S_IDLE && s1_valid && s1_we && hit
                             && hit_way == WAY_BITS'(gw)) begin
                    mem[addr_set(s1_addr)] <= 1'b0;
                end
            end
        end
    endgenerate

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
    // Stage-2 (output) registers — HIT_LATENCY=3.
    //
    // Breaks the combinational chain `tag_out → way_hit → hit →
    // o_busy → arbiter → cache_vipt's fill_state_logic →
    // valid[i].LSR` that becomes the critical path when HAS_L2=1
    // is enabled.  Per doc/internals/l2-cache.md the recommended
    // fix at this point is HIT_LATENCY=3 (one more pipeline
    // stage on the output side).
    //
    // We register the *meaning* of stage-1's result (hit,
    // hit_data, plus the s1_* qualifiers needed to mux o_busy/
    // o_rdata correctly) instead of registering the raw output
    // ports.  Registering the ports directly is fundamentally
    // wrong: the CPU's STALL logic samples o_busy in the same
    // cycle as i_re assertion, so an output flop with reset
    // value 0 looks like "not busy" before the first edge has
    // propagated the new request.
    //
    // The FSM still uses combinational `hit` so miss handling
    // and PLRU updates fire at stage-1 (no extra latency on the
    // critical fill path).
    // ══════════════════════════════════════════════════════════
    logic                hit_q;
    logic [31:0]         hit_data_q;
    logic                s1_valid_q;
    logic                s1_re_q;
    logic                s1_cacheable_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            hit_q          <= 1'b0;
            hit_data_q     <= 32'b0;
            s1_valid_q     <= 1'b0;
            s1_re_q        <= 1'b0;
            s1_cacheable_q <= 1'b0;
        end else begin
            hit_q          <= hit;
            hit_data_q     <= hit_data;
            s1_valid_q     <= s1_valid;
            s1_re_q        <= s1_re;
            s1_cacheable_q <= s1_cacheable;
        end
    end

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
            S_INVAL_ALL: begin
                // Post-reset auto-INVAL (ready==0): pass-through so
                // boot ROM and uncached MMIO work normally while the
                // walker initialises valid BRAMs in the background.
                //
                // Software-initiated INVAL (ready==1): don't drive
                // the bus at all.  The o_busy mux below holds every
                // memory access until the walk finishes — this is
                // the implicit synchronisation that means software
                // doesn't have to poll STATUS.busy before the next
                // memory operation.
                if (!ready) begin
                    if (!l2_active(i_cacheable) && (i_re || i_we)) begin
                        o_mem_re = i_re;
                        o_mem_we = i_we;
                    end
                end
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
        // BISECT: s2-q reverted to combinational hit/hit_data path
        // to test whether the s2-q pipeline is the source of the
        // dhrystone/memtest immediate-hang bug on RTL sim.  If
        // dhrystone passes after this, the bug is in s2-q timing.
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
                    // BISECT: reverted to combinational hit (was s2-q).
                    if (!s1_valid)
                        o_busy = 1'b1;
                    else if (hit)
                        o_busy = 1'b0;
                    else
                        o_busy = 1'b1;
                end else begin
                    o_busy = 1'b0;
                end
            end
            S_FILL:      o_busy = 1'b1;
            S_INVAL_ALL: begin
                // Two regimes:
                //   - !ready: post-reset auto-INVAL.  Pass-through
                //     mirrors i_mem_busy so boot ROM and uncached
                //     MMIO work during the ~NUM_SETS-cycle walk.
                //   - ready: software-initiated INVAL via WRSYS.
                //     Stall *every* memory access (cached and
                //     uncached) until the walk completes.  This is
                //     the implicit "fence" — code that issues
                //     WRSYS CACHE_INVAL_ALL doesn't have to poll
                //     STATUS.busy before the next load/store/MMIO;
                //     the very next memory op auto-waits.
                if (!ready) begin
                    if (!l2_active(i_cacheable) && (i_re || i_we))
                        o_busy = i_mem_busy;
                    else
                        o_busy = 1'b0;
                end else begin
                    if (i_re || i_we)
                        o_busy = 1'b1;
                    else
                        o_busy = 1'b0;
                end
            end
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
            // STATUS.busy: either software-requested INVAL_ALL is
            // walking, or the post-reset auto-INVAL hasn't finished
            // (ready still 0).  Both make the cache unsafe to
            // consult, so report busy in both cases.
            SYSREG_CACHE_STATUS: o_sys_rdata = {31'b0,
                                                ((state == S_INVAL_ALL) || !ready)};
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
            // valid BRAMs come up undefined on real HW.  Enter
            // S_INVAL_ALL immediately so the walker clears every
            // valid bit before any cached access can hit; until
            // it finishes, `ready` is 0 and l2_active() returns
            // false (pass-through serves uncached/disabled traffic).
            state           <= S_INVAL_ALL;
            ready           <= 1'b0;
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
                            // Victim's valid bit is dropped by the
                            // g_valid generate block on the same
                            // edge (its miss-kickoff write port
                            // condition mirrors this branch).
                        end else if (s1_we && hit) begin
                            // Write hit: invalidate the cached copy.
                            // The memory write is already in flight
                            // (drove o_mem_we in cycle 0).
                            // hit_way's valid bit is cleared by the
                            // g_valid generate block on the same edge.
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
                        // Data BRAM write happens in the per-way
                        // generate block above (g_data[gw].mem
                        // always_ff) — keyed on fill_way == gw.
                        fill_in_flight <= 1'b0;
                        if (fill_word_idx ==
                            (WORD_BITS+1)'(LINE_WORDS - 1)) begin
                            // Last word — install tag, update PLRU,
                            // return to S_IDLE.  fill_way's valid
                            // bit is set by the g_valid generate
                            // block on the same edge.  The CPU's
                            // request is still pending; the pipeline
                            // will re-serve it as a hit in the next
                            // 2 cycles.
                            tags [fill_way][fill_set] <= fill_tag;
                            plru[fill_set] <=
                                plru_update(plru[fill_set], fill_way);
                            state <= S_IDLE;
                        end else begin
                            fill_word_idx <= fill_word_idx + 1'b1;
                        end
                    end
                end

                S_INVAL_ALL: begin
                    // Walker steps inval_walk_idx through every set;
                    // the per-way g_valid generate block writes 0
                    // to all NUM_WAYS valid BRAMs at that index on
                    // the same edge.
                    if (inval_walk_idx == (SET_BITS)'(NUM_SETS - 1)) begin
                        inval_walk_wrap <= 1'b1;
                        state <= S_IDLE;
                        // Whether this was the post-reset auto-INVAL
                        // or a software-requested one, the cache is
                        // safe to use after this edge.  Setting
                        // `ready` is idempotent.
                        ready <= 1'b1;
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

    // The downstream bus may be driven during S_INVAL_ALL only
    // by pass-through (l2_active==0) — cached accesses don't
    // reach the bus while the walker is running.
    assert property (@(posedge i_clk) disable iff (i_rst)
        ((state == S_INVAL_ALL) && (o_mem_re || o_mem_we)) |->
            !l2_active(i_cacheable))
        else $error("l2_cache: cached bus access during INVAL_ALL walk");

    // fill_word_idx never overruns LINE_WORDS in S_FILL — the
    // last-word capture transitions state out of S_FILL before
    // we'd attempt to increment past the line size.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_FILL) |->
            (fill_word_idx <= (WORD_BITS+1)'(LINE_WORDS - 1)))
        else $error("l2_cache: fill_word_idx overran LINE_WORDS");

endmodule

// verilator lint_on UNUSEDSIGNAL
