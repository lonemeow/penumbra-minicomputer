// penumbra2_vecfetch — Penumbra/2 exception vector-fetch FSM.
//
// The one sequential ("multi-cycle") part of exception entry. When the commit
// point takes a fault (i_fault_commit), the handler address must be read from
// the vector table at physical vector_table[vec<<2] — an indirect load the
// normal pipeline can't issue, because it was just flushed. This small FSM
// performs it, then redirects fetch to the handler.
//
// It drives the instruction-fetch port (shared, muxed in the core: while
// o_active is high the FSM owns the port and IF1 is held). The vector table is
// read at a physical address with MMU translation bypassed — mandatory, so the
// vector page is reachable with no TLB entry and entry never recurses into a
// TLB miss. In the unified-memory stand-in the address (vec<<2, a low RAM
// address) reads the handler word the kernel stored there earlier; in the real
// machine the same physical access goes through the cache hierarchy.
//
// Timing tracks the front port's launch/resolve contract: launch the read
// into an idle port (the fault may have killed a fetch whose fill is still
// draining — no lookup may launch while the port is busy), then hold the
// read request until the completion cycle — i_mem_busy low, drop-equals-
// valid. Against the flat stand-in (busy tied low) that is one cycle each,
// the original 2-cycle entry; in the machine the vector read is a bypass-
// translated (hence uncacheable) pass-through, so the completion waits out
// the downstream round trip. No flush is needed on the redirect — the
// pipeline downstream was already emptied by the fault-commit flush.
//
//   IDLE  — dormant; the fetch port belongs to IF1.
//   DRIVE — own the port; once it is idle, drive vec<<2 + read-enable
//           (launch the read).
//   WAIT  — hold the read request; at the completion cycle the handler
//           word is on i_mem_rdata: redirect PC to it, return.

module penumbra2_vecfetch (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Launch (from the commit point) ───────────────────────────
    input  logic        i_fault_commit,    // a fault was taken this cycle
    input  logic [3:0]  i_fault_vec,        // its vector number

    // ── Front port (shared with IF2; muxed onto port A in the core) ──
    input  logic [31:0] i_mem_rdata,        // handler address, valid at completion
    input  logic        i_mem_busy,         // port mid-transaction: no launch / not done

    // ── Fetch-port ownership ─────────────────────────────────────
    output logic        o_active,           // FSM owns the fetch port + holds IF1
    output logic [31:0] o_fetch_addr,       // vector_table[vec<<2], physical
    output logic        o_fetch_en,         // launch the table read
    output logic        o_fetch_re,         // the read request, held until completion

    // ── PC redirect to the handler ───────────────────────────────
    output logic        o_redirect,
    output logic [31:0] o_redirect_pc
);

    localparam logic [1:0] S_IDLE  = 2'd0;
    localparam logic [1:0] S_DRIVE = 2'd1;
    localparam logic [1:0] S_WAIT  = 2'd2;

    logic [1:0] state, next_state;
    logic [3:0] vec_q;

    // Latch the vector number when entry launches; it indexes the table read
    // a cycle later in DRIVE.
    always_ff @(posedge i_clk) begin
        if (i_rst)                                vec_q <= 4'd0;
        else if (state == S_IDLE && i_fault_commit) vec_q <= i_fault_vec;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) state <= S_IDLE;
        else       state <= next_state;
    end

    // Per-state work + transition, together so each state reads as "do this,
    // therefore advance". o_fetch_en and o_redirect are the control strobes
    // (the actual work); the data outputs below are state-independent muxes.
    always_comb begin
        next_state = state;
        o_fetch_en = 1'b0;
        o_fetch_re = 1'b0;
        o_redirect = 1'b0;
        case (state)
            S_IDLE: begin
                // Dormant; launch entry when the commit point takes a fault.
                if (i_fault_commit) next_state = S_DRIVE;
            end
            S_DRIVE: begin
                // Launch the vector-table read — into an idle port only. The
                // fault may have killed a fetch whose fill is still draining;
                // that transaction completes into the void first (no lookup
                // may launch while the port is busy), then the launch fires.
                if (!i_mem_busy) begin
                    o_fetch_en = 1'b1;
                    next_state = S_WAIT;
                end
            end
            S_WAIT: begin
                // The read launched in DRIVE resolves here. Hold the request
                // level until the completion cycle (busy low, drop-equals-
                // valid — the first WAIT cycle against the flat stand-in; the
                // busy drop of the pass-through round trip in the machine),
                // then redirect PC to the handler word and finish — downstream
                // was already flushed at the fault commit, no flush needed.
                o_fetch_re = 1'b1;
                if (!i_mem_busy) begin
                    o_redirect = 1'b1;
                    next_state = S_IDLE;
                end
            end
            default: next_state = S_IDLE;
        endcase
    end

    // ── Data outputs (function of the latched vector / the read data) ─
    assign o_active      = (state != S_IDLE);
    assign o_fetch_addr  = {26'b0, vec_q, 2'b00};   // vec<<2, physical (RAM region: bit31=0)
    assign o_redirect_pc = i_mem_rdata;             // handler word, valid at completion

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // No nested entry during a vector fetch: interrupts are masked and the
    // access is to a backed physical page, so a second fault cannot arrive
    // while the FSM is mid-fetch. Sampled at the clock edge — fault_commit and
    // state are both registered, so the check is on their edge-aligned values,
    // not on combinational settling within the cycle.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_fault_commit |-> state == S_IDLE)
        else $error("penumbra2_vecfetch: fault commit during an in-progress vector fetch");

    // The redirect fires only from WAIT, when the handler word is valid.
    always_comb begin
        assert (!o_redirect || state == S_WAIT)
            else $error("penumbra2_vecfetch: redirect outside the WAIT state");
    end

endmodule
