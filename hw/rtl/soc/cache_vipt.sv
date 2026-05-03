// Penumbra Cache (VIPT) — virtually-indexed, physically-tagged L1
//
// Functional twin of the PIPT `cache` module, retimed for parallel
// TLB lookup: the cache RAM read is driven from the *virtual* address
// (no wait for translation), while the tag compare and memory bus
// still use the *physical* address.
//
// Aliasing-free precondition: cache size ≤ page size, so all index
// and offset bits live entirely within the page offset, where vaddr
// and paddr are bit-identical. In that regime VIPT is purely a
// timing change — no synonym/homonym handling, no ASID flushes, no
// page coloring. This module assumes (and asserts) that condition.
//
// Reusable for I-cache and D-cache. Direct-mapped (NUM_WAYS=1) with
// write-through, write-no-allocate. Burst-fills the entire line on
// read miss. Pass-through when disabled (CTRL.enable=0) or
// uncacheable (i_cacheable=0).
//
// Two-domain timing design:
//   * Hit path (vaddr → idx ‖ TLB → tag compare → o_rdata, o_busy)
//     stays fully combinational so a read hit completes in 1 cycle.
//     This is the whole point of VIPT — preserving 1-cycle hits.
//   * State machine (read-miss → S_FILL, write-hit → data update,
//     invalidate → clear-all-valids) reacts via *registered* shadow
//     copies of the cache inputs. The state machine therefore sees
//     "yesterday's request" and updates one cycle later than it
//     would in a fully-combinational design. Inserting a real flop
//     boundary on the slow inputs is what cuts the µROM-rooted
//     pathology that landed at valid[i].LSR — ABC cannot retime
//     across a clocked flop in synth_ecp5's default flow, so the
//     long combinational chain is physically broken in the netlist.
//
// CPU-visible cost: read-miss fills start +1 cycle later than
// previously, write-hit data updates land +1 cycle later, and
// invalidates take +1 cycle to clear valid bits. The CPU stalls
// across these via the unchanged combinational o_busy, so it
// never observes the internal delay — it just sees one extra
// cycle on the boundary of a miss/write/invalidate.

// verilator lint_off UNUSEDSIGNAL

module cache_vipt
    import penumbra_pkg::*;
