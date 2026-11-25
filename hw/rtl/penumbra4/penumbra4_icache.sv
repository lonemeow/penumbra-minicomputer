// penumbra4_icache — the L1-I: answers fetch lookups and carries the fetch
// side's accesses.
//
// A lookup launched in one cycle gives its verdict, the hit and the line,
// in the next. An access is requested in a lookup's verdict cycle, by IF2
// for a fetch the L1-I had no word for, or with no lookup, by the fetch
// unit for a vector read; it is the fetch side's request on the
// core-to-uncore port (doc/internals/penumbra3/l1-cache.md, "Busy and
// done"). busy is up from the cycle after the request until the uncore's
// done; done follows a cycle later, and the access's result holds from
// then until the next done, so a fetch launched at done takes it.
//
// The L1-I has no arrays: no lookup hits, and every fetch is an access.

module penumbra4_icache
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Lookups ──────────────────────────────────────────────────
    input  logic        i_launch,
    input  vaddr_t      i_launch_vaddr,
    output logic        o_hit,
    output line_t       o_line,

    // ── Accesses ─────────────────────────────────────────────────
    input  logic        i_miss_req,
    input  paddr_t      i_miss_addr,
    input  logic        i_miss_cacheable,
    input  logic        i_vector_req,
    input  paddr_t      i_vector_addr,
    output logic        o_busy,
    output logic        o_done,
    output line_t       o_access_line,  // a line read's line, or a word read's word at its index
    output word_t       o_access_word,  // the word at the access's address
    output logic        o_access_fault,

    // ── The fetch side of the core-to-uncore port ────────────────
    output logic        o_uc_valid,
    output uncore_req_t o_uc_req,
    input  logic        i_uc_done,
    input  logic        i_uc_fault,
    input  line_t       i_uc_line
);

    logic        valid_q, valid_d;
    uncore_req_t req_q, req_d;
    logic        done_q;
    line_t       line_q, line_d;
    word_t       word_q, word_d;
    logic        fault_q;

    always_comb begin
        valid_d = valid_q;
        req_d   = req_q;
        if (i_uc_done) begin
            valid_d = 1'b0;
        end
        if (i_miss_req || i_vector_req) begin
            valid_d         = 1'b1;
            req_d           = '0;
            req_d.byte_en   = 4'b1111;
            if (i_vector_req) begin
                req_d.addr      = i_vector_addr;
                req_d.cacheable = 1'b0;
            end else begin
                req_d.addr      = i_miss_addr;
                req_d.cacheable = i_miss_cacheable;
            end
        end
    end

    // A word read's word arrives as the port's word 0; it goes to the index
    // its address selects, where a line read's would be.
    always_comb begin
        if (is_line_read(req_q)) begin
            line_d = i_uc_line;
        end else begin
            line_d = '0;
            line_d[line_word_of(req_q.addr)] = i_uc_line[0];
        end
        word_d = line_d[line_word_of(req_q.addr)];
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
            line_q  <= line_d;
            word_q  <= word_d;
            fault_q <= i_uc_fault;
        end
    end

    assign o_hit          = 1'b0;
    assign o_line         = '0;
    assign o_busy         = valid_q;
    assign o_done         = done_q;
    assign o_access_line  = line_q;
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
        if (!i_rst) begin
            assert (!(i_miss_req && i_vector_req))
                else $error("penumbra4_icache: two accesses requested in one cycle");
            assert (!((i_miss_req || i_vector_req) && valid_q))
                else $error("penumbra4_icache: an access requested while busy");
        end
    end

    // The uncore's done answers the access this L1 has up.
    assert property (@(posedge i_clk) disable iff (i_rst)
        i_uc_done |-> valid_q)
        else $error("penumbra4_icache: a done with no access up");

endmodule
