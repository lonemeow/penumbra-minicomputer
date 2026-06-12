// cache_bram_vipt — Penumbra/2 BRAM-backed L1 cache (VIPT, set-associative)
//
// The gen2 L1, designed around BRAM/EBR-resident storage with registered
// output (the BRAM-backed cache decision in
// doc/internals/penumbra2/design-decisions.md): a lookup is a two-cycle
// launch/resolve exchange rather than gen1's combinational hit cone. The
// pipeline absorbs the latency — IF1 launches and IF2 resolves on the
// I-side; MEM's single-STALL access does both on the D-side. One module
// serves both sides (an I-side instance ties the write inputs off).
//
// Front-side contract (the registered-read convention every RAM-shaped
// gen2 module follows — unified_mem, tlb_bram, mmu_bram):
//
//   Launch (cycle T): i_en samples i_vaddr at the clock edge; the indexed
//   tag/data/valid of every way are read and registered, available the
//   next cycle. i_en low holds those outputs, keeping a resolved lookup
//   paired with a stalled consumer. The virtual address is used only for
//   indexing (VIPT): index and offset bits must sit inside the page
//   offset, where vaddr and paddr agree — cache size per way ≤ page size
//   (asserted below).
//
//   Resolve (cycle T+1): the MMU verdict for the launched access arrives
//   (i_paddr / i_cacheable / i_fault — registered and held by the MMU
//   under the same convention) and the consumer asserts its request,
//   i_re or i_we (with i_wdata / i_byte_en). The registered tags compare
//   against the physical tag and the access completes or stalls:
//     - cacheable read hit:   o_busy=0 with o_rdata valid, this cycle;
//     - cacheable read miss:  o_busy=1 from this cycle; a line request
//       goes out the back side, and o_busy drops with o_rdata valid on
//       the same cycle (drop-equals-valid) once the fill completes;
//     - any write:            write-through — o_busy follows the
//       downstream busy handshake; a tag hit also byte-updates the local
//       copy (write-no-allocate: a miss changes nothing locally);
//     - uncacheable/disabled: pass-through single beat; o_busy follows
//       downstream and o_rdata presents the downstream read data;
//     - i_fault=1:            the slot is inert — o_busy=0, nothing
//       reaches the back side, no cache state mutates (the consumer
//       takes the fault instead of the data).
//
//   Consumer obligations: hold the address/request inputs stable from
//   launch to the completion cycle; never launch a new lookup (i_en)
//   while o_busy=1; deassert the request the cycle after completion. A
//   WRSYS that rewrites cache state under an in-flight lookup (INVAL_ALL,
//   CTRL) leaves that lookup its pre-write verdict — benign, because
//   WRSYS is context-synchronizing and the post-commit re-fetch discards
//   the in-flight word.
//
// Back-side contract (doc/internals/penumbra2/memory-interface.md):
//   - A cacheable read miss raises one line request: o_mem_re=1 with
//     o_mem_cacheable=1 and a line-aligned o_mem_addr, held level until
//     the fill sequencer's i_fill_done. The sequencer streams the line
//     through the fill port (i_fill_we / i_fill_word / i_fill_wdata),
//     writing the data array directly; the cache captures the requested
//     word in flight and serves it the cycle after i_fill_done. A fill
//     runs to completion once requested — transactions are atomic.
//   - Everything else is a single beat forwarded with its full shape
//     ({addr, wdata, byte_en, re, we, cacheable}); completion is the
//     downstream busy-drop, the same handshake as the gen1 memory port.
//
// Storage:
//   - Tags and data are per-way BRAMs (separate generate-scoped
//     memories: a merged multi-way array tempts yosys into one very wide
//     memory built from bit-sliced DP16KDs at ~4x the EBR cost — the
//     per-way split is the same lesson l2_cache.sv records).
//   - Valid bits are per-way flop vectors, not BRAM: INVAL_ALL (the
//     I-side runs it on every exec-page load) and reset must clear every
//     line in one cycle from a single net, where a RAM port clears one
//     address per cycle. They are sampled at launch and registered to
//     stay aligned with the BRAM outputs (the tlb_bram valid template).
//     Tags and data are never cleared; valid=0 masks stale contents.
//   - Replacement state is tree-PLRU in flops, 3 bits per set at 4 ways
//     (the l2_cache.sv scheme); a 2-way build uses bit 0 as the LRU
//     pointer and synthesis prunes the rest.
//
// Sysreg layout is the unified cache map (INFO geometry / CTRL.enable /
// INVAL_ALL / perfctr regs via cache_perfctr) — identical to gen1's L1
// and the L2, so the same kernel cache bring-up drives both generations.
// FLUSH ops are architectural no-ops under write-through (nothing dirty
// to write back); INVAL_LINE stays reserved.