#(
    parameter NUM_SETS    = 64,
    parameter LINE_WORDS  = 4,
    parameter NUM_WAYS    = 1,
    parameter ADDRESSING  = CACHE_ADDR_VIPT,
    parameter WRITE_BACK  = 1'b0,   // 0 = write-through, 1 = write-back
    parameter WRITE_ALLOC = 1'b0    // 0 = write-no-allocate, 1 = write-allocate
)
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── CPU side ───────────────────────────────────────────
    // i_vaddr drives index/offset (parallel with TLB lookup).
    // i_paddr drives the tag compare and memory bus address.
    // Caller must guarantee i_vaddr[11:0] === i_paddr[11:0]
    // (page-offset bits identical) — i.e., cache ≤ page size.
    input  logic [31:0] i_vaddr,
    input  logic [31:0] i_paddr,
    input  logic [31:0] i_wdata,
    input  logic [3:0]  i_byte_en,
    input  logic        i_we,
    input  logic        i_re,
    input  logic        i_cacheable,
    // i_fault: live MMU fault for the current access.  Used internally
    // (registered) to suppress side-effecting state changes on faulting
    // accesses — fill entry and write-hit data updates.  The cache hit
    // read path itself has no side effects and runs unconditionally,
    // letting cache RAM lookup and TLB run in parallel (the whole point
    // of VIPT).  Bus-output suppression for pass-through is enforced
    // externally by the CPU (gating o_mem_re/we with !mmu_fault).
    input  logic        i_fault,
    output logic [31:0] o_rdata,
    output logic        o_busy,

    // ── Memory / bus side (always physical) ────────────────
    output logic [31:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,
    output logic [3:0]  o_mem_byte_en,
    output logic        o_mem_we,
    output logic        o_mem_re,
    input  logic [31:0] i_mem_rdata,
    input  logic        i_mem_busy,

    // ── Sysreg interface (one per cache instance) ──────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    output logic [31:0] o_sys_rdata
);

    // ══════════════════════════════════════════════════════════
    // Derived parameters
    // ══════════════════════════════════════════════════════════
    localparam WORD_BITS = $clog2(LINE_WORDS);
    localparam SET_BITS  = $clog2(NUM_SETS);
    localparam WORD_LSB  = 2;                      // bits [1:0] = byte offset
    localparam SET_LSB   = WORD_LSB + WORD_BITS;
    localparam TAG_LSB   = SET_LSB + SET_BITS;
    localparam TAG_BITS  = 32 - TAG_LSB;

    // ══════════════════════════════════════════════════════════
    // Address field extraction — VIPT split
    // ══════════════════════════════════════════════════════════
    logic [WORD_BITS-1:0] addr_word;
    logic [SET_BITS-1:0]  addr_set;
    logic [TAG_BITS-1:0]  addr_tag;

    assign addr_word = i_vaddr[WORD_LSB +: WORD_BITS];
    assign addr_set  = i_vaddr[SET_LSB  +: SET_BITS];
    assign addr_tag  = i_paddr[TAG_LSB  +: TAG_BITS];

    // ══════════════════════════════════════════════════════════
    // Cache storage
    // ══════════════════════════════════════════════════════════
    logic                valid [0:NUM_SETS-1];
    logic [TAG_BITS-1:0] tags  [0:NUM_SETS-1];
    logic [31:0]         data  [0:NUM_SETS*LINE_WORDS-1];

    function automatic int data_idx(
        input logic [SET_BITS-1:0]  s,
        input logic [WORD_BITS-1:0] w
    );
        return int'(s) * LINE_WORDS + int'(w);
    endfunction

    // ══════════════════════════════════════════════════════════
    // Hit detection (combinational)
    // ══════════════════════════════════════════════════════════
    logic hit;
    assign hit = valid[addr_set] && (tags[addr_set] == addr_tag);

    logic cache_en;
    logic cache_active;
    assign cache_active = cache_en && i_cacheable;

    // ══════════════════════════════════════════════════════════
    // Sysreg: control and info registers
    // ══════════════════════════════════════════════════════════
    logic inval_req;

    localparam logic [31:0] INFO_VALUE = {
        10'b0,                  // [31:22] reserved
        WRITE_ALLOC[0:0],       // [21]    write-allocate
        WRITE_BACK[0:0],        // [20]    write-back
        ADDRESSING[1:0],        // [19:18] PIPT/VIPT/VIVT
        NUM_WAYS[3:0],          // [17:14]
        NUM_SETS[9:0],          // [13:4]
        LINE_WORDS[3:0]         // [3:0]
    };

    always_comb begin
        case (i_sys_reg)
            SYSREG_CACHE_INFO:  o_sys_rdata = INFO_VALUE;
            SYSREG_CACHE_CTRL:  o_sys_rdata = {31'b0, cache_en};
            default:            o_sys_rdata = 32'b0;
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
                    SYSREG_CACHE_CTRL:  cache_en  <= i_sys_wdata[0];
                    SYSREG_CACHE_INVAL: inval_req <= 1'b1;
                    default: ;
                endcase
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // State-machine input shadow flops
    // ══════════════════════════════════════════════════════════
    // Registered copies of the cache inputs that the state machine
    // consumes. The hit path (o_rdata, o_busy on hit, memory bus
    // output) uses the unregistered inputs directly to keep 1-cycle
    // hits. The state machine reacts via the *_q signals one cycle
    // later. This is the architectural change vs. cache.sv (PIPT):
    // a flop boundary the synthesizer cannot fuse across, breaking
    // the long microcode → state-machine → valid[i].LSR chain.
    //
    // Note on hit_q: registering `hit` is what lets the state
    // machine react to misses without re-evaluating the deep cache-
    // RAM-read + tag-compare cone every cycle. The subtle case is
    // the cycle right after a fill completes: hit_q is "stale"
    // because it was registered before the valid bit was set, so
    // we'd spuriously re-enter S_FILL. The state_was_fill flop
    // suppresses that one-cycle window — combinational `hit` is
    // already correct, so the CPU's read is served via the live
    // hit-path; we just don't kick off another fill.
    logic        i_re_q;
    logic        i_we_q;
    logic        i_cacheable_q;
    logic [3:0]  i_byte_en_q;
    logic [31:0] i_paddr_q;
    logic [31:0] i_wdata_q;
    logic        hit_q;
    logic        state_was_fill;
    // i_fault_q gates side-effecting state transitions (fill entry,
    // write-hit data update) so a faulting access never mutates cache
    // state.  Registered so the FSM next-state logic stays out of the
    // live mmu_fault → state.D path.
    logic        i_fault_q;
    // Note: there are no shadow flops on i_mem_busy / i_mem_rdata.  The
    // CPU-side bus arbiter (cpu_bus_arbiter) already registers external
    // bus state in its FSM, so cache.i_mem_busy / cache.i_mem_rdata are
    // already one register hop away from the live bus signals — the
    // arbiter's state flop is the boundary that breaks the
    // `bus_busy → fill_state_logic → valid[i].LSR` chain.

    // Registered address fields (derived from i_paddr_q; since
    // page-offset bits agree by precondition, vaddr/paddr give
    // identical low-order bits).
    logic [WORD_BITS-1:0] addr_word_q;
    logic [SET_BITS-1:0]  addr_set_q;
    logic [TAG_BITS-1:0]  addr_tag_q;

    assign addr_word_q = i_paddr_q[WORD_LSB +: WORD_BITS];
    assign addr_set_q  = i_paddr_q[SET_LSB  +: SET_BITS];
    assign addr_tag_q  = i_paddr_q[TAG_LSB  +: TAG_BITS];

    logic cache_active_q;
    assign cache_active_q = cache_en && i_cacheable_q;

    // ══════════════════════════════════════════════════════════
    // State machine — only S_FILL needs a state
    // ══════════════════════════════════════════════════════════
    typedef enum logic {
        S_IDLE,
        S_FILL
    } state_t;

    state_t state;

    logic [31:0]          fill_base_addr;
    logic [TAG_BITS-1:0]  fill_tag;
    logic [SET_BITS-1:0]  fill_set;
    logic [WORD_BITS-1:0] fill_count;
    logic                 fill_req;
    logic                 fill_wait;

    // ══════════════════════════════════════════════════════════
    // Memory bus output mux — always physical
    // ══════════════════════════════════════════════════════════
    always_comb begin
        o_mem_addr    = i_paddr;
        o_mem_wdata   = i_wdata;
        o_mem_byte_en = i_byte_en;
        o_mem_we      = 1'b0;
        o_mem_re      = 1'b0;

        case (state)
            S_IDLE: begin
                if (cache_active && i_re && hit) begin
                    // Read hit: no memory access
                end else if (cache_active && i_re && !hit) begin
                    // Read miss: don't access memory here — S_FILL handles it
                end else begin
                    // Pass through: uncacheable, disabled, or any write
                    o_mem_re = i_re;
                    o_mem_we = i_we;
                end
            end

            S_FILL: begin
                o_mem_addr = {fill_base_addr[31:WORD_LSB+WORD_BITS],
                              fill_count, {WORD_LSB{1'b0}}};
                // Pulse o_mem_re for the request cycle only.  The CPU
                // bus arbiter latches the request on IDLE→BUSY, after
                // which the cache no longer needs to drive o_mem_re —
                // the arbiter holds the latched address/we/re onto the
                // external bus from its own state until the transaction
                // completes.  This also keeps `fill_wait && i_mem_busy`
                // out of o_mem_re, which would otherwise close a
                // combinational loop through the arbiter's busy mux.
                o_mem_re   = fill_req;
            end
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Read data output
    // ══════════════════════════════════════════════════════════
    always_comb begin
        if (state == S_IDLE && cache_active && i_re && hit)
            o_rdata = data[data_idx(addr_set, addr_word)];
        else
            o_rdata = i_mem_rdata;
    end

    // ══════════════════════════════════════════════════════════
    // Busy signal
    // ══════════════════════════════════════════════════════════
    always_comb begin
        case (state)
            S_IDLE: begin
                if (cache_active && i_re && hit)
                    o_busy = 1'b0;
                else if (cache_active && i_re && !hit)
                    o_busy = 1'b1;
                else
                    o_busy = i_mem_busy;
            end
            S_FILL:
                o_busy = 1'b1;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Shadow-flop update — capture inputs at the cache boundary
    // ══════════════════════════════════════════════════════════
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            i_re_q         <= 1'b0;
            i_we_q         <= 1'b0;
            i_cacheable_q  <= 1'b0;
            i_byte_en_q    <= 4'b0;
            i_paddr_q      <= 32'b0;
            i_wdata_q      <= 32'b0;
            hit_q          <= 1'b0;
            state_was_fill <= 1'b0;
            i_fault_q      <= 1'b0;
        end else begin
            i_re_q         <= i_re;
            i_we_q         <= i_we;
            i_cacheable_q  <= i_cacheable;
            i_byte_en_q    <= i_byte_en;
            i_paddr_q      <= i_paddr;
            i_wdata_q      <= i_wdata;
            hit_q          <= hit;
            state_was_fill <= (state == S_FILL);
            i_fault_q      <= i_fault;
        end
    end

    // ══════════════════════════════════════════════════════════
    // State machine and storage updates
    //
    // CPU-side inputs (i_re_q, i_we_q, hit_q, i_paddr_q, i_wdata_q,
    // i_byte_en_q, i_cacheable_q, i_fault_q) come from the registered
    // shadow flops above — required to break the µROM → state.D path.
    // Bus-side inputs (i_mem_busy, i_mem_rdata) are consumed live;
    // the cpu_bus_arbiter's state register is the flop boundary.
    // ══════════════════════════════════════════════════════════
    integer i;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state      <= S_IDLE;
            fill_count <= '0;
            fill_req   <= 1'b0;
            fill_wait  <= 1'b0;
            fill_base_addr <= 32'b0;
            fill_tag   <= '0;
            fill_set   <= '0;
            for (i = 0; i < NUM_SETS; i++)
                valid[i] <= 1'b0;
        end else begin

            if (inval_req) begin
                for (i = 0; i < NUM_SETS; i++)
                    valid[i] <= 1'b0;
            end

            case (state)
                S_IDLE: begin
                    // Write hit: update the cache line one cycle
                    // after the CPU presented the write. The bus
                    // output mux already sent the write through
                    // to memory in the original cycle.
                    // Gated by !i_fault_q so a faulting write never
                    // mutates the cache data array.
                    if (cache_active_q && i_we_q && hit_q && !i_fault_q) begin
                        if (i_byte_en_q[0])
                            data[data_idx(addr_set_q, addr_word_q)][ 7: 0] <= i_wdata_q[ 7: 0];
                        if (i_byte_en_q[1])
                            data[data_idx(addr_set_q, addr_word_q)][15: 8] <= i_wdata_q[15: 8];
                        if (i_byte_en_q[2])
                            data[data_idx(addr_set_q, addr_word_q)][23:16] <= i_wdata_q[23:16];
                        if (i_byte_en_q[3])
                            data[data_idx(addr_set_q, addr_word_q)][31:24] <= i_wdata_q[31:24];
                    end

                    // Read miss → start fill. !state_was_fill
                    // suppresses the spurious miss-detect on the
                    // cycle right after a fill completes (hit_q
                    // is stale by one cycle while combinational
                    // `hit` already reflects the just-set valid
                    // bit, so the CPU's read is being served via
                    // the live hit path and we must not re-fill).
                    // !i_fault_q gates fill entry so a faulting miss
                    // never initiates a bus access with bogus paddr.
                    if (cache_active_q && i_re_q && !hit_q && !state_was_fill && !i_fault_q) begin
                        fill_base_addr <= i_paddr_q;
                        fill_tag       <= addr_tag_q;
                        fill_set       <= addr_set_q;
                        fill_count     <= '0;
                        fill_req       <= 1'b1;
                        fill_wait      <= 1'b0;
                        state          <= S_FILL;
                    end
                end

                S_FILL: begin
                    if (fill_req && !fill_wait) begin
                        fill_wait <= 1'b1;
                        fill_req  <= 1'b0;
                    end else if (fill_wait && !i_mem_busy) begin
                        data[data_idx(fill_set, fill_count)] <= i_mem_rdata;

                        if (fill_count == WORD_BITS'(LINE_WORDS - 1)) begin
                            tags[fill_set]  <= fill_tag;
                            valid[fill_set] <= 1'b1;
                            fill_wait       <= 1'b0;
                            state           <= S_IDLE;
                        end else begin
                            fill_count <= fill_count + WORD_BITS'(1);
                            fill_req   <= 1'b1;
                            fill_wait  <= 1'b0;
                        end
                    end
                end
            endcase
        end
    end

    // ══════════════════════════════════════════════════════════
    // VIPT precondition: cache index/offset must be inside the
    // page offset, so vaddr and paddr agree on those bits. If they
    // don't, the index lookup races a tag from a different page —
    // silently corrupt. Stripped by Yosys (simulation-only).
    // ══════════════════════════════════════════════════════════
    assert property (@(posedge i_clk) disable iff (i_rst)
        (i_re || i_we) |-> (i_vaddr[TAG_LSB-1:0] == i_paddr[TAG_LSB-1:0]))
        else $error("cache_vipt: vaddr/paddr disagree on index bits — cache > page size?");

    // Combinational hit-path contract — same as PIPT cache.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (state == S_IDLE && cache_active && i_re && hit) |-> !o_busy)
        else $error("cache_vipt: read hit but o_busy=1 — register stage on hit path?");

endmodule

// verilator lint_on UNUSEDSIGNAL
