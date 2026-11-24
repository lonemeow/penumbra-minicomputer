// penumbra4_dcache — the L1-D: answers data lookups and carries the data
// side's accesses.
//
// A lookup launched in one cycle gives its verdict, the hit and the word,
// in the next. An access is requested in a lookup's verdict cycle, by MEM2
// for a load the L1-D had no word for or an uncacheable store's write; it
// is the data side's request on the core-to-uncore port
// (doc/internals/penumbra3/l1-cache.md, "Busy and done"). busy is up from
// the cycle after the request until the uncore's done; done follows a cycle
// later, and the access's word and fault hold from then until the next
// done.
//
// The word is the one the access's address selects: a line read's word at
// addr[3:2], a word read's the port's word 0, its bytes in their bus lanes
// (doc/internals/penumbra3/uncore-port.md, "Read data"). The client
// extracts a sub-word from it.
//
// The L1-D has no arrays: no lookup hits, and every load and every store's
// write is an access.

module penumbra4_dcache
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Lookups ──────────────────────────────────────────────────
    input  logic        i_launch,
    input  vaddr_t      i_launch_vaddr,
    output logic        o_hit,
    output word_t       o_word,

    // ── Accesses ─────────────────────────────────────────────────
    input  logic        i_req,
    input  uncore_req_t i_access,
    output logic        o_done,
    output word_t       o_access_word,
    output logic        o_access_fault,

    // ── The data side of the core-to-uncore port ─────────────────
    output logic        o_uc_valid,
    output uncore_req_t o_uc_req,
    input  logic        i_uc_done,
    input  logic        i_uc_fault,
    input  line_t       i_uc_line
);

    logic        valid_q, valid_d;
    uncore_req_t req_q, req_d;
    logic        done_q;
    word_t       word_q, word_d;
    logic        fault_q;

    always_comb begin
        valid_d = valid_q;
        req_d   = req_q;
        if (i_uc_done) begin
            valid_d = 1'b0;
        end
        if (i_req) begin
            valid_d = 1'b1;
            req_d   = i_access;
        end
    end

    always_comb begin
        if (is_line_read(req_q)) begin
            word_d = i_uc_line[line_word_of(req_q.addr)];
        end else begin
            word_d = i_uc_line[0];
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            valid_q <= 1'b0;
            done_q  <= 1'b0;
        end else begin
            valid_q <= valid_d;
            done_q  <= i_uc_done;
        end
    end

    always_ff @(posedge i_clk) begin
        req_q <= req_d;
    end

    always_ff @(posedge i_clk) begin
        if (i_uc_done) begin
            word_q  <= word_d;
            fault_q <= i_uc_fault;
        end
    end

    assign o_hit          = 1'b0;
    assign o_word         = '0;
    assign o_done         = done_q;
    assign o_access_word  = word_q;
    assign o_access_fault = fault_q;
    assign o_uc_valid     = valid_q;
    assign o_uc_req       = req_q;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_lookup;
    assign unused_lookup = ^{i_launch, i_launch_vaddr};
    /* verilator lint_on UNUSEDSIGNAL */

    // ── Contract ─────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst && i_req) begin
            assert (!valid_q)
                else $error("penumbra4_dcache: an access requested while busy");
            assert (!(i_access.we && i_access.cacheable))
                else $error("penumbra4_dcache: a cacheable write requested, which the store side carries");
        end
    end

endmodule
