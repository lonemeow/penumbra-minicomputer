// Penumbra CPU bus arbiter — gives icache and dcache private bus ports
//
// The CPU has two memory clients (split I/D L1) but only one external
// bus.  The naive solution is a combinational mux of addr/we/re plus
// an OR of `o_busy` aggregation, but that creates a structural cross-
// dependency where dcache's bus traffic fans into icache's `i_mem_busy`
// (and vice versa) through device-decode and busy-aggregation logic.
// Synthesis closes for the worst-case path through *both* caches even
// when only one is active.  That cross-coupling pinned the CPU's
// critical path between the two caches.
//
// This arbiter eliminates the cross-coupling structurally.  Each cache
// gets a private (i_re, i_we, addr, wdata) → (busy, rdata) port.  The
// arbiter holds a small FSM (IDLE/BUSY/DONE) and *registers* both the
// outgoing request (addr/wdata/we) and the incoming response (rdata)
// at the FSM transitions.  The path from one cache's request to the
// other cache's busy/rdata now goes through the arbiter's state flop —
// the synthesizer cannot retime across that boundary, so each cache
// sees only its own busy/rdata net.
//
// Arbitration policy: dcache priority.  Fetch (S_FETCH) and data
// access (S_EXEC) are mutually exclusive at the CPU level, so the
// priority is a tiebreaker — only relevant during a corner-case
// overlap (e.g. a fetch refill straddling a data access).
//
// Why three states (IDLE/BUSY/DONE) and not two?
//   The cache (and CPU pass-through path) holds i_re asserted until
//   it sees `i_mem_busy` drop — the CPU's STALL exits combinationally
//   on busy↓.  If the arbiter went BUSY→IDLE directly, in the same
//   cycle the request signal would still be high (the cache's state
//   machine hasn't reacted yet) and the arbiter would re-latch and
//   run the transaction again.  The DONE state is a 1-cycle cooldown
//   that lets the cache observe busy↓ and deassert before the
//   arbiter accepts new traffic.
//
// Costs:
//   - +1 cycle per uncached MMIO access (registered busy/rdata).
//   - +1 cycle per fill-word completion (≈+3% on a 4-word line fill,
//     dwarfed by SDRAM+CDC latency).
//   - Zero cycles on cached hits — they never reach the arbiter.

// verilator lint_off UNUSEDSIGNAL

