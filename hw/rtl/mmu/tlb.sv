// Penumbra TLB — 64-entry 2-way set-associative translation lookaside buffer
//
// Pure lookup hardware: no replacement logic, no dirty tracking.
// Software manages all entries via indexed read/write through the
// sysreg interface.
//
// Storage strategy (distributed-RAM friendly):
//   Storage is split by way (way0/way1, 32 entries each) and
//   replicated by read port (lookup vs sysreg readback).  Each
//   resulting bank has exactly one write port and one async read
//   port — the canonical ECP5 SLICEMEM (DPR16X*) shape.  This
//   replaces the previous flop-storage + 6:1-mux-tree pattern,
//   which forced the lookup to traverse ~12 LUT4 levels before
//   reaching the comparator.  With DPRAM the entry data appears
//   at the bank output through a single SLICEMEM read (~1.5 ns)
//   so the comparator runs near-immediately on cycle start.
//
// V bits live in a small flop vector with synchronous reset:
// SLICEMEMs have no async clear, so we can't reset the DPRAM
// contents themselves on i_rst.  Keeping V in flops preserves the
// original "all-invalid after reset" invariant — any stale data
// in the DPRAM is masked by V==0 until software writes a valid PTE.
//
// Entry format (64 bits, two 32-bit halves):
//   VPN word: {4'b0, VPN[19:0], ASID[7:0]}
//   PTE word: {PPN[19:0], SW[3:0], flags[7:0]}
//   flags:    [7]G [6]U [5]X [4]W [3]R [2]C [1]rsvd [0]V
//
// Lookup: set = vaddr[16:12], compare VPN + ASID on both ways in
// parallel.  Permission check on hit.  Miss/fault signalled to MMU.

// verilator lint_off UNUSEDSIGNAL

