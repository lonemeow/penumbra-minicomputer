// Penumbra Pinned TLB — fully-associative translation buffer
//
// Slot count is set by NUM_ENTRIES (default 8).  All entries compare
// in parallel; the priority encoder and output mux are the only
// structures whose depth grows with NUM_ENTRIES.
//
// Holds permanently-mapped entries that must never cause TLB misses
// (e.g., TLB miss handler code, page global directory).
//
// Checked in parallel with the main set-associative TLB.  A pinned
// hit takes priority over the main TLB.
//
// Entry format is identical to the main TLB:
//   VPN word: {4'b0, VPN[19:0], ASID[7:0]}
//   PTE word: {PPN[19:0], SW[3:0], flags[7:0]}
//
// Module interface:
//   Lookup port (i_vaddr/i_access_type/i_user_mode/i_asid/i_lookup_en →
//   o_paddr/o_cacheable/o_hit/o_fault/o_fault_status) runs fully
//   combinational and is checked in parallel with the main TLB.  The
//   wrapper (tlb_unit) selects pinned-vs-main from the o_hit pair.
//
//   Indexed read/write port (i_idx/i_write_vpn/i_write_pte/i_write_en
//   → o_read_vpn/o_read_pte) addresses one of the 4 slots directly.
//   Software does not see PIN_-prefixed sysregs: tlb_unit reuses the
//   shared SYSREG_MMU_TLB_VPN/SYSREG_MMU_TLB_PTE/SYSREG_MMU_TLB_IDX
//   sysregs and drives this port when IDX[6]=1.
//
// Entries are never evicted by the main TLB's replacement logic.

// verilator lint_off UNUSEDSIGNAL

module tlb_pinned
    import penumbra_pkg::*;
