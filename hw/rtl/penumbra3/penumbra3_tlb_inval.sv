// penumbra3_tlb_inval -- the gen3 MMU's invalidation sweep: the three
// invalidation registers of MMU_CONFIG version 2 (doc/system/mmu.md,
// "Invalidation Registers").
//
// A sweep reads a set through the D-copy, then writes the ways that match
// back with V cleared, the rest of each PTE word kept: two cycles a set,
// since a store's write suppresses its read. TLB_INVAL_VA visits the one
// set its VPN indexes; TLB_INVAL_ASID and TLB_INVAL_ALL visit every set.
// Only valid ways match, so the entries the TLB holds out of reset, which
// software has not yet invalidated, are cleared by TLB_INVAL_ALL like any
// others.
//
// o_busy is the state register itself. The MMU holds fetch on it, and a
// sweep only starts from a WRSYS, which commits into a drained back end
// whose younger work the resync throws away: so while it runs no lookup
// launches on either copy, nothing else writes them, and the D-copy's read
// port is free to steer.
module penumbra3_tlb_inval
    import penumbra_pkg::*;
#(
    parameter int SETS = 32,
    parameter int WAYS = 2
) (
    input  logic                    i_clk,
    input  logic                    i_rst,

    // Start: one cycle, the invalidation register's WRSYS commit
    input  logic                    i_inval_va,
    input  logic                    i_inval_asid,
    input  logic                    i_inval_all,
    input  logic [27:0]             i_target,       // the written VPN and ASID
    output logic                    o_busy,

    // Read a set through the D-copy; its words arrive the next cycle
    output logic                    o_rd_en,
    output logic [$clog2(SETS)-1:0] o_rd_set,
    input  logic [WAYS-1:0][31:0]   i_rd_vpn_word,
    input  logic [WAYS-1:0][31:0]   i_rd_pte_word,

    // Write the matched ways' PTE words, to both copies
    output logic [$clog2(SETS)-1:0] o_wr_set,
    output logic [WAYS-1:0]         o_wr_pte_en,
    output logic [WAYS-1:0][31:0]   o_wr_pte_word
);

    localparam int SET_BITS = $clog2(SETS);

    typedef enum logic [1:0] {
        INV_IDLE,
        INV_VA,     // TLB_INVAL_VA: valid, VPN equal, ASID equal or G
        INV_ASID,   // TLB_INVAL_ASID: valid, G=0, ASID equal
        INV_ALL     // TLB_INVAL_ALL: valid
    } inval_e;

    inval_e              kind_q;
    logic                checking_q; // this cycle checks the set read last cycle
    logic [SET_BITS-1:0] set_q;
    logic [SET_BITS-1:0] last_q;
    logic [27:0]         target_q;

    logic last_set;
    assign last_set = (set_q == last_q);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            kind_q     <= INV_IDLE;
            checking_q <= 1'b0;
        end else if (kind_q == INV_IDLE) begin
            if (i_inval_va) begin
                kind_q   <= INV_VA;
                set_q    <= i_target[8 +: SET_BITS];   // the VPN's set
                last_q   <= i_target[8 +: SET_BITS];
                target_q <= i_target;
            end else if (i_inval_asid || i_inval_all) begin
                kind_q   <= i_inval_asid ? INV_ASID : INV_ALL;
                set_q    <= '0;
                last_q   <= '1;
                target_q <= i_target;
            end
        end else if (!checking_q) begin
            checking_q <= 1'b1;
        end else begin
            checking_q <= 1'b0;
            if (last_set) kind_q <= INV_IDLE;
            else          set_q  <= set_q + 1'b1;
        end
    end

    assign o_busy   = (kind_q != INV_IDLE);
    assign o_rd_en  = o_busy && !checking_q;
    assign o_rd_set = set_q;
    assign o_wr_set = set_q;

    // The ways of the set read last cycle that the sweep invalidates.
    logic [WAYS-1:0] way_valid, way_global, way_asid_eq, way_vpn_eq, way_match;
    always_comb begin
        for (int w = 0; w < WAYS; w++) begin
            way_valid[w]   = i_rd_pte_word[w][TLB_V];
            way_global[w]  = i_rd_pte_word[w][TLB_G];
            way_asid_eq[w] = (i_rd_vpn_word[w][7:0] == target_q[7:0]);
            way_vpn_eq[w]  = (i_rd_vpn_word[w][27:8] == target_q[27:8]);
            case (kind_q)
                INV_VA:   way_match[w] = way_valid[w] && way_vpn_eq[w]
                                      && (way_global[w] || way_asid_eq[w]);
                INV_ASID: way_match[w] = way_valid[w] && !way_global[w] && way_asid_eq[w];
                INV_ALL:  way_match[w] = way_valid[w];
                default:  way_match[w] = 1'b0;
            endcase
        end
    end

    always_comb begin
        for (int w = 0; w < WAYS; w++) begin
            o_wr_pte_en[w]   = checking_q && way_match[w];
            o_wr_pte_word[w] = i_rd_pte_word[w] & ~(32'b1 << TLB_V);
        end
    end

    // ── Assertions (sim-only; Verilator --assert) ────────────────
    // A sweep starts only from a WRSYS, and fetch is held while one runs,
    // so no second start can arrive before it ends; the register writes
    // decode one at a time.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(o_busy && (i_inval_va || i_inval_asid || i_inval_all)))
        else $error("penumbra3_tlb_inval: an invalidation started during a sweep");
    always_comb
        assert ($onehot0({i_inval_va, i_inval_asid, i_inval_all}))
            else $error("penumbra3_tlb_inval: two invalidations started together");

endmodule
