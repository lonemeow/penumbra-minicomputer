// Penumbra TLB translation verdict cone — 2-way match + permission
//
// Pure combinational. Given the two ways of one set (raw VPN/PTE words +
// the V bit each) and a query (VPN, ASID, access type, user mode, page
// offset), it produces the translation verdict: paddr, cacheability, hit,
// and a permission-fault flag. It does not compose FAULT_STATUS — per the
// detector-composes rule (Decision 16), the MMU wrapper owns that, from its
// own registered copy of the query.
//
// This is the gen2 main TLB's per-lookup cone. The BRAM-backed main TLB
// (penumbra2_tlb) reads both ways of a set out of dual-port BRAM and feeds them
// here once per translation port — so the same cone serves the I-side
// (port A) and D-side (port B) translations from one piece of RTL.
//
// Entry word layout (shared with the rest of the MMU):
//   VPN word: {4'b0, VPN[19:0], ASID[7:0]}
//   PTE word: {PPN[19:0], SW[3:0], flags[7:0]}, flags = G U X W R C - V
// The V bit is supplied separately (i_w*_v) because the BRAM copy of it is
// stale after reset — the live value lives in a flop vector upstream.

// verilator lint_off UNUSEDSIGNAL

// keep_hierarchy: hold this boundary through synth_ecp5 so the translate
// verdict cone reads with real signal names in timing reports and places as
// a unit. Paired across the TLB cone modules (penumbra2_mmu / penumbra2_tlb_unit /
// penumbra2_tlb / penumbra2_tlb_perm).
(* keep_hierarchy = "yes" *)
module penumbra2_tlb_perm
    import penumbra_pkg::*;
(
    // ── Query ──────────────────────────────────────────────
    input  logic        i_lookup_en,     // 0 → no translation this lookup (outputs idle)
    input  logic [19:0] i_vpn,           // virtual page number to match
    input  logic [7:0]  i_asid,          // current ASID (from MMUCR)
    input  logic [2:0]  i_access_type,   // ACC_READ / ACC_WRITE / ACC_EXEC (one-hot)
    input  logic        i_user_mode,     // 1 = user (supervisor bypasses the U check)
    input  logic [11:0] i_page_off,      // page offset, concatenated below the PPN

    // ── Way 0 / Way 1 raw entries (read from storage) ──────
    input  logic [31:0] i_w0_vpn_word,
    input  logic [31:0] i_w0_pte_word,
    input  logic        i_w0_v,
    input  logic [31:0] i_w1_vpn_word,
    input  logic [31:0] i_w1_pte_word,
    input  logic        i_w1_v,

    // ── Verdict ────────────────────────────────────────────
    output logic [31:0] o_paddr,         // {PPN, page_off} on hit; {0, page_off} otherwise
    output logic        o_cacheable,     // PTE.C of the matching entry
    output logic        o_hit,           // an entry matched
    output logic        o_fault          // matched but permission denied
);

    // ── Per-way field extraction ───────────────────────────
    logic [19:0] w0_vpn,  w1_vpn;
    logic [7:0]  w0_asid, w1_asid;
    assign w0_vpn  = i_w0_vpn_word[27:8];
    assign w0_asid = i_w0_vpn_word[7:0];
    assign w1_vpn  = i_w1_vpn_word[27:8];
    assign w1_asid = i_w1_vpn_word[7:0];

    // ── Per-way match: V && VPN equal && (global || ASID equal) ──
    logic w0_match, w1_match;
    assign w0_match = i_w0_v && (w0_vpn == i_vpn)
                   && (i_w0_pte_word[TLB_G] || (w0_asid == i_asid));
    assign w1_match = i_w1_v && (w1_vpn == i_vpn)
                   && (i_w1_pte_word[TLB_G] || (w1_asid == i_asid));

    // ── Way select (way 0 wins ties; a correctly-managed TLB never
    //    holds the same VPN/ASID in both ways of a set) + permission ──
    logic        matched;
    logic        matched_c, matched_u;
    logic [2:0]  matched_rwx;   // {X, W, R} of the winning entry
    logic        perm_denied;   // matched entry forbids this access

    // o_paddr's PPN is hoisted out of the hit/miss verdict below: it resolves
    // from w0_match alone (way-0-priority 2:1) instead of the full o_hit
    // reduction, so the physical address — and the L1 tag compare it feeds —
    // settles a LUT level earlier. On a no-match o_hit is 0 and the consumer
    // drops the access on the resulting fault (the L1 gates its request on the
    // fault verdict), so the else leg picking way 1's PPN is a don't-care, not
    // a tie-break. Cacheability and permission stay in the verdict block —
    // they are not on the address-to-tag-compare critical path.
    logic [19:0] sel_ppn;
    assign sel_ppn = w0_match ? i_w0_pte_word[31:12] : i_w1_pte_word[31:12];
    assign o_paddr = i_lookup_en ? {sel_ppn, i_page_off} : {20'b0, i_page_off};

    always_comb begin
        matched        = 1'b0;
        matched_c      = 1'b0;
        matched_u      = 1'b0;
        matched_rwx    = 3'b0;
        perm_denied    = 1'b0;
        o_hit          = 1'b0;
        o_cacheable    = 1'b0;
        o_fault        = 1'b0;

        if (i_lookup_en) begin
            if (w0_match) begin
                matched     = 1'b1;
                matched_c   = i_w0_pte_word[TLB_C];
                matched_u   = i_w0_pte_word[TLB_U];
                matched_rwx = {i_w0_pte_word[TLB_X], i_w0_pte_word[TLB_W], i_w0_pte_word[TLB_R]};
            end else if (w1_match) begin
                matched     = 1'b1;
                matched_c   = i_w1_pte_word[TLB_C];
                matched_u   = i_w1_pte_word[TLB_U];
                matched_rwx = {i_w1_pte_word[TLB_X], i_w1_pte_word[TLB_W], i_w1_pte_word[TLB_R]};
            end

            o_hit       = matched;
            o_cacheable = matched_c;

            // Permission verdict: a matched entry must permit *this* access.
            // Denied if the requested one-hot access bit is absent from the
            // entry's {X,W,R}, or if a user-mode access hits an entry with
            // U=0 (supervisor is exempt). A miss is not a fault here — that
            // is a TLB miss the caller handles.
            perm_denied = ((matched_rwx & i_access_type) == 3'b0)
                       || (i_user_mode && !matched_u);

            o_fault = matched && perm_denied;
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // These check this module's logic and its hardware-driven input
    // contract — not software TLB-management policy. (A duplicate entry in
    // both ways, for instance, is a software error the hardware still
    // resolves deterministically by way-0 priority, so it is not asserted
    // here; catching that belongs in a software/integration check.)
    // ══════════════════════════════════════════════════════════

    // The permission check ANDs the one-hot access type against {X,W,R}: a
    // multi-hot type would permit on any matching bit, a zero type would deny
    // everything. The access type is driven by the core's decode, so a
    // non-one-hot value on an enabled lookup is a hardware bug.
    always_comb begin
        assert (!i_lookup_en || $onehot(i_access_type))
            else $error("penumbra2_tlb_perm: access type not one-hot on an enabled lookup");
    end

    // A protection fault implies a hit — this module sets o_fault only from a
    // matched entry that forbids the access. fault && !hit would be a fault on
    // an unmapped page (a miss, which the MMU vectors differently); a
    // self-check on the logic above.
    always_comb begin
        assert (!o_fault || o_hit)
            else $error("penumbra2_tlb_perm: fault asserted without a hit");
    end

endmodule

// verilator lint_on UNUSEDSIGNAL
