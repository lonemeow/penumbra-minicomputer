// Penumbra L2 unified cache — phase 1
//
// Sits between cpu_core.o_mem_* and the shared system bus, always
// instantiated by machine_sim and ulx3s_penumbra1_top.  When CTRL.enable=0
// (default at reset) the module is a transparent pass-through;
// software must explicitly enable L2 after the caches and MMU are
// configured, matching how the existing L1 caches are brought up.
//
// Geometry: 64 KiB, 4-way set-associative, 16-byte lines, PIPT
// (addresses are already physical past the MMU + L1).  Hits go
// through a 2-stage pipeline (BRAM read launched in cycle 0,
// tag compare + way mux + output drive in cycle 1) — this adds
// 1 cycle to every cached access vs L1, which is the price of
// using EBR-resident storage at 25 MHz.
//
// Write policy is **write-through, write-no-allocate (WT-WnA)**:
// cached writes are forwarded to memory unconditionally; on a tag
// hit the cached line's data is also updated in place via
// byte-enable, keeping the line valid for subsequent reads.  On
// a tag miss the store passes through unchanged (no allocation).
// The phase-1 predecessor was write-invalidate-on-hit, which
// dropped the cached line on write hit — same software-visible
// contract (memory is always up to date) but caused hot read-
// modify-write lines to self-evict from L2.  Phase 2 (planned)
// upgrades to write-back, write-allocate to absorb the writes
// themselves.
//
// Replacement: tree-PLRU, 3 bits/set.  At 4-way the miss rate is
// within noise of true LRU, and the update logic is 3 bit flips
// per access vs LRU's 6 pairwise relations.
//
// Uncacheable accesses (i_cacheable=0) skip the tag/data lookup
// entirely and are wired straight to the memory bus — same timing
// as the disabled/pass-through path.  This is the contract the doc
// promises: "L2 introduces no extra cycle vs. the no-L2 build"
// for pass-through traffic.
//
// Sysreg device 9 (SYSDEV_L2_CACHE) — same layout as L1_DCACHE/L1_ICACHE:
//   reg 0 INFO       (R)  unified cache INFO encoding (see penumbra_pkg.sv);
//                          INFO=0 means "no L2 present" (only a
//                          future variant that drops this instance)
//   reg 1 CTRL       (RW) {31'b0, enable}; reset value 0 (disabled)
//   reg 2 INVAL_ALL  (W)  write triggers multi-cycle valid-bit walk
//   reg 6 STATUS     (R)  {31'b0, busy}; busy=1 while INVAL_ALL walks
//
// WT-WnA is software-visibly write-through, write-no-allocate,
// so INFO advertises WB=0, WA=0.  The phase-2 upgrade to true
// write-back will flip these.
//
// See `doc/internals/l2-cache.md` for the full design plan
// (subsequent phases will add write-back, write-allocate,
// FLUSH ops; perfctrs landed alongside the cache_perfctr module).

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
    parameter int NUM_WAYS    = 4,
    // Read-hit latency. 2: the hit resolves combinationally at stage 1 (busy
    // and rdata come off the live tag compare) — one fewer cycle, but the hit
    // verdict crosses combinationally into the consumer's fill/install logic.
    // 3: the verdict is registered at stage 2 before it leaves the cache,
    // breaking that chain at the cost of one cycle and back-to-back read
    // throughput (II 2→3). Set 3 where the L2→L1 hit chain is the fmax limiter
    // (gen2); keep 2 where the limiter is elsewhere (gen1's own core).
    parameter int HIT_LATENCY = 2
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
    output logic        o_fault,    // access fault, coincident with the front-side completion

    // ── Bus side (downstream — to bus_devsel / devices) ─────
    output logic [31:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,
    output logic [3:0]  o_mem_byte_en,
    output logic        o_mem_we,
    output logic        o_mem_re,
    input  logic [31:0] i_mem_rdata,
    input  logic        i_mem_busy,
    input  logic        i_mem_fault,    // bus-side access fault, coincident with i_mem_busy drop

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
    //
    // Per-way data BRAM has two write paths:
    //   1. S_FILL response capture: writes one word per beat of
    //      the fill burst into (fill_set, fill_word_idx), only on
    //      the way being filled.
    //   2. S_IDLE write hit (phase 1.5 WT-WnA): byte-en update of
    //      the cached line at (s1_addr) on the hit_way.  The store
    //      itself is already in flight on the downstream bus (the
    //      output mux drove o_mem_we in cycle 0); this updates the
    //      cached copy so subsequent reads see the new bytes
    //      without paying an SDRAM round-trip.  Conditions are
    //      mutually exclusive (S_FILL vs S_IDLE), so the priority
    //      encoder collapses.
    generate
        for (genvar gw = 0; gw < NUM_WAYS; gw++) begin : g_data
            (* ram_style = "block" *)
            logic [31:0] mem [NUM_SETS * LINE_WORDS];

            always_ff @(posedge i_clk) begin
                data_out[gw] <= mem[data_idx(addr_set(i_addr),
                                             addr_word(i_addr))];
                if (fill_beat && fill_way == WAY_BITS'(gw)) begin
                    mem[data_idx(fill_set,
                                 fill_word_idx[WORD_BITS-1:0])]
                        <= i_mem_rdata;
                end else if (state == S_IDLE && s1_valid && s1_we
                             && hit && hit_way == WAY_BITS'(gw)) begin
                    if (s1_byte_en[0])
                        mem[data_idx(addr_set(s1_addr),
                                     addr_word(s1_addr))][ 7: 0]
                            <= s1_wdata[ 7: 0];
                    if (s1_byte_en[1])
                        mem[data_idx(addr_set(s1_addr),
                                     addr_word(s1_addr))][15: 8]
                            <= s1_wdata[15: 8];
                    if (s1_byte_en[2])
                        mem[data_idx(addr_set(s1_addr),
                                     addr_word(s1_addr))][23:16]
                            <= s1_wdata[23:16];
                    if (s1_byte_en[3])
                        mem[data_idx(addr_set(s1_addr),
                                     addr_word(s1_addr))][31:24]
                            <= s1_wdata[31:24];
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
    // Conditions are mutually exclusive (different states; or
    // different `gw` selection in S_IDLE), so the priority encoder
    // collapses.  Phase 1.5 dropped the "S_IDLE write hit clears
    // hit_way's valid bit" path that used to live here — write
    // hits now update the cached data via g_data (above) and leave
    // the valid bit alone.
    // ══════════════════════════════════════════════════════════
    generate
        for (genvar gw = 0; gw < NUM_WAYS; gw++) begin : g_valid
            (* ram_style = "block" *)
            logic mem [NUM_SETS];

            always_ff @(posedge i_clk) begin
                valid_out[gw] <= mem[addr_set(i_addr)];

                if (state == S_INVAL_ALL) begin
                    mem[inval_walk_idx] <= 1'b0;
                end else if (fill_install && fill_way == WAY_BITS'(gw)) begin
                    mem[fill_set] <= 1'b1;
                end else if (state == S_IDLE && s1_valid && s1_re && !hit
                             && plru_victim(plru[addr_set(s1_addr)])
                                == WAY_BITS'(gw)) begin
                    mem[addr_set(s1_addr)] <= 1'b0;
                end
            end
        end
    endgenerate

    // ══════════════════════════════════════════════════════════
    // Stage-1 registered request
    // ══════════════════════════════════════════════════════════
    logic [31:0]            s1_addr;
    logic [31:0]            s1_wdata;
    logic [3:0]             s1_byte_en;
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
    // valid[i].LSR` that became the critical path once the L2 was
    // wired in.  Per doc/internals/l2-cache.md the recommended fix
    // at this point is HIT_LATENCY=3 (one more pipeline stage on
    // the output side).
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
    // critical fill path); only the read-hit *output* is registered.
    //
    // When HIT_LATENCY=3 these flops drive the o_rdata/o_busy muxes: the
    // read verdict is registered before it crosses into the arbiter and
    // the L1 fill install, breaking the tag_out→hit→…→valid[i] chain that
    // was the gen2 critical path. Cost: a read hit takes one more cycle,
    // and — until the read pipeline is decoupled — back-to-back L2 reads
    // serialise (a fill-throughput hit on L1 misses), traded for fmax. At
    // HIT_LATENCY=2 (the default) the muxes read the live stage-1 verdict
    // and these flops are pruned.
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
    // Carries a guard bit above the WORD_BITS an in-line index needs, so a
    // walk that overruns the line lands out of range instead of wrapping
    // silently back to word 0 — the overrun assertion below reads it.
    logic [WORD_BITS:0]     fill_word_idx;
    logic                   fill_in_flight;

    // The line's final word index, typed to match the counter it is
    // compared against.
    localparam logic [WORD_BITS:0] LAST_FILL_WORD =
        (WORD_BITS+1)'(LINE_WORDS - 1);

    // A fill response word landing: the downstream bus has answered the
    // address presented for the word currently in flight, so this cycle
    // captures it.  fill_install narrows that to the line's last word —
    // the cycle that commits the tag and the valid bit.
    logic                   fill_beat;
    logic                   fill_install;
    assign fill_beat    = (state == S_FILL) && fill_in_flight && !i_mem_busy;
    assign fill_install = fill_beat && (fill_word_idx == LAST_FILL_WORD);

    // Deferred PLRU touch: a read hit captures its set + way here and the
    // plru read-modify-write applies one cycle later, off the combinational
    // hit / hit_way path. A read hit never changes state, so the apply always
    // lands back in S_IDLE.
    //
    // Read hits are the only producer, which is what keeps the plru array's
    // per-set clock-enable a decode of registered signals — no combinational
    // hit verdict and no downstream i_mem_busy reaches it. A freshly filled
    // line gets its touch from the re-serve hit that follows the install.
    logic                   plru_touch;
    logic [SET_BITS-1:0]    plru_touch_set;
    logic [WAY_BITS-1:0]    plru_touch_way;

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
                    // the store; if the line is in the cache, the
                    // cycle-1 byte-en update in g_data refreshes
                    // the cached copy).
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
                // Bus master must hold re asserted for the entire
                // fill burst
                o_mem_re   = 1'b1;
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
    // Hit-resolution signals selected by latency: HIT_LATENCY>=3 reads the
    // registered stage-2 verdict (off the L2→L1 chain); 2 the live stage-1
    // signals. The unused set is pruned at elaboration.
    logic        rsv_valid, rsv_re, rsv_cacheable, rsv_hit;
    logic [31:0] rsv_data;
    always_comb begin
        if (HIT_LATENCY >= 3) begin
            rsv_valid     = s1_valid_q;
            rsv_re        = s1_re_q;
            rsv_cacheable = s1_cacheable_q;
            rsv_hit       = hit_q;
            rsv_data      = hit_data_q;
        end else begin
            rsv_valid     = s1_valid;
            rsv_re        = s1_re;
            rsv_cacheable = s1_cacheable;
            rsv_hit       = hit;
            rsv_data      = hit_data;
        end
    end

    always_comb begin
        // Drive o_rdata from the (latency-selected) resolved read hit; at
        // HIT_LATENCY=3 that is the flopped stage-2 verdict, so the read
        // result leaves the cache registered.
        if (state == S_IDLE && rsv_valid && l2_active(rsv_cacheable)
            && rsv_re && rsv_hit)
            o_rdata = rsv_data;
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
                    // Cached read: busy until the hit resolves. The resolve
                    // signals (rsv_*) are the live stage-1 verdict at
                    // HIT_LATENCY=2 and the registered stage-2 verdict at 3 —
                    // so at 3 the hit cone ends in a flop and never reaches
                    // o_busy. A miss leaves for S_FILL, so a resolved in-S_IDLE
                    // read is always the hit.
                    o_busy = !(rsv_valid && rsv_re && rsv_hit);
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
    // Output fault
    //
    // A bus-side fault rides the front-side completion only on the
    // pass-through path: an uncached/disabled access or a cached
    // write-through forwards the beat to the bus, so its fault forwards
    // the same cycle (o_busy == i_mem_busy there).  A cached read hit
    // completes locally with no bus access, so it can never fault.  A
    // fault on L2's own line fill from the bus (S_FILL) is a separate
    // abort case — not yet handled (see the assertion below) — so the
    // fill state is excluded here rather than mis-forwarded as a beat.
    // ══════════════════════════════════════════════════════════
    assign o_fault = i_mem_fault & (state != S_FILL);

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

    // Perfctr read-side fans in below — declared up front so the
    // sysreg read mux can fall through to it.
    logic [31:0] perfctr_rdata;

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
            default:             o_sys_rdata = perfctr_rdata;
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
            s1_wdata        <= 32'b0;
            s1_byte_en      <= 4'b0;
            s1_re           <= 1'b0;
            s1_we           <= 1'b0;
            s1_cacheable    <= 1'b0;
            fill_base_addr  <= 32'b0;
            fill_tag        <= '0;
            fill_set        <= '0;
            fill_way        <= '0;
            fill_word_idx   <= '0;
            fill_in_flight  <= 1'b0;
            plru_touch      <= 1'b0;
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
                    // Apply a PLRU touch captured last cycle; a one-cycle
                    // pulse. Every plru write lands here, so the array's
                    // clock-enable is a set decode of registered signals —
                    // neither the combinational hit path nor the downstream
                    // bus's i_mem_busy reaches it. That is the cut which keeps
                    // the NUM_SETS-wide flop array off both critical paths.
                    plru_touch <= 1'b0;
                    if (plru_touch)
                        plru[plru_touch_set] <=
                            plru_update(plru[plru_touch_set], plru_touch_way);

                    // ── Stage 0: latch a new cached request ──
                    // Only if not already holding stage 1, and the
                    // memory port is idle (so we don't re-latch the
                    // same in-flight cached write across multiple
                    // cycles while memory chews on it).
                    // At HIT_LATENCY=3 also gate on !s1_valid_q: don't re-latch
                    // the still-held request while its stage-2 verdict is being
                    // presented (that would issue a phantom second access of
                    // the same line). At 2 there is no stage-2 hold, so
                    // s1_valid alone suffices (and II stays 2).
                    if (!s1_valid && (HIT_LATENCY < 3 || !s1_valid_q)
                        && !i_mem_busy
                        && l2_active(i_cacheable) && (i_re || i_we)) begin
                        s1_valid     <= 1'b1;
                        s1_addr      <= i_addr;
                        s1_wdata     <= i_wdata;
                        s1_byte_en   <= i_byte_en;
                        s1_re        <= i_re;
                        s1_we        <= i_we;
                        s1_cacheable <= i_cacheable;
                    end

                    // ── Stage 1: process whatever's pending ──
                    if (s1_valid) begin
                        s1_valid <= 1'b0;
                        if (s1_re && hit) begin
                            // Read hit: capture the PLRU touch (set + way) and
                            // apply it next cycle (above), off the combinational
                            // hit / hit_way path. busy/rdata already driven
                            // combinationally above. This also carries the
                            // touch for a line installed by the fill that
                            // this access is being re-served from.
                            plru_touch     <= 1'b1;
                            plru_touch_set <= addr_set(s1_addr);
                            plru_touch_way <= hit_way;
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
                            // Write hit (phase 1.5 WT-WnA): the
                            // store itself is already in flight on
                            // the downstream bus (the bus output mux
                            // drove o_mem_we in cycle 0).  The
                            // cached copy is updated by g_data's
                            // byte-en write port on the same edge —
                            // no FSM state change here.
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
                        && fill_word_idx <= LAST_FILL_WORD) begin
                        fill_in_flight <= 1'b1;
                    end else if (fill_beat) begin
                        // Data BRAM write happens in the per-way
                        // generate block above (g_data[gw].mem
                        // always_ff) — keyed on fill_way == gw.
                        fill_in_flight <= 1'b0;
                        if (fill_word_idx == LAST_FILL_WORD) begin
                            // Last word — install tag and return to
                            // S_IDLE.  fill_way's valid bit is set by
                            // the g_valid generate block on the same
                            // edge.  The CPU's request is still
                            // pending; the pipeline will re-serve it
                            // as a hit in the next 2 cycles.
                            //
                            // Nothing touches plru here.  That re-serve
                            // hits on the way just installed, so it
                            // carries the touch for this line — and it
                            // carries it off i_mem_busy, which gates
                            // this branch and would otherwise put the
                            // whole downstream device decode in front
                            // of the plru array's per-set clock-enable.
                            tags [fill_way][fill_set] <= fill_tag;
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
            (fill_word_idx <= LAST_FILL_WORD))
        else $error("l2_cache: fill_word_idx overran LINE_WORDS");

    // An install writes no PLRU state of its own: it relies on the re-serve
    // that follows to hit the just-installed way and touch it.  Should that
    // re-serve ever stop happening, the line would sit at the head of its own
    // victim order and thrash — silently.  Pin it here instead.
    // fill_reserve_pending marks exactly that window, so its re-serve cycle
    // is the one that must capture the touch.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (fill_reserve_pending && state == S_IDLE && s1_valid && s1_re)
            |=> plru_touch)
        else $error("l2_cache: fill install not followed by a re-serve PLRU touch");

    // Deferred: a bus fault during L2's own line fill (cacheable miss while
    // L2 is enabled) is not yet aborted/propagated — it would install a
    // garbage line.  Reachable only with L2 enabled AND a cacheable mapping
    // to an unclaimed address; pass-through faults (the live path) never
    // enter S_FILL.  Fail loudly here until the L2 fill-abort lands.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_FILL) |-> !i_mem_fault)
        else $error("l2_cache: bus fault during line fill — L2 fill-abort not implemented");

    // ══════════════════════════════════════════════════════════
    // Performance counters
    // ══════════════════════════════════════════════════════════
    // Pulse derivation for the 2-cycle L2 pipeline.  The signal
    // landscape worth knowing:
    //
    //   * Stage 0 (cycle N) latches a new request into s1_*
    //     registers (line 627-634).  Latching gate already includes
    //     l2_active(i_cacheable) — so s1_valid==1 implies the
    //     access was cacheable and the cache was enabled.
    //   * Stage 1 (cycle N+1) is where the FSM classifies and
    //     acts (line 637-668).  `hit` is combinational on stage-1's
    //     tag/data BRAM outputs.  `s1_re`, `s1_we`, `s1_addr` carry
    //     the latched request.  s1_valid is cleared on the same
    //     edge the FSM processes it.
    //   * After S_FILL completes, the CPU's still-presented i_re=1
    //     gets re-latched in stage 0 the next time state==S_IDLE,
    //     and stage 1 then sees `hit=1` on the just-filled line.
    //     That is a re-classification of an access already counted
    //     as a miss — for one-event-per-CPU-access semantics, this
    //     re-serve must NOT count a second time.
    //
    // The four event-pulse expressions go here.  See the L2 design
    // doc and the Learn-by-Doing prompt for context and the
    // re-serve question.
    logic event_read_hit;
    logic event_read_miss;
    logic event_write_hit;
    logic event_write_miss;

    // After S_FILL completes, the CPU's still-asserted request (it's
    // been in STALL the whole time) gets re-latched by stage 0 the
    // next cycle, and stage 1 then naturally classifies it as a hit
    // on the just-installed line.  That re-serve was already counted
    // as a read_miss when the miss was first detected — so we
    // suppress the spurious second event with a one-shot flag.
    //
    // Set at the S_FILL → S_IDLE transition; cleared on the next
    // stage-1 cycle (which is by construction the re-serve).  Also
    // cleared on S_INVAL_ALL entry so a kernel-issued INVAL_ALL
    // between fill completion and re-serve doesn't leave the flag
    // armed against a later unrelated hit.
    logic fill_reserve_pending;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            fill_reserve_pending <= 1'b0;
        end else if (fill_install) begin
            fill_reserve_pending <= 1'b1;
        end else if (state == S_INVAL_ALL) begin
            fill_reserve_pending <= 1'b0;
        end else if (s1_valid && state == S_IDLE) begin
            fill_reserve_pending <= 1'b0;
        end
    end

    assign event_read_hit   = s1_valid && s1_re && hit && !fill_reserve_pending;
    assign event_read_miss  = s1_valid && s1_re && !hit;
    assign event_write_hit  = s1_valid && s1_we && hit;
    assign event_write_miss = s1_valid && s1_we && !hit;

    cache_perfctr u_perfctr (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_event_read_hit   (event_read_hit),
        .i_event_read_miss  (event_read_miss),
        .i_event_write_hit  (event_write_hit),
        .i_event_write_miss (event_write_miss),
        .i_sys_reg          (i_sys_reg),
        .o_sys_rdata        (perfctr_rdata)
    );

endmodule

// verilator lint_on UNUSEDSIGNAL