#(
    parameter NUM_ENTRIES = 8,
    // DUAL_TRANSLATE=1 adds a second concurrent lookup port (B). The entry
    // flops feed any number of comparators, so port B is a replicated
    // combinational cone — no second storage. =0 generates none of it.
    parameter bit DUAL_TRANSLATE = 1'b0
) (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Lookup port A (parallel with main TLB) ────────────
    input  logic [31:0] i_vaddr,
    input  logic [2:0]  i_access_type,  // ACC_READ / ACC_WRITE / ACC_EXEC
    input  logic        i_user_mode,
    input  logic [7:0]  i_asid,
    input  logic        i_lookup_en,

    output logic [31:0] o_paddr,
    output logic        o_cacheable,
    output logic        o_hit,
    output logic        o_fault,
    output logic [31:0] o_fault_status,

    // ── Lookup port B (DUAL_TRANSLATE only) — shares i_asid ──
    input  logic [31:0] i_b_vaddr,
    input  logic [2:0]  i_b_access_type,
    input  logic        i_b_user_mode,
    input  logic        i_b_lookup_en,
    output logic [31:0] o_b_paddr,
    output logic        o_b_cacheable,
    output logic        o_b_hit,
    output logic        o_b_fault,
    output logic [31:0] o_b_fault_status,

    // ── Indexed read/write (sysreg access) ─────────────────
    input  logic [$clog2(NUM_ENTRIES)-1:0] i_idx,  // Slot index
    input  logic [31:0] i_write_vpn,
    input  logic [31:0] i_write_pte,
    input  logic        i_write_en,
    output logic [31:0] o_read_vpn,
    output logic [31:0] o_read_pte
);

    // ══════════════════════════════════════════════════════════
    // Storage — NUM_ENTRIES × 64 bits
    // ══════════════════════════════════════════════════════════

    logic [31:0] entries_vpn [0:NUM_ENTRIES-1];
    logic [31:0] entries_pte [0:NUM_ENTRIES-1];

    // ══════════════════════════════════════════════════════════
    // Lookup logic — fully associative (check all entries)
    // ══════════════════════════════════════════════════════════

    logic [19:0] lookup_vpn;
    assign lookup_vpn = i_vaddr[31:12];

    // Per-entry match signals
    logic [NUM_ENTRIES-1:0] entry_match;
    logic [19:0] entry_ppn   [0:NUM_ENTRIES-1];
    logic        entry_c     [0:NUM_ENTRIES-1];
    logic        entry_u     [0:NUM_ENTRIES-1];
    logic [2:0]  entry_rwx   [0:NUM_ENTRIES-1];

    genvar g;
    generate
        for (g = 0; g < NUM_ENTRIES; g++) begin : gen_match
            logic [19:0] e_vpn;
            logic [7:0]  e_asid;
            logic        e_v, e_g;

            assign e_vpn  = entries_vpn[g][27:8];
            assign e_asid = entries_vpn[g][7:0];
            assign e_v    = entries_pte[g][TLB_V];
            assign e_g    = entries_pte[g][TLB_G];

            assign entry_match[g] = e_v
                                 && (e_vpn == lookup_vpn)
                                 && (e_g || (e_asid == i_asid));

            assign entry_ppn[g] = entries_pte[g][31:12];
            assign entry_c[g]   = entries_pte[g][TLB_C];
            assign entry_u[g]   = entries_pte[g][TLB_U];
            assign entry_rwx[g] = {entries_pte[g][TLB_X],
                                   entries_pte[g][TLB_W],
                                   entries_pte[g][TLB_R]};
        end
    endgenerate

    // ── Priority encoder: lowest-numbered matching entry wins ──

    logic        matched;
    logic [19:0] matched_ppn;
    logic        matched_c, matched_u;
    logic [2:0]  matched_rwx;
    logic        perm_ok;

    always_comb begin
        matched     = 1'b0;
        matched_ppn = 20'b0;
        matched_c   = 1'b0;
        matched_u   = 1'b0;
        matched_rwx = 3'b0;
        o_hit       = 1'b0;
        o_fault     = 1'b0;
        o_fault_status = 32'b0;
        o_paddr     = {20'b0, i_vaddr[11:0]};
        o_cacheable = 1'b0;
        perm_ok     = 1'b0;

        if (i_lookup_en) begin
            // Find first matching entry (lowest index wins)
            for (int i = 0; i < NUM_ENTRIES; i++) begin
                if (!matched && entry_match[i]) begin
                    matched     = 1'b1;
                    matched_ppn = entry_ppn[i];
                    matched_c   = entry_c[i];
                    matched_u   = entry_u[i];
                    matched_rwx = entry_rwx[i];
                end
            end

            o_hit   = matched;
            o_paddr = {matched_ppn, i_vaddr[11:0]};
            o_cacheable = matched_c;

            // Permission check — same logic as main TLB
            perm_ok = |(i_access_type & matched_rwx);
            if (i_user_mode && !matched_u)
                perm_ok = 1'b0;

            if (matched && !perm_ok) begin
                o_fault = 1'b1;
                o_fault_status = {20'b0, i_user_mode, i_access_type, 4'b0, FAULT_PROT};
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Lookup port B (DUAL_TRANSLATE) — second concurrent reader of the
    // same entry flops; mirrors the port-A cone above.
    // ══════════════════════════════════════════════════════════
    generate
    if (DUAL_TRANSLATE) begin : gen_port_b
        logic [19:0] lookup_vpn_b;
        assign lookup_vpn_b = i_b_vaddr[31:12];

        logic [NUM_ENTRIES-1:0] entry_match_b;
        for (genvar gb = 0; gb < NUM_ENTRIES; gb++) begin : gen_match_b
            logic [19:0] eb_vpn;
            logic [7:0]  eb_asid;
            logic        eb_v, eb_g;
            assign eb_vpn  = entries_vpn[gb][27:8];
            assign eb_asid = entries_vpn[gb][7:0];
            assign eb_v    = entries_pte[gb][TLB_V];
            assign eb_g    = entries_pte[gb][TLB_G];
            assign entry_match_b[gb] = eb_v
                                    && (eb_vpn == lookup_vpn_b)
                                    && (eb_g || (eb_asid == i_asid));
        end

        logic        matched_b, perm_ok_b;
        logic [19:0] matched_ppn_b;
        logic        matched_c_b, matched_u_b;
        logic [2:0]  matched_rwx_b;

        always_comb begin
            matched_b        = 1'b0;
            matched_ppn_b    = 20'b0;
            matched_c_b      = 1'b0;
            matched_u_b      = 1'b0;
            matched_rwx_b    = 3'b0;
            perm_ok_b        = 1'b0;
            o_b_hit          = 1'b0;
            o_b_fault        = 1'b0;
            o_b_fault_status = 32'b0;
            o_b_paddr        = {20'b0, i_b_vaddr[11:0]};
            o_b_cacheable    = 1'b0;

            if (i_b_lookup_en) begin
                for (int i = 0; i < NUM_ENTRIES; i++) begin
                    if (!matched_b && entry_match_b[i]) begin
                        matched_b     = 1'b1;
                        matched_ppn_b = entries_pte[i][31:12];
                        matched_c_b   = entries_pte[i][TLB_C];
                        matched_u_b   = entries_pte[i][TLB_U];
                        matched_rwx_b = {entries_pte[i][TLB_X],
                                         entries_pte[i][TLB_W],
                                         entries_pte[i][TLB_R]};
                    end
                end

                o_b_hit       = matched_b;
                o_b_paddr     = {matched_ppn_b, i_b_vaddr[11:0]};
                o_b_cacheable = matched_c_b;

                perm_ok_b = |(i_b_access_type & matched_rwx_b);
                if (i_b_user_mode && !matched_u_b)
                    perm_ok_b = 1'b0;

                if (matched_b && !perm_ok_b) begin
                    o_b_fault = 1'b1;
                    o_b_fault_status = {20'b0, i_b_user_mode, i_b_access_type, 4'b0, FAULT_PROT};
                end
            end
        end
    end else begin : gen_no_port_b
        assign o_b_paddr        = 32'b0;
        assign o_b_cacheable    = 1'b0;
        assign o_b_hit          = 1'b0;
        assign o_b_fault        = 1'b0;
        assign o_b_fault_status = 32'b0;
    end
    endgenerate

    // ══════════════════════════════════════════════════════════
    // Indexed read/write (sysreg access)
    // ══════════════════════════════════════════════════════════

    assign o_read_vpn = entries_vpn[i_idx];
    assign o_read_pte = entries_pte[i_idx];

    integer j;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            for (j = 0; j < NUM_ENTRIES; j++) begin
                entries_vpn[j] <= 32'b0;
                entries_pte[j] <= 32'b0;
            end
        end else if (i_write_en) begin
            entries_vpn[i_idx] <= i_write_vpn;
            entries_pte[i_idx] <= i_write_pte;
        end
    end

endmodule

// verilator lint_on UNUSEDSIGNAL