module tlb
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Lookup interface (active when MMU M=1) ─────────────
    input  logic [31:0] i_vaddr,        // Virtual address to translate
    input  logic [2:0]  i_access_type,  // ACC_READ / ACC_WRITE / ACC_EXEC (one-hot)
    input  logic        i_user_mode,    // 1 = user mode
    input  logic [7:0]  i_asid,         // Current ASID from MMUCR
    input  logic        i_lookup_en,    // Perform lookup this cycle

    output logic [31:0] o_paddr,        // Translated physical address
    output logic        o_cacheable,    // C bit from matching entry
    output logic        o_hit,          // TLB hit (entry found)
    output logic        o_fault,        // Permission violation on hit
    output logic [31:0] o_fault_status, // Fault reason (valid when o_fault=1)

    // ── Indexed read/write (sysreg access) ─────────────────
    input  logic [4:0]  i_idx_set,      // Set index (0-31)
    input  logic        i_idx_way,      // Way select (0 or 1)
    input  logic [31:0] i_write_vpn,    // TLB_VPN data: {4'b0, VPN[19:0], ASID[7:0]}
    input  logic [31:0] i_write_pte,    // TLB_PTE data: {PPN[19:0], SW[7:0], flags[7:0]}
    input  logic        i_write_en,     // Commit entry (triggered by TLB_PTE write)
    output logic [31:0] o_read_vpn,     // TLB_VPN readback
    output logic [31:0] o_read_pte      // TLB_PTE readback
);

    // ══════════════════════════════════════════════════════════
    // Storage — split by way, replicated by read port
    // ══════════════════════════════════════════════════════════
    // Eight banks total: {way0,way1} × {vpn,pte} × {lookup,sysreg}.
    // Each bank is 32 entries × 32 bits, 1W/1R, distributed-RAM
    // mapped via SLICEMEM.  Lookup and sysreg mirrors are written
    // in lockstep so they always carry identical data.

    // Lookup banks (read at lookup_set)
    (* ram_style = "distributed" *) logic [31:0] way0_vpn_lookup [0:31];
    (* ram_style = "distributed" *) logic [31:0] way1_vpn_lookup [0:31];
    (* ram_style = "distributed" *) logic [31:0] way0_pte_lookup [0:31];
    (* ram_style = "distributed" *) logic [31:0] way1_pte_lookup [0:31];

    // Sysreg readback banks (read at i_idx_set)
    (* ram_style = "distributed" *) logic [31:0] way0_vpn_sysreg [0:31];
    (* ram_style = "distributed" *) logic [31:0] way1_vpn_sysreg [0:31];
    (* ram_style = "distributed" *) logic [31:0] way0_pte_sysreg [0:31];
    (* ram_style = "distributed" *) logic [31:0] way1_pte_sysreg [0:31];

    // V bits — flops with sync reset (one bit per set, per way).
    logic [31:0] way0_v_vec;
    logic [31:0] way1_v_vec;

    // ══════════════════════════════════════════════════════════
    // Lookup logic (combinational)
    // ══════════════════════════════════════════════════════════

    logic [19:0] lookup_vpn;
    logic [4:0]  lookup_set;
    assign lookup_vpn = i_vaddr[31:12];
    assign lookup_set = i_vaddr[16:12];  // Lower 5 bits of VPN

    // Read both ways from lookup banks — single SLICEMEM read each.
    logic [31:0] way0_vpn_data, way0_pte_data;
    logic [31:0] way1_vpn_data, way1_pte_data;
    assign way0_vpn_data = way0_vpn_lookup[lookup_set];
    assign way0_pte_data = way0_pte_lookup[lookup_set];
    assign way1_vpn_data = way1_vpn_lookup[lookup_set];
    assign way1_pte_data = way1_pte_lookup[lookup_set];

    // Extract fields from each way
    // VPN word: {4'b0, VPN[19:0], ASID[7:0]}
    logic [19:0] way0_entry_vpn, way1_entry_vpn;
    logic [7:0]  way0_entry_asid, way1_entry_asid;
    assign way0_entry_vpn  = way0_vpn_data[27:8];
    assign way0_entry_asid = way0_vpn_data[7:0];
    assign way1_entry_vpn  = way1_vpn_data[27:8];
    assign way1_entry_asid = way1_vpn_data[7:0];

    // PTE word: {PPN[19:0], SW[7:0], flags[7:0]}
    // V comes from the flop vector (DPRAM copy is stale-after-reset).
    logic [19:0] way0_ppn, way1_ppn;
    logic        way0_v, way0_c, way0_r, way0_w, way0_x, way0_u, way0_g;
    logic        way1_v, way1_c, way1_r, way1_w, way1_x, way1_u, way1_g;

    assign way0_ppn = way0_pte_data[31:12];
    assign way0_v   = way0_v_vec[lookup_set];
    assign way0_c   = way0_pte_data[TLB_C];
    assign way0_r   = way0_pte_data[TLB_R];
    assign way0_w   = way0_pte_data[TLB_W];
    assign way0_x   = way0_pte_data[TLB_X];
    assign way0_u   = way0_pte_data[TLB_U];
    assign way0_g   = way0_pte_data[TLB_G];

    assign way1_ppn = way1_pte_data[31:12];
    assign way1_v   = way1_v_vec[lookup_set];
    assign way1_c   = way1_pte_data[TLB_C];
    assign way1_r   = way1_pte_data[TLB_R];
    assign way1_w   = way1_pte_data[TLB_W];
    assign way1_x   = way1_pte_data[TLB_X];
    assign way1_u   = way1_pte_data[TLB_U];
    assign way1_g   = way1_pte_data[TLB_G];

    // ── Match logic ────────────────────────────────────────
    // Hit = V && VPN match && (G || ASID match)
    logic way0_vpn_match, way0_asid_match, way0_match;
    logic way1_vpn_match, way1_asid_match, way1_match;

    assign way0_vpn_match  = (way0_entry_vpn == lookup_vpn);
    assign way0_asid_match = way0_g || (way0_entry_asid == i_asid);
    assign way0_match      = way0_v && way0_vpn_match && way0_asid_match;

    assign way1_vpn_match  = (way1_entry_vpn == lookup_vpn);
    assign way1_asid_match = way1_g || (way1_entry_asid == i_asid);
    assign way1_match      = way1_v && way1_vpn_match && way1_asid_match;

    // ── Way select mux + permission check ──────────────────
    // One-hot access types (ACC_READ/WRITE/EXEC) match the R/W/X flag
    // positions, so the permission check is a single bitwise AND.

    logic        matched;
    logic [19:0] matched_ppn;
    logic        matched_c, matched_u;
    logic [2:0]  matched_rwx;  // {X, W, R} from winning entry

    logic        perm_ok;

    always_comb begin
        // Defaults — no hit, no fault
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
            // Way select: way0 has priority if both match (shouldn't happen)
            if (way0_match) begin
                matched     = 1'b1;
                matched_ppn = way0_ppn;
                matched_c   = way0_c;
                matched_u   = way0_u;
                matched_rwx = {way0_x, way0_w, way0_r};
            end else if (way1_match) begin
                matched     = 1'b1;
                matched_ppn = way1_ppn;
                matched_c   = way1_c;
                matched_u   = way1_u;
                matched_rwx = {way1_x, way1_w, way1_r};
            end

            o_hit   = matched;
            o_paddr = {matched_ppn, i_vaddr[11:0]};
            o_cacheable = matched_c;

            // Permission check (only on hit)
            // R/W/X: one-hot access_type ANDed with entry's {X,W,R}
            // U: user mode must have U=1; supervisor bypasses
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
    // Sysreg readback (combinational — addresses i_idx_set)
    // ══════════════════════════════════════════════════════════
    // Sysreg mirror banks track the lookup banks in lockstep but
    // are addressed independently, so a sysreg read never contests
    // the lookup port and stays off the critical path.

    logic [31:0] sysreg_vpn_w0, sysreg_vpn_w1;
    logic [31:0] sysreg_pte_w0, sysreg_pte_w1;
    assign sysreg_vpn_w0 = way0_vpn_sysreg[i_idx_set];
    assign sysreg_vpn_w1 = way1_vpn_sysreg[i_idx_set];
    assign sysreg_pte_w0 = way0_pte_sysreg[i_idx_set];
    assign sysreg_pte_w1 = way1_pte_sysreg[i_idx_set];

    // V bit comes from the flop vector for a consistent post-reset
    // view — the DPRAM copy may be stale.  Bit 0 of the PTE word
    // is overridden with v_vec on readback.
    logic sysreg_v;
    assign sysreg_v = i_idx_way ? way1_v_vec[i_idx_set]
                                : way0_v_vec[i_idx_set];

    logic [31:0] sysreg_pte_raw;
    assign sysreg_pte_raw = i_idx_way ? sysreg_pte_w1 : sysreg_pte_w0;

    assign o_read_vpn = i_idx_way ? sysreg_vpn_w1 : sysreg_vpn_w0;
    assign o_read_pte = {sysreg_pte_raw[31:1], sysreg_v};

    // ══════════════════════════════════════════════════════════
    // Writes — update lookup mirror, sysreg mirror, and V vec
    // ══════════════════════════════════════════════════════════
    // Only the addressed bank is written.  V vec is the sole entity
    // touched by reset (DPRAM has no clear; software invalidates
    // explicitly before relying on TLB contents).

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            way0_v_vec <= 32'b0;
            way1_v_vec <= 32'b0;
        end else if (i_write_en) begin
            if (i_idx_way == 1'b0) begin
                way0_vpn_lookup[i_idx_set] <= i_write_vpn;
                way0_pte_lookup[i_idx_set] <= i_write_pte;
                way0_vpn_sysreg[i_idx_set] <= i_write_vpn;
                way0_pte_sysreg[i_idx_set] <= i_write_pte;
                way0_v_vec[i_idx_set]      <= i_write_pte[TLB_V];
            end else begin
                way1_vpn_lookup[i_idx_set] <= i_write_vpn;
                way1_pte_lookup[i_idx_set] <= i_write_pte;
                way1_vpn_sysreg[i_idx_set] <= i_write_vpn;
                way1_pte_sysreg[i_idx_set] <= i_write_pte;
                way1_v_vec[i_idx_set]      <= i_write_pte[TLB_V];
            end
        end
    end

endmodule

// verilator lint_on UNUSEDSIGNAL
