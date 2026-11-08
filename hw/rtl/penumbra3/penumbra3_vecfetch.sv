// penumbra3_vecfetch -- Penumbra/3 exception vector-fetch FSM.
//
// The one sequential ("multi-cycle") part of exception entry. When the commit
// point takes a fault (i_fault_commit), the handler address must be read from
// the vector table at physical vector_table[vec<<2] -- an indirect load the
// normal pipeline can't issue, because it was just flushed. This small FSM
// performs it, then redirects fetch to the handler.
//
// The read is the L1-I's access with no lookup: an uncacheable read at a
// physical address (doc/internals/penumbra3/l1-cache.md, "The fetch side").
// While o_active is high IF1 is held, so the FSM has the fetch side to
// itself. No translation and no lookup is mandatory: the vector page is
// reachable with no TLB entry, entry never recurses into a TLB miss, and a
// handler software has just stored is read rather than a stale line.
//
// The fault may have killed a fetch whose access is still in flight, and the
// L1-I takes one access at a time, so the FSM waits until it is not busy
// before it asks. No flush is needed on the redirect -- the pipeline
// downstream was already emptied by the fault-commit flush.
//
//   IDLE  -- dormant; the fetch side belongs to IF1.
//   DRIVE -- wait out any access in flight, then ask for vec<<2.
//   WAIT  -- at the read's done capture the handler word and return.
//
// The redirect fires from a register, one cycle after the done, with the
// captured handler word. o_active covers the redirect cycle, so the FSM owns
// the fetch side until its redirect has fired.

module penumbra3_vecfetch (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Launch (from the commit point) ───────────────────────────
    input  logic        i_fault_commit,    // a fault was taken this cycle
    input  logic [3:0]  i_fault_vec,        // its vector number

    // ── The read: the L1-I's access ──────────────────────────────
    input  logic        i_busy,             // the L1-I is busy
    input  logic        i_access_done,      // the L1-I's access is done (one cycle)
    input  logic [31:0] i_access_word,      // handler address, valid from the done
    output logic        o_active,           // the FSM owns the fetch side + holds IF1
    output logic        o_req,              // one cycle: ask for the table word
    output logic [31:0] o_req_addr,         // vector_table[vec<<2], physical

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
        if (i_rst)                                  vec_q <= 4'd0;
        else if (state == S_IDLE && i_fault_commit) vec_q <= i_fault_vec;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) state <= S_IDLE;
        else       state <= next_state;
    end

    // Per-state work + transition, together so each state reads as "do this,
    // therefore advance". o_req and o_redirect are the control strobes (the
    // actual work); the data outputs below are state-independent muxes. A done
    // in DRIVE is the killed fetch's.
    logic read_done;
    assign read_done = (state == S_WAIT) && i_access_done;

    always_comb begin
        next_state = state;
        case (state)
            S_IDLE: begin
                // Dormant; launch entry when the commit point takes a fault.
                if (i_fault_commit) next_state = S_DRIVE;
            end
            S_DRIVE: begin
                // Hold until the L1-I is not busy. The request (o_req below)
                // fires with this transition.
                if (!i_busy) next_state = S_WAIT;
            end
            S_WAIT: begin
                if (read_done) next_state = S_IDLE;
            end
            default: next_state = S_IDLE;
        endcase
    end

    assign o_req = (state == S_DRIVE) && !i_busy;

    // ── Registered redirect: captured word, next-cycle pulse ─────
    logic        redirect_q;
    logic [31:0] handler_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) redirect_q <= 1'b0;
        else       redirect_q <= read_done;
        if (read_done) handler_q <= i_access_word;
    end
    assign o_redirect = redirect_q;

    // ── Data outputs (function of the latched vector / captured word) ─
    // Ownership covers the redirect cycle: the fetch side stays the FSM's until
    // the redirect has fired, so IF1 cannot slip a launch in between.
    assign o_active      = (state != S_IDLE) || redirect_q;
    assign o_req_addr    = {26'b0, vec_q, 2'b00};   // vec<<2, physical
    assign o_redirect_pc = handler_q;               // captured at the done

    // ══════════════════════════════════════════════════════════
    // Assertions -- sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // No nested entry during a vector fetch: interrupts are masked and the
    // access is to a backed physical page, so a second fault cannot arrive
    // while the FSM is mid-fetch (including the trailing redirect cycle).
    // Sampled at the clock edge -- fault_commit and state are both registered,
    // so the check is on their edge-aligned values, not on combinational
    // settling within the cycle.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_fault_commit |-> !o_active)
        else $error("penumbra3_vecfetch: fault commit during an in-progress vector fetch");

    // The redirect fires exactly one cycle after the read completes -- the FSM
    // is back in IDLE, still owning the port through o_active.
    assert property (@(posedge i_clk) disable iff (i_rst)
        o_redirect |-> state == S_IDLE)
        else $error("penumbra3_vecfetch: redirect outside the post-completion cycle");

endmodule
