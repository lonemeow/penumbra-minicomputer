// penumbra3_tlb_verdict -- the MEM2 translation verdict cone.
//
// Pure combinational. Given one set's two ways (registered out of the main
// TLB BRAM) plus the pinned-TLB result, and the access query, it produces
// the verdict: paddr, hit, cacheability, and the protection-fault flag.
// This is the timing-critical cone P0.3 measures -- it sits directly behind
// the 5.8 ns BRAM Tco, so its depth is what the period budget must absorb.
//
// Entry layout (sysreg TLB interface):
//   vpn_word = {4'b0, VPN[19:0], ASID[7:0]}
//   pte_word = {PPN[19:0], SW[7:0], flags}, flags = G U X W R C - V
// The valid bit is supplied separately (i_valid) -- see penumbra3_tlb_store.
module penumbra3_tlb_verdict
    import penumbra_pkg::*;
#(
    parameter int WAYS = 2
) (
    input  logic                  i_lookup_en,
    input  logic [19:0]           i_vpn,         // access VPN (MEM2)
    input  logic [7:0]            i_asid,        // current ASID
    input  logic [2:0]            i_access_type, // ACC_READ/WRITE/EXEC (one-hot)
    input  logic                  i_user_mode,   // 1 = user
    input  logic [11:0]           i_page_off,

    // Main-TLB set (both ways), registered out of the BRAM
    input  logic [WAYS-1:0]       i_valid,
    input  logic [WAYS-1:0][31:0] i_vpn_word,
    input  logic [WAYS-1:0][31:0] i_pte_word,

    // Pinned-TLB result (async, resolved in MEM2; pinned-hit-wins)
    input  logic                  i_pinned_hit,
    input  logic [31:0]           i_pinned_pte_word,

    output logic [31:0]           o_paddr,
    output logic                  o_cacheable,
    output logic                  o_hit,
    output logic                  o_prot_fault   // matched but access denied
);

    // ── Per-way match: V && VPN equal && (global || ASID equal) ──
    logic [WAYS-1:0] way_match;
    always_comb begin
        for (int w = 0; w < WAYS; w++)
            way_match[w] = i_valid[w]
                        && (i_vpn_word[w][27:8] == i_vpn)
                        && (i_pte_word[w][TLB_G] || (i_vpn_word[w][7:0] == i_asid));
    end

    logic main_hit;
    assign main_hit = |way_match;

    // Way select: way 0 wins ties (a correctly-managed TLB never holds the
    // same VPN/ASID in two ways of a set, so a tie is a software error the
    // hardware still resolves deterministically).
    logic [31:0] main_pte;
    always_comb begin
        main_pte = i_pte_word[0];
        for (int w = WAYS - 1; w >= 0; w--)
            if (way_match[w]) main_pte = i_pte_word[w];
    end

    // Pinned-hit-wins: select the pinned entry's PTE when it hit, else the
    // matched main-TLB entry's. On a full miss sel_pte is a don't-care --
    // o_hit is low and the consumer drops the access on the resulting miss.
    logic [31:0] sel_pte;
    assign sel_pte = i_pinned_hit ? i_pinned_pte_word : main_pte;

    assign o_hit       = i_lookup_en && (i_pinned_hit || main_hit);
    assign o_cacheable = sel_pte[TLB_C];
    assign o_paddr     = i_lookup_en ? {sel_pte[31:12], i_page_off}
                                     : {20'b0, i_page_off};

    // ── Protection verdict ───────────────────────────────────────
    // A hit must permit *this* access; deny raises a protection fault (a
    // miss is not a fault here -- the consumer handles ~o_hit as a TLB miss).
    // TODO(human): drive prot_denied from the selected entry's permission
    // bits and the query. sel_pte's flags: TLB_R/TLB_W/TLB_X are the per-mode
    // permissions, TLB_U marks user-accessible. i_access_type is one-hot
    // {X,W,R} (ACC_READ/WRITE/EXEC); i_user_mode is set for user accesses
    // (supervisor is exempt from the U check). Compare against gen2's
    // penumbra2_tlb_perm for the established policy.
    logic [2:0] matched_rwx;
    logic       matched_u; 
    assign matched_rwx = {sel_pte[TLB_X], sel_pte[TLB_W], sel_pte[TLB_R]};
    assign matched_u   = sel_pte[TLB_U];

    logic prot_denied;
    assign prot_denied = ((matched_rwx & i_access_type) == 3'b0) | (i_user_mode & !matched_u);

    assign o_prot_fault = o_hit && prot_denied;

    // ── Assertions (combinational, Verilator --assert) ───────────
    // The permission check treats i_access_type as one-hot; the core's decode
    // guarantees that, so a non-one-hot value on an enabled lookup is a bug.
    always_comb
        assert (!i_lookup_en || $onehot(i_access_type))
            else $error("penumbra3_tlb_verdict: access type not one-hot");

    // A protection fault implies a hit -- it is only raised on a matched
    // entry that forbids the access.
    always_comb
        assert (!o_prot_fault || o_hit)
            else $error("penumbra3_tlb_verdict: protection fault without a hit");

endmodule
