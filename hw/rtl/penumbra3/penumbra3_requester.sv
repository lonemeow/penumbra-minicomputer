// penumbra3_requester -- an L1's access to the uncore: the one request on
// its side of the core-to-uncore port, and the install of the line a line
// read brings back.
//
// penumbra3_l1 issues an access as its client requests one (i_issue). The
// cell holds the request up from flops until the uncore's done
// (doc/internals/penumbra3/uncore-port.md, "Handshake"). In the done cycle
// it installs the line of a read of a cacheable page into the way the L1's
// replacement named at the issue, unless the done faulted or the L1 is
// disabled. It keeps the word the access's address selects and the done's
// fault from the cycle after the done until the next done. A write returns
// nothing and installs nothing.
//
// One access, cycle by cycle. i is the issue, r the uncore's done:
//
//   i     i_issue: the access is latched, with the way it would fill
//   i+1   o_req_valid rises; the request is held
//   r     i_rsp_done. o_fill_we installs a line read's line
//   r+1   o_req_valid is low. o_rsp_word and o_rsp_fault hold r's result
//         until the next done
module penumbra3_requester #(
    parameter  int WAYS       = 2,      // the L1's ways
    parameter  int LINE_WORDS = 4,      // the port's line
    localparam int WAY_BITS   = $clog2(WAYS),
    localparam int WORD_BITS  = $clog2(LINE_WORDS)
) (
    input  logic                i_clk,
    input  logic                i_rst,

    // -- The access ------------------------------------------------
    input  logic                i_issue,          // one cycle; never while o_req_valid
    input  logic                i_we,             // a write
    input  logic [31:0]         i_addr,           // physical
    input  logic                i_cacheable,      // the translation's verdict
    input  logic [3:0]          i_byte_en,
    input  logic [31:0]         i_wdata,          // each byte in its lane
    input  logic [WAY_BITS-1:0] i_victim_way,     // where the L1 would fill this line
    input  logic                i_enabled,        // the L1's CTRL.ENABLE

    // -- Core-to-uncore port, this side ----------------------------
    output logic                o_req_valid,
    output logic                o_req_cacheable,
    output logic [31:0]         o_req_addr,
    output logic                o_req_we,
    output logic [3:0]          o_req_byte_en,
    output logic [31:0]         o_req_wdata,
    input  logic                i_rsp_done,       // one cycle, only while o_req_valid
    input  logic                i_rsp_fault,      // valid with i_rsp_done
    input  logic [LINE_WORDS-1:0][31:0] i_rsp_line,

    // -- Line install (the line runs from the uncore to the arrays) --
    output logic                o_fill_we,
    output logic [WAY_BITS-1:0] o_fill_way,
    output logic [31:0]         o_fill_paddr,

    // -- The result, from the cycle after the done -----------------
    output logic [31:0]         o_rsp_word,
    output logic                o_rsp_fault
);

    // ── The request ──────────────────────────────────────────────
    // Loaded every cycle no request is up, so only req_valid_q carries the
    // issue decision, which waits on the TLB verdict and the L1 hit; these
    // registers' enables stay off it.
    logic [31:0]         addr_q, wdata_q;
    logic                cacheable_q, we_q;
    logic [3:0]          byte_en_q;
    logic [WAY_BITS-1:0] way_q;

    // req_valid_q: a request is up on the port. High from i+1 through r.
    logic req_valid_q;

    always_ff @(posedge i_clk)
        if (!req_valid_q) begin
            addr_q      <= i_addr;
            cacheable_q <= i_cacheable;
            we_q        <= i_we;
            byte_en_q   <= i_byte_en;
            wdata_q     <= i_wdata;
            way_q       <= i_victim_way;
        end

    always_ff @(posedge i_clk)
        if (i_rst)           req_valid_q <= 1'b0;
        else if (i_issue)    req_valid_q <= 1'b1;
        else if (i_rsp_done) req_valid_q <= 1'b0;

    assign o_req_valid     = req_valid_q;
    assign o_req_cacheable = cacheable_q;
    assign o_req_addr      = addr_q;
    assign o_req_we        = we_q;
    assign o_req_byte_en   = byte_en_q;
    assign o_req_wdata     = wdata_q;

    // ── The install ──────────────────────────────────────────────
    // A read of a cacheable page is a line read. A faulted one installs
    // nothing (uncore-port.md, "Faults"), and neither does one that
    // completes with the L1 disabled: a disabled L1 passes its accesses to
    // the next level.
    logic line_read;
    assign line_read = !we_q && cacheable_q;

    assign o_fill_we    = i_rsp_done && line_read && !i_rsp_fault && i_enabled;
    assign o_fill_way   = way_q;
    assign o_fill_paddr = addr_q;

    // ── The result ───────────────────────────────────────────────
    // The uncore's response lasts only r, so it is captured there. A line
    // read's word is the one the address selects; a word read's comes back
    // in word 0.
    logic [WORD_BITS-1:0] rsp_word_idx;
    assign rsp_word_idx = cacheable_q ? addr_q[2 +: WORD_BITS] : '0;

    logic [31:0] rsp_word_q;
    logic        rsp_fault_q;
    always_ff @(posedge i_clk)
        if (i_rsp_done) begin
            rsp_word_q  <= i_rsp_line[rsp_word_idx];
            rsp_fault_q <= i_rsp_fault;
        end

    assign o_rsp_word  = rsp_word_q;
    assign o_rsp_fault = rsp_fault_q;

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // One request at a time, and the uncore answers it once.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_issue && req_valid_q))
        else $error("penumbra3_requester: an access issued while a request is up");

    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_rsp_done && !req_valid_q))
        else $error("penumbra3_requester: done with no request up");

endmodule