module cpu_bus_arbiter
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Port D: dcache (priority, full read/write) ─────────
    input  logic [31:0] i_d_addr,
    input  logic [31:0] i_d_wdata,
    input  logic [3:0]  i_d_byte_en,
    input  logic        i_d_we,
    input  logic        i_d_re,
    output logic [31:0] o_d_rdata,
    output logic        o_d_busy,

    // ── Port I: icache (read-only fetch) ───────────────────
    input  logic [31:0] i_i_addr,
    input  logic        i_i_re,
    output logic [31:0] o_i_rdata,
    output logic        o_i_busy,

    // ── External bus (single, shared with rest of system) ──
    output logic [31:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,
    output logic [3:0]  o_mem_byte_en,
    output logic        o_mem_we,
    output logic        o_mem_re,
    input  logic [31:0] i_mem_rdata,
    input  logic        i_mem_busy
);

    // ══════════════════════════════════════════════════════════
    // FSM — IDLE → BUSY → DONE → IDLE
    // ══════════════════════════════════════════════════════════
    typedef enum logic [1:0] {
        S_IDLE,
        S_BUSY,
        S_DONE
    } state_t;

    state_t state, state_n;

    // Latched request — driven onto the external bus during S_BUSY.
    logic [31:0] req_addr;
    logic [31:0] req_wdata;
    logic [3:0]  req_byte_en;
    logic        req_we;
    logic        req_re;
    logic        owner;        // 0 = D, 1 = I (last/current owner)

    // Latched response — captured on BUSY→DONE, presented in S_DONE.
    logic [31:0] resp_rdata;

    // Pending request flags (combinational; held by cache while STALLed).
    logic pending_d, pending_i;
    assign pending_d = i_d_re | i_d_we;
    assign pending_i = i_i_re;

    // Pick policy: dcache wins ties.
    logic pick_d, pick_i;
    assign pick_d = pending_d;
    assign pick_i = pending_i & ~pending_d;

    // ── Sequential: state, latched request, latched response ─────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state       <= S_IDLE;
            req_addr    <= 32'b0;
            req_wdata   <= 32'b0;
            req_byte_en <= 4'b0;
            req_we      <= 1'b0;
            req_re      <= 1'b0;
            owner       <= 1'b0;
            resp_rdata  <= 32'b0;
        end else begin
            state <= state_n;

            // Latch outgoing request on IDLE→BUSY transition.
            if (state == S_IDLE && (pick_d | pick_i)) begin
                if (pick_d) begin
                    req_addr    <= i_d_addr;
                    req_wdata   <= i_d_wdata;
                    req_byte_en <= i_d_byte_en;
                    req_we      <= i_d_we;
                    req_re      <= i_d_re;
                    owner       <= 1'b0;
                end else begin
                    req_addr    <= i_i_addr;
                    req_wdata   <= 32'b0;
                    req_byte_en <= 4'b0;
                    req_we      <= 1'b0;
                    req_re      <= i_i_re;
                    owner       <= 1'b1;
                end
            end

            // Latch incoming response on BUSY→DONE transition.
            if (state == S_BUSY && !i_mem_busy)
                resp_rdata <= i_mem_rdata;
        end
    end

    // ── Next-state logic ────────────────────────────────────────
    always_comb begin
        state_n = state;
        case (state)
            S_IDLE:
                if (pending_d | pending_i)
                    state_n = S_BUSY;
            S_BUSY:
                if (!i_mem_busy)
                    state_n = S_DONE;
            S_DONE:
                state_n = S_IDLE;
            default:
                state_n = S_IDLE;
        endcase
    end

    // ── External bus drive — only in S_BUSY ────────────────────
    assign o_mem_addr    = req_addr;
    assign o_mem_wdata   = req_wdata;
    assign o_mem_byte_en = req_byte_en;
    assign o_mem_we      = (state == S_BUSY) && req_we;
    assign o_mem_re      = (state == S_BUSY) && req_re;

    // ── Per-cache busy/rdata ────────────────────────────────────
    // Each cache sees a private busy that depends only on FSM state
    // (registered) and its own pending bit — never on the other cache's
    // state.  rdata is always the latched resp_rdata; the cache only
    // reads it when busy drops, which only happens in S_DONE for the
    // last owner.
    always_comb begin
        o_d_busy = 1'b0;
        o_i_busy = 1'b0;

        case (state)
            S_IDLE: begin
                // About to be picked: drive busy=1 so the cache's
                // STALL stays asserted across the IDLE→BUSY edge.
                o_d_busy = pick_d;
                o_i_busy = pick_i;
            end
            S_BUSY: begin
                // Owner sees busy=1 for the duration of the bus cycle.
                // Other cache, if also waiting, sees busy=1 too — its
                // request will be picked once we return to IDLE.
                o_d_busy = (owner == 1'b0) || pending_d;
                o_i_busy = (owner == 1'b1) || pending_i;
            end
            S_DONE: begin
                // Last owner sees busy=0 with rdata valid; this is the
                // single cycle in which it captures its data and exits
                // STALL.  The other cache (if waiting) still sees
                // busy=1 — we haven't accepted its request yet.
                o_d_busy = (owner == 1'b1) && pending_d;
                o_i_busy = (owner == 1'b0) && pending_i;
            end
            default: begin
                o_d_busy = 1'b0;
                o_i_busy = 1'b0;
            end
        endcase
    end

    assign o_d_rdata = resp_rdata;
    assign o_i_rdata = resp_rdata;

endmodule

// verilator lint_on UNUSEDSIGNAL