// verilator lint_off UNUSEDSIGNAL

module cache_bram_vipt
    import penumbra_pkg::*;
#(
    parameter int CACHE_BYTES = 4096,
    parameter int LINE_BYTES  = 16,
    parameter int NUM_WAYS    = 4
)
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Front side: lookup launch (cycle T) ──────────────────────
    input  logic [31:0] i_vaddr,
    input  logic        i_en,           // launch clock-enable (holds outputs when low)

    // ── Front side: resolve + request (cycle T+1 onward) ─────────
    input  logic [31:0] i_paddr,        // MMU verdict, registered + held
    input  logic        i_cacheable,    // PTE.C from the same verdict
    input  logic        i_fault,        // translation fault: the slot is inert
    input  logic        i_re,
    input  logic        i_we,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Back side: request port (toward the I/D arbiter) ─────────
    output logic [31:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,
    output logic [3:0]  o_mem_byte_en,
    output logic        o_mem_re,
    output logic        o_mem_we,
    output logic        o_mem_cacheable,
    input  logic [31:0] i_mem_rdata,
    input  logic        i_mem_busy,

    // ── Back side: fill port (driven by the fill sequencer) ──────
    input  logic                              i_fill_we,
    input  logic [$clog2(LINE_BYTES/4)-1:0]   i_fill_word,
    input  logic [31:0]                       i_fill_wdata,
    input  logic                              i_fill_done,

    // ── Sysreg interface (one per cache instance) ────────────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    output logic [31:0] o_sys_rdata
);

    // ══════════════════════════════════════════════════════════
    // Geometry
    // ══════════════════════════════════════════════════════════
    localparam int LINE_WORDS  = LINE_BYTES / 4;
    localparam int WORD_BITS   = $clog2(LINE_WORDS);
    localparam int WORD_LSB    = 2;                    // bits [1:0] = byte offset
    localparam int OFFSET_BITS = $clog2(LINE_BYTES);
    localparam int NUM_SETS    = CACHE_BYTES / (LINE_BYTES * NUM_WAYS);
    localparam int SET_BITS    = $clog2(NUM_SETS);
    localparam int TAG_LSB     = OFFSET_BITS + SET_BITS;
    localparam int TAG_BITS    = 32 - TAG_LSB;
    localparam int WAY_BITS    = $clog2(NUM_WAYS);
    localparam int PAGE_BYTES  = 4096;                 // architectural page size

    // Parameter validity — elaboration-time generate checks, hard errors
    // in every flow (simulation and synthesis), so an unsupported
    // geometry can never build silently.
    if (NUM_WAYS != 2 && NUM_WAYS != 4) begin : g_check_ways
        $error("cache_bram_vipt: tree-PLRU is coded for 2 or 4 ways");
    end
    if (CACHE_BYTES / NUM_WAYS > PAGE_BYTES) begin : g_check_vipt
        $error("cache_bram_vipt: way size exceeds the page size (VIPT precondition)");
    end
    if (LINE_WORDS < 2 || (1 << WORD_BITS) != LINE_WORDS
        || (1 << SET_BITS) != NUM_SETS) begin : g_check_pow2
        $error("cache_bram_vipt: geometry must be power-of-two with multi-word lines");
    end

    function automatic logic [SET_BITS+WORD_BITS-1:0] data_idx(
        input logic [SET_BITS-1:0]  s,
        input logic [WORD_BITS-1:0] w
    );
        return {s, w};
    endfunction

    // ══════════════════════════════════════════════════════════
    // Replacement policy — tree-PLRU
    //
    //         bit 0
    //         /   \
    //       0      1
    //      / \    / \
    //   way0 1  way2 3
    //   bit 1   bit 2
    //
    // bit 0: 0 = LRU is in (way0, way1), 1 = LRU is in (way2, way3)
    // bit 1: (pair 0,1) 0 = LRU is way 0, 1 = way 1
    // bit 2: (pair 2,3) 0 = LRU is way 2, 1 = way 3
    //
    // The 2-way variant keeps only bit 0 as the LRU way index. The
    // function widths stay fixed at the 4-way shape so both builds
    // elaborate; unused bits are constant and pruned.
    // ══════════════════════════════════════════════════════════

    // The way the PLRU tree currently points at (least recently used).
    function automatic logic [1:0] plru_lru(input logic [2:0] p);
        if (NUM_WAYS == 4) begin
            if (p[0] == 1'b0) return p[1] ? 2'd1 : 2'd0;
            else              return p[2] ? 2'd3 : 2'd2;
        end else begin
            return {1'b0, p[0]};
        end
    endfunction

    // Mark `way` most-recently-used: flip the bits along its path away
    // from it, so the LRU pointer migrates to the untouched subtree.
    function automatic logic [2:0] plru_update(input logic [2:0] p,
                                               input logic [1:0] way);
        logic [2:0] r;
        r = p;
        if (NUM_WAYS == 4) begin
            case (way)
                2'd0: begin r[0] = 1'b1; r[1] = 1'b1; end
                2'd1: begin r[0] = 1'b1; r[1] = 1'b0; end
                2'd2: begin r[0] = 1'b0; r[2] = 1'b1; end
                2'd3: begin r[0] = 1'b0; r[2] = 1'b0; end
                default: ;
            endcase
        end else begin
            r[0] = ~way[0];
        end
        return r;
    endfunction

    // ── Victim selection — the way a read miss fills ──────────────
    // Chooses among the set's ways given their valid bits (v, sampled at
    // launch) and the set's PLRU state (p); plru_lru(p) decodes the
    // tree's current least-recently-used way.
    function automatic logic [1:0] victim_way(
        input logic [NUM_WAYS-1:0] v,
        input logic [2:0]          p
    );
        // Invalid-first, in index order: never evict a live line while
        // an empty way exists (and post-INVAL_ALL fills pack ways
        // 0,1,2,... predictably). Only a full set consults the tree.
        for (int i = 0; i < NUM_WAYS; i++) begin
            if (!v[i])
                return 2'(i);
        end
        return plru_lru(p);
    endfunction

    // ══════════════════════════════════════════════════════════
    // Sysreg: control and info registers
    // ══════════════════════════════════════════════════════════
    logic cache_en;
    logic inval_req;

    localparam logic [31:0] INFO_VALUE = {
        2'b0,                   // [31:30] reserved
        1'b0,                   // [29]    write-allocate
        1'b0,                   // [28]    write-back
        CACHE_ADDR_VIPT,        // [27:26] addressing
        5'(NUM_WAYS),           // [25:21] ways
        15'(NUM_SETS),          // [20:6]  sets
        6'(LINE_WORDS)          // [5:0]   line words
    };

    logic [31:0] perfctr_rdata;

    always_comb begin
        case (i_sys_reg)
            SYSREG_CACHE_INFO: o_sys_rdata = INFO_VALUE;
            SYSREG_CACHE_CTRL: o_sys_rdata = {31'b0, cache_en};
            default:           o_sys_rdata = perfctr_rdata;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            cache_en  <= 1'b0;
            inval_req <= 1'b0;
        end else begin
            inval_req <= 1'b0;
            if (i_sys_we) begin
                case (i_sys_reg)
                    SYSREG_CACHE_CTRL:      cache_en  <= i_sys_wdata[0];
                    SYSREG_CACHE_INVAL_ALL: inval_req <= 1'b1;
                    default: ;
                endcase
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Storage — valid flops + replacement flops
    // ══════════════════════════════════════════════════════════
    logic [NUM_SETS-1:0] valid_vec [NUM_WAYS];
    logic [2:0]          plru      [NUM_SETS];

    // Per-way BRAM read outputs (registered at launch, held by i_en).
    logic [TAG_BITS-1:0] tag_out  [NUM_WAYS];
    logic [31:0]         data_out [NUM_WAYS];

    // ── Launch-cycle address fields ───────────────────────────────
    logic [SET_BITS-1:0]  launch_set;
    logic [WORD_BITS-1:0] launch_word;
    assign launch_set  = i_vaddr[OFFSET_BITS +: SET_BITS];
    assign launch_word = i_vaddr[WORD_LSB    +: WORD_BITS];

    // ── Launch capture (cycle T): valid sample + resolve context ──
    // Sampled with the BRAM read and registered so the whole verdict —
    // tags, data, valids, and the address that produced them — stays
    // coherent and holds together under i_en=0.
    logic [SET_BITS-1:0]  set_q;
    logic [WORD_BITS-1:0] word_q;
    logic [TAG_LSB-1:0]   vlow_q;        // launch vaddr low bits (VIPT check)
    logic [NUM_WAYS-1:0]  valid_q;
    logic                 lookup_q;      // a launched lookup resolves this cycle

    always_ff @(posedge i_clk) begin
        if (i_en) begin
            set_q  <= launch_set;
            word_q <= launch_word;
            vlow_q <= i_vaddr[TAG_LSB-1:0];
            for (int w = 0; w < NUM_WAYS; w++)
                valid_q[w] <= valid_vec[w][launch_set];
        end
        if (i_rst) lookup_q <= 1'b0;
        else       lookup_q <= i_en;
    end

    // ══════════════════════════════════════════════════════════
    // Resolve (cycle T+1): tag compare + request classification
    // ══════════════════════════════════════════════════════════
    logic [TAG_BITS-1:0] paddr_tag;
    assign paddr_tag = i_paddr[TAG_LSB +: TAG_BITS];

    logic [NUM_WAYS-1:0]  way_hit;
    logic                 hit;
    logic [WAY_BITS-1:0]  hit_way;

    always_comb begin
        for (int w = 0; w < NUM_WAYS; w++)
            way_hit[w] = valid_q[w] && (tag_out[w] == paddr_tag);
    end
    assign hit = |way_hit;

    always_comb begin
        hit_way = '0;
        for (int w = 0; w < NUM_WAYS; w++)
            if (way_hit[w]) hit_way = WAY_BITS'(w);
    end

    // A faulting slot is inert: it raises no request anywhere.
    logic rd_req, wr_req, cache_active, cached_rd, cached_wr, pt_read;
    assign rd_req       = i_re && !i_fault;
    assign wr_req       = i_we && !i_fault;
    assign cache_active = cache_en && i_cacheable;
    assign cached_rd    = cache_active && rd_req;
    assign cached_wr    = cache_active && wr_req;
    assign pt_read      = rd_req && !cache_active;

    // ── Resolved-verdict hold for the write path ──────────────────
    // A write outlives its resolve cycle (it waits on the downstream
    // busy handshake), so the hit verdict is registered at resolve and
    // the effective verdict muxes live-vs-held — the same value either
    // way, on whichever cycle the write completes.
    logic                hit_q;
    logic [WAY_BITS-1:0] hit_way_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            hit_q     <= 1'b0;
            hit_way_q <= '0;
        end else if (lookup_q) begin
            hit_q     <= hit;
            hit_way_q <= hit_way;
        end
    end

    logic                eff_hit;
    logic [WAY_BITS-1:0] eff_way;
    assign eff_hit = lookup_q ? hit     : hit_q;
    assign eff_way = lookup_q ? hit_way : hit_way_q;

    // ══════════════════════════════════════════════════════════
    // Access FSM — IDLE resolves, FILL streams, SERVE delivers
    // ══════════════════════════════════════════════════════════
    typedef enum logic [1:0] {
        S_IDLE,                 // resolve hits/writes/pass-through; detect misses
        S_FILL,                 // line request out; sequencer streams the line in
        S_SERVE                 // deliver the captured word (busy-drop cycle)
    } state_t;

    state_t state;

    logic [31:OFFSET_BITS] fill_base;       // line-aligned physical address
    logic [TAG_BITS-1:0]   fill_tag;
    logic [SET_BITS-1:0]   fill_set;
    logic [WORD_BITS-1:0]  fill_word_req;   // the word the stalled access wants
    logic [WAY_BITS-1:0]   fill_way;
    logic [31:0]           fill_rdata_q;    // requested word, captured in flight
    logic                  got_word;        // capture happened (assertion fodder)

    logic miss_resolve, rd_hit_resolve, wr_done;
    assign miss_resolve   = lookup_q && (state == S_IDLE) && cached_rd && !hit;
    assign rd_hit_resolve = lookup_q && (state == S_IDLE) && cached_rd && hit;
    // The write's completion cycle: the downstream busy-drop (which can
    // be the resolve cycle itself for an immediately-ready device).
    assign wr_done        = (state == S_IDLE) && cached_wr && !i_mem_busy;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state    <= S_IDLE;
            got_word <= 1'b0;
        end else begin
            unique case (state)
                S_IDLE: begin
                    if (miss_resolve) begin
                        state         <= S_FILL;
                        fill_base     <= i_paddr[31:OFFSET_BITS];
                        fill_tag      <= paddr_tag;
                        fill_set      <= set_q;
                        fill_word_req <= word_q;
                        fill_way      <= WAY_BITS'(victim_way(valid_q, plru[set_q]));
                        got_word      <= 1'b0;
                    end
                end
                S_FILL: begin
                    if (i_fill_we && (i_fill_word == fill_word_req)) begin
                        fill_rdata_q <= i_fill_wdata;
                        got_word     <= 1'b1;
                    end
                    if (i_fill_done)
                        state <= S_SERVE;
                end
                S_SERVE: state <= S_IDLE;
                default: state <= S_IDLE;
            endcase
        end
    end

    // ── Valid bits: install at fill-done, flash-clear on INVAL/reset ──
    // The clear is last so it wins over a same-cycle install: software
    // asked for an empty cache, and the in-flight line's consumer is
    // wrong-path by the WRSYS resync argument anyway.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            for (int w = 0; w < NUM_WAYS; w++)
                valid_vec[w] <= '0;
        end else begin
            if (state == S_FILL && i_fill_done)
                valid_vec[fill_way][fill_set] <= 1'b1;
            if (inval_req)
                for (int w = 0; w < NUM_WAYS; w++)
                    valid_vec[w] <= '0;
        end
    end

    // ── PLRU: touch on read hit, completed write hit, fill install ──
    // The three sites are mutually exclusive (one access in flight, and
    // fills only end in S_FILL).
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            for (int s = 0; s < NUM_SETS; s++)
                plru[s] <= 3'b000;
        end else if (state == S_FILL && i_fill_done) begin
            plru[fill_set] <= plru_update(plru[fill_set], 2'(fill_way));
        end else if (rd_hit_resolve) begin
            plru[set_q] <= plru_update(plru[set_q], 2'(hit_way));
        end else if (wr_done && eff_hit) begin
            plru[set_q] <= plru_update(plru[set_q], 2'(eff_way));
        end
    end

    // ══════════════════════════════════════════════════════════
    // Per-way BRAM storage — tags + data
    // ══════════════════════════════════════════════════════════
    generate
        for (genvar gw = 0; gw < NUM_WAYS; gw++) begin : g_way
            (* ram_style = "block" *)
            logic [TAG_BITS-1:0] tag_mem  [NUM_SETS];
            (* ram_style = "block" *)
            logic [31:0]         data_mem [NUM_SETS*LINE_WORDS];

            // Launch read, clock-enabled by i_en (hold under stall).
            always_ff @(posedge i_clk) begin
                if (i_en) begin
                    tag_out[gw]  <= tag_mem[launch_set];
                    data_out[gw] <= data_mem[data_idx(launch_set, launch_word)];
                end
            end

            // Tag install — once per fill, at fill-done.
            always_ff @(posedge i_clk) begin
                if (state == S_FILL && i_fill_done && (fill_way == WAY_BITS'(gw)))
                    tag_mem[fill_set] <= fill_tag;
            end

            // Data writes: fill-stream beats, or the write-hit local
            // update on the write's completion cycle. Mutually exclusive
            // by state, so they share the one write port.
            logic dwr_fill, dwr_hit;
            assign dwr_fill = (state == S_FILL) && i_fill_we
                              && (fill_way == WAY_BITS'(gw));
            assign dwr_hit  = wr_done && eff_hit && (eff_way == WAY_BITS'(gw));

            always_ff @(posedge i_clk) begin
                if (dwr_fill) begin
                    data_mem[data_idx(fill_set, i_fill_word)] <= i_fill_wdata;
                end else if (dwr_hit) begin
                    if (i_byte_en[0]) data_mem[data_idx(set_q, word_q)][ 7: 0] <= i_wdata[ 7: 0];
                    if (i_byte_en[1]) data_mem[data_idx(set_q, word_q)][15: 8] <= i_wdata[15: 8];
                    if (i_byte_en[2]) data_mem[data_idx(set_q, word_q)][23:16] <= i_wdata[23:16];
                    if (i_byte_en[3]) data_mem[data_idx(set_q, word_q)][31:24] <= i_wdata[31:24];
                end
            end
        end
    endgenerate

    // ══════════════════════════════════════════════════════════
    // Front-side outputs
    // ══════════════════════════════════════════════════════════
    always_comb begin
        unique case (state)
            S_IDLE: begin
                if (cached_rd)               o_busy = ~hit;
                else if (rd_req || wr_req)   o_busy = i_mem_busy;
                else                         o_busy = 1'b0;
            end
            S_FILL:  o_busy = 1'b1;
            S_SERVE: o_busy = 1'b0;
            default: o_busy = 1'b0;
        endcase
    end

    // Hit data routes through the held verdict (eff_way) so a stalled
    // consumer keeps reading the same word; pass-through reads present
    // the downstream data (held by the device per the sync-bus contract).
    always_comb begin
        if (state == S_SERVE) o_rdata = fill_rdata_q;
        else if (pt_read)     o_rdata = i_mem_rdata;
        else                  o_rdata = data_out[eff_way];
    end

    // ══════════════════════════════════════════════════════════
    // Back-side request port
    // ══════════════════════════════════════════════════════════
    always_comb begin
        o_mem_addr      = i_paddr;
        o_mem_wdata     = i_wdata;
        o_mem_byte_en   = i_byte_en;
        o_mem_re        = 1'b0;
        o_mem_we        = 1'b0;
        o_mem_cacheable = i_cacheable;
        unique case (state)
            S_IDLE: begin
                // A cacheable read never raises a single beat — it is
                // served locally or by the fill path. Everything else
                // that requests goes downstream: write-through stores,
                // uncacheable and disabled accesses. A forwarded read is
                // single-beat by construction, so it must not present
                // cacheable=1 — that is the arbiter's line-mode select
                // (`cacheable && re` engages the fill sequencer). A
                // disabled cache therefore forwards its reads as
                // uncacheable at this layer; writes keep the PTE bit.
                if (!cached_rd) begin
                    o_mem_re = rd_req;
                    o_mem_we = wr_req;
                    if (rd_req)
                        o_mem_cacheable = 1'b0;
                end
            end
            S_FILL: begin
                o_mem_addr      = {fill_base, {OFFSET_BITS{1'b0}}};
                o_mem_re        = 1'b1;
                o_mem_cacheable = 1'b1;
            end
            S_SERVE: ;
            default: ;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Performance counters — one pulse per access, at its resolve
    // ══════════════════════════════════════════════════════════
    logic event_write_hit, event_write_miss;
    assign event_write_hit  = lookup_q && (state == S_IDLE) && cached_wr && hit;
    assign event_write_miss = lookup_q && (state == S_IDLE) && cached_wr && !hit;

    cache_perfctr u_perfctr (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_event_read_hit   (rd_hit_resolve),
        .i_event_read_miss  (miss_resolve),
        .i_event_write_hit  (event_write_hit),
        .i_event_write_miss (event_write_miss),
        .i_sys_reg          (i_sys_reg),
        .o_sys_rdata        (perfctr_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // VIPT precondition, checked per access: the launch vaddr and the
    // resolve paddr agree on every index/offset bit. A mismatch means
    // the index lookup raced a tag from a different page.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (lookup_q && (i_re || i_we)) |-> (vlow_q == i_paddr[TAG_LSB-1:0]))
        else $error("cache_bram_vipt: vaddr/paddr disagree on index bits — way > page size?");

    // Consumer contract: no new launch while an access is in flight.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_busy |-> !i_en)
        else $error("cache_bram_vipt: lookup launched while busy");

    // The fill port belongs to the in-flight fill alone.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_fill_we || i_fill_done) |-> (state == S_FILL))
        else $error("cache_bram_vipt: fill-port activity outside a fill");

    // By fill-done the sequencer must have delivered the requested word
    // (possibly on the done cycle itself).
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_FILL && i_fill_done) |->
            (got_word || (i_fill_we && i_fill_word == fill_word_req)))
        else $error("cache_bram_vipt: fill done without the requested word");

    // One access at a time: reads and writes never co-assert, and a
    // store can never overlap a fill (the missing read owns the core).
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_re && i_we))
        else $error("cache_bram_vipt: simultaneous read and write request");
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state != S_IDLE) |-> !i_we)
        else $error("cache_bram_vipt: write request during a fill");

    // A miss is only ever observed on its resolve cycle — one cycle
    // later it lives in S_FILL. A held miss in S_IDLE is a lost fill.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_IDLE && cached_rd && !hit) |-> lookup_q)
        else $error("cache_bram_vipt: unresolved miss held in S_IDLE");

    // The back side raises one request shape at a time.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_mem_re && o_mem_we))
        else $error("cache_bram_vipt: simultaneous back-side read and write");

    // Reset establishes the all-invalid invariant (checks the reset
    // behaviour itself, so not disabled under i_rst).
    logic any_valid;
    always_comb begin
        any_valid = 1'b0;
        for (int w = 0; w < NUM_WAYS; w++)
            any_valid |= |valid_vec[w];
    end
    assert property (@(posedge i_clk)
        i_rst |=> !any_valid)
        else $error("cache_bram_vipt: reset did not clear the valid vectors");

endmodule

// verilator lint_on UNUSEDSIGNAL
