// penumbra3_mem_completion -- the data side's completion unit, for loads.
//
// The back-end's single load-completion point, and the module that keeps
// the cache hit verdict off the issue and MMU cones. A load MEM2 cannot
// serve -- it missed the L1, or its page is uncacheable -- is parked here
// as it leaves MEM2. The unit is then the data side's requester on the
// core-to-uncore port (doc/internals/penumbra3/uncore-port.md): it holds
// one request up until the uncore's done, installs the line a cacheable
// load brought back into the L1, and hands the load's word to the regfile
// a cycle later. The pipeline sees none of it but the registered
// o_mem_pending, which freezes the launch side (MEM1, the MEM1/MEM2
// register, the translate and L1 launches, issue) from the cycle after the
// park.
//
// One load, cycle by cycle. p is the park, r the uncore's done:
//
//   p     i_park: the load's descriptor is latched
//   p+1   o_mem_pending and o_req_valid rise; the request is held
//   r     i_rsp_done with the line. o_fill_we writes it into the L1 (a
//         cacheable load, L1 enabled, no fault); the load's word is taken
//   r+1   o_req_valid is low. o_complete: the regfile write.
//         o_mem_pending is low, unless the slot in MEM2 reads the L1's
//         lookup and the L1 asked in r for an extra cycle
//   r+2   o_mem_pending is low
//
// The descriptor also keeps what the sub-word extract needs (size, sign,
// the address's low bits): the response is a whole word and arrives after
// the load has left the pipe.
module penumbra3_mem_completion #(
    parameter  int IDX_BITS   = 5,      // destination scoreboard-index width
    parameter  int WAYS       = 2,      // the L1's ways
    parameter  int LINE_WORDS = 4,      // the port's line
    localparam int WAY_BITS   = $clog2(WAYS),
    localparam int WORD_BITS  = $clog2(LINE_WORDS)
) (
    input  logic                i_clk,
    input  logic                i_rst,

    // -- Park: a load that MEM2 cannot serve leaves MEM2 this cycle --
    input  logic                i_park,           // one cycle; never while o_mem_pending
    input  logic [IDX_BITS-1:0] i_load_dest,
    input  logic [1:0]          i_load_size,
    input  logic                i_load_sign,
    input  logic [31:0]         i_load_paddr,
    input  logic                i_load_cacheable,
    input  logic [3:0]          i_load_byte_en,
    input  logic [WAY_BITS-1:0] i_victim_way,     // where the L1 would fill its line
    input  logic                i_l1_enabled,     // the L1's CTRL.ENABLE

    // -- The slot in MEM2 this cycle ------------------------------
    // While o_mem_pending is high the MEM1/MEM2 register is frozen, so
    // this is the instruction behind the parked load and it does not
    // change. In the first cycle o_mem_pending is low it takes effect on
    // what the L1 gives its lookup then: it commits, a store writes the L1
    // if it hit, a load that missed parks. Only a load or a store reads
    // the lookup.
    input  logic                i_mem2_slot_mem,  // level: a load or a store

    // -- Core-to-uncore port, data side ---------------------------
    output logic                o_req_valid,
    output logic                o_req_cacheable,
    output logic [31:0]         o_req_addr,
    output logic [3:0]          o_req_byte_en,
    input  logic                i_rsp_done,       // one cycle, only while o_req_valid
    input  logic                i_rsp_fault,      // valid with i_rsp_done
    input  logic [LINE_WORDS-1:0][31:0] i_rsp_line,

    // -- L1 line install (the line runs from the uncore to the L1) --
    output logic                o_fill_we,
    output logic [WAY_BITS-1:0] o_fill_way,
    output logic [31:0]         o_fill_paddr,
    input  logic                i_l1_extra_cycle, // the held lookup needs a cycle more

    // -- Pipeline gate + completion -------------------------------
    output logic                o_mem_pending,    // freezes the launch side
    output logic                o_complete,       // the parked load finished (one cycle)
    output logic [IDX_BITS-1:0] o_complete_dest,
    output logic [31:0]         o_complete_data,  // the whole word; the consumer extracts
    output logic                o_complete_fault,
    output logic [1:0]          o_complete_size,
    output logic [1:0]          o_complete_addr_lo,
    output logic                o_complete_sign
);

    // ── The parked load ──────────────────────────────────────────
    // Loaded every cycle nothing is parked, so only the two flops below
    // carry the park decision, which waits on the TLB verdict and the L1
    // hit; these registers' enables stay off it. From p+1 through the
    // last cycle o_mem_pending is high they hold the parked load.
    logic [IDX_BITS-1:0] dest_q;
    logic [1:0]          size_q;
    logic                sign_q;
    logic [31:0]         paddr_q;
    logic                cacheable_q;
    logic [3:0]          byte_en_q;
    logic [WAY_BITS-1:0] way_q;        // the L1 way its line goes to
    logic                install_q;    // level: r installs the line unless it faulted

    // req_valid_q: a request is up on the port. High from p+1 through r.
    // pending_q:   the launch side is frozen. High from p+1 through r,
    //              and through r+1 as well unless release_at_done was high
    //              in r.
    // settle_q:    high in r+1 when release_at_done was low in r.
    logic req_valid_q, pending_q, settle_q;

    always_ff @(posedge i_clk)
        if (!pending_q) begin
            dest_q      <= i_load_dest;
            size_q      <= i_load_size;
            sign_q      <= i_load_sign;
            paddr_q     <= i_load_paddr;
            cacheable_q <= i_load_cacheable;
            byte_en_q   <= i_load_byte_en;
            way_q       <= i_victim_way;
            install_q   <= i_load_cacheable && i_l1_enabled;
        end

    assign o_req_valid     = req_valid_q;
    assign o_req_cacheable = cacheable_q;
    assign o_req_addr      = paddr_q;
    assign o_req_byte_en   = byte_en_q;

    assign o_fill_we    = i_rsp_done && install_q && !i_rsp_fault;
    assign o_fill_way   = way_q;
    assign o_fill_paddr = paddr_q;

    // ── Release ──────────────────────────────────────────────────
    // release_at_done is read in the done cycle r only. High there:
    // o_mem_pending is low from r+1. Low there: it is low from r+2.
    //
    // The slot in MEM2 takes effect in that first low cycle, on what the
    // L1 gives its lookup then. The L1 gives the cache as it is in r+1
    // unless it asks in r for an extra cycle, and a slot that reads no
    // lookup has nothing to wait for.
    logic release_at_done;
    assign release_at_done = !i_l1_extra_cycle || !i_mem2_slot_mem;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            req_valid_q <= 1'b0;
            pending_q   <= 1'b0;
            settle_q    <= 1'b0;
        end else begin
            settle_q <= i_rsp_done && !release_at_done;

            if (i_park)          req_valid_q <= 1'b1;
            else if (i_rsp_done) req_valid_q <= 1'b0;

            if (i_park)                             pending_q <= 1'b1;
            else if (i_rsp_done && release_at_done) pending_q <= 1'b0;
            else if (settle_q)                      pending_q <= 1'b0;
        end
    end
    assign o_mem_pending = pending_q;

    // ── Completion ───────────────────────────────────────────────
    // Captured in r, since the parked registers above may load the next
    // slot from r+1 on. A line read's word is the one the address selects;
    // a word read's comes back in word 0.
    logic                complete_q;
    logic [IDX_BITS-1:0] complete_dest_q;
    logic [31:0]         complete_data_q;
    logic                complete_fault_q;
    logic [1:0]          complete_size_q;
    logic [1:0]          complete_addr_lo_q;
    logic                complete_sign_q;

    logic [WORD_BITS-1:0] rsp_word_idx;
    assign rsp_word_idx = cacheable_q ? paddr_q[2 +: WORD_BITS] : '0;

    always_ff @(posedge i_clk) begin
        if (i_rst) complete_q <= 1'b0;
        else       complete_q <= i_rsp_done;
        if (i_rsp_done) begin
            complete_dest_q    <= dest_q;
            complete_data_q    <= i_rsp_line[rsp_word_idx];
            complete_fault_q   <= i_rsp_fault;
            complete_size_q    <= size_q;
            complete_addr_lo_q <= paddr_q[1:0];
            complete_sign_q    <= sign_q;
        end
    end

    assign o_complete         = complete_q;
    assign o_complete_dest    = complete_dest_q;
    assign o_complete_data    = complete_data_q;
    assign o_complete_fault   = complete_fault_q;
    assign o_complete_size    = complete_size_q;
    assign o_complete_addr_lo = complete_addr_lo_q;
    assign o_complete_sign    = complete_sign_q;

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // One load at a time: the freeze keeps a second from reaching MEM2's
    // exit while one is parked.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_park && pending_q))
        else $error("penumbra3_mem_completion: a load parked while one is pending");

    // The uncore answers a request, and only once.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_rsp_done && !req_valid_q))
        else $error("penumbra3_mem_completion: done with no request up");

endmodule
