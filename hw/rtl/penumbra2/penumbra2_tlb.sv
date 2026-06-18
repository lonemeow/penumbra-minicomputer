// Penumbra gen2 main TLB — LUTRAM-backed, async read, dual translate port
//
// 64-entry 2-way set-associative translation cache for the pipelined core.
// Storage is distributed RAM (LUTRAM) with a *combinational* read, so a lookup
// resolves the same cycle its query is presented: the translate verdict is
// produced in the access launch cycle and registered downstream (penumbra2_mmu),
// rather than waiting a cycle for a synchronous (BRAM) read to land before the
// match cone can run. This reclaims the launch cycle — otherwise idle — for the
// translate cone, balancing it against the tag-compare cycle it feeds. The
// storage recipe (banks split by way, replicated per read port, V bits in a
// flop vector, lockstep writes) follows the gen1 main TLB (tlb.sv); the match
// cone is the shared penumbra2_tlb_perm, one instance per port.
//
// Two concurrent translations (port A I-side, port B D-side) each need an
// independent read address, and a 1W/1R distributed-RAM bank serves only one
// read address — so storage is replicated per port. The single write port
// updates both copies in lockstep. Port B's read address muxes in the sysreg
// index for indexed readback (port B's three duties — D-translate, readback,
// write — are mutually exclusive in time; asserted below).
//
// V bits live in a flop vector with synchronous reset: distributed RAM has no
// async clear, so V==0 masks stale RAM after reset until software writes a
// valid PTE.
//
// Entry word layout (shared MMU convention):
//   VPN word: {4'b0, VPN[19:0], ASID[7:0]}
//   PTE word: {PPN[19:0], SW[3:0], flags[7:0]}, flags = G U X W R C - V

// verilator lint_off UNUSEDSIGNAL

// keep_hierarchy: hold this boundary through synth_ecp5 so the translate
// verdict cone reads with real signal names in timing reports and places as
// a unit. Paired across the TLB cone modules (penumbra2_mmu / penumbra2_tlb_unit /
// penumbra2_tlb / penumbra2_tlb_perm).
(* keep_hierarchy = "yes" *)
module penumbra2_tlb
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic [7:0]  i_asid,            // current ASID (from MMUCR)

    // ── Port A: I-side translate (combinational: query and verdict same cycle) ──
    input  logic [31:0] i_a_vaddr,
    input  logic [2:0]  i_a_access_type,
    input  logic        i_a_user_mode,
    input  logic        i_a_lookup_en,
    output logic [31:0] o_a_paddr,
    output logic        o_a_cacheable,
    output logic        o_a_hit,
    output logic        o_a_fault,

    // ── Port B: D-side translate (same combinational lookup) ──
    input  logic [31:0] i_b_vaddr,
    input  logic [2:0]  i_b_access_type,
    input  logic        i_b_user_mode,
    input  logic        i_b_lookup_en,
    output logic [31:0] o_b_paddr,
    output logic        o_b_cacheable,
    output logic        o_b_hit,
    output logic        o_b_fault,

    // ── Indexed write / readback (sysreg, via port B's read address) ──
    input  logic [4:0]  i_idx_set,
    input  logic        i_idx_way,
    input  logic [31:0] i_write_vpn,
    input  logic [31:0] i_write_pte,
    input  logic        i_write_en,        // commit an entry this cycle
    input  logic        i_read_en,         // readback request (data is combinational)
    output logic [31:0] o_read_vpn,        // readback (combinational off the held index)
    output logic [31:0] o_read_pte
);

    localparam int SETS = 32;

    // ══════════════════════════════════════════════════════════
    // Storage — one 64-bit {pte, vpn} word per (set, way), distributed RAM,
    // replicated per read port (A, B). Async read; one synchronous write port
    // updates both copies in lockstep. V bits are a flop vector (no async
    // clear; V==0 masks stale RAM after reset).
    // ══════════════════════════════════════════════════════════
    (* ram_style = "distributed" *) logic [63:0] way0_a_mem [0:SETS-1];
    (* ram_style = "distributed" *) logic [63:0] way1_a_mem [0:SETS-1];
    (* ram_style = "distributed" *) logic [63:0] way0_b_mem [0:SETS-1];
    (* ram_style = "distributed" *) logic [63:0] way1_b_mem [0:SETS-1];
    logic [SETS-1:0] way0_v_vec, way1_v_vec;

    // ── Port A read (combinational, at the I-side EA set) ──────
    logic [4:0]  a_set;
    logic [63:0] way0_a, way1_a;
    assign a_set  = i_a_vaddr[16:12];
    assign way0_a = way0_a_mem[a_set];
    assign way1_a = way1_a_mem[a_set];

    penumbra2_tlb_perm u_perm_a (
        .i_lookup_en   (i_a_lookup_en),
        .i_vpn         (i_a_vaddr[31:12]),
        .i_asid        (i_asid),
        .i_access_type (i_a_access_type),
        .i_user_mode   (i_a_user_mode),
        .i_page_off    (i_a_vaddr[11:0]),
        .i_w0_vpn_word (way0_a[31:0]),
        .i_w0_pte_word (way0_a[63:32]),
        .i_w0_v        (way0_v_vec[a_set]),
        .i_w1_vpn_word (way1_a[31:0]),
        .i_w1_pte_word (way1_a[63:32]),
        .i_w1_v        (way1_v_vec[a_set]),
        .o_paddr       (o_a_paddr),
        .o_cacheable   (o_a_cacheable),
        .o_hit         (o_a_hit),
        .o_fault       (o_a_fault)
    );

    // ── Port B read (combinational; D-translate set, or sysreg index for a
    //    readback — the two never co-occur, so the read address is muxed) ──
    logic [4:0]  b_rd_set;
    logic [63:0] way0_b, way1_b;
    assign b_rd_set = i_b_lookup_en ? i_b_vaddr[16:12] : i_idx_set;
    assign way0_b   = way0_b_mem[b_rd_set];
    assign way1_b   = way1_b_mem[b_rd_set];

    penumbra2_tlb_perm u_perm_b (
        .i_lookup_en   (i_b_lookup_en),
        .i_vpn         (i_b_vaddr[31:12]),
        .i_asid        (i_asid),
        .i_access_type (i_b_access_type),
        .i_user_mode   (i_b_user_mode),
        .i_page_off    (i_b_vaddr[11:0]),
        .i_w0_vpn_word (way0_b[31:0]),
        .i_w0_pte_word (way0_b[63:32]),
        .i_w0_v        (way0_v_vec[b_rd_set]),
        .i_w1_vpn_word (way1_b[31:0]),
        .i_w1_pte_word (way1_b[63:32]),
        .i_w1_v        (way1_v_vec[b_rd_set]),
        .o_paddr       (o_b_paddr),
        .o_cacheable   (o_b_cacheable),
        .o_hit         (o_b_hit),
        .o_fault       (o_b_fault)
    );

    // ── Readback (combinational): the port-B copy at the sysreg index, V
    //    overridden from the flop vector (the RAM copy is stale after reset).
    //    Indexed reads hold their inputs across the RDSYS, and the consuming
    //    core registers the response — so a combinational readback meets the
    //    2-cycle RDSYS timing without an internal readback register.
    logic [63:0] rb_word;
    assign rb_word    = i_idx_way ? way1_b : way0_b;   // b_rd_set == i_idx_set when ~i_b_lookup_en
    assign o_read_vpn = rb_word[31:0];
    assign o_read_pte = {rb_word[63:33],
                         (i_idx_way ? way1_v_vec[i_idx_set] : way0_v_vec[i_idx_set])};

    // ── Writes (both port copies in lockstep) + V vector reset ──
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            way0_v_vec <= '0;
            way1_v_vec <= '0;
        end else if (i_write_en) begin
            if (i_idx_way == 1'b0) begin
                way0_a_mem[i_idx_set] <= {i_write_pte, i_write_vpn};
                way0_b_mem[i_idx_set] <= {i_write_pte, i_write_vpn};
                way0_v_vec[i_idx_set] <= i_write_pte[TLB_V];
            end else begin
                way1_a_mem[i_idx_set] <= {i_write_pte, i_write_vpn};
                way1_b_mem[i_idx_set] <= {i_write_pte, i_write_vpn};
                way1_v_vec[i_idx_set] <= i_write_pte[TLB_V];
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════
    // Port B's three duties must not collide in a single cycle — the read
    // address mux and the single write port assume mutual exclusion.
    assert property (@(posedge i_clk) disable iff (i_rst)
        $countones({i_write_en, i_b_lookup_en, i_read_en}) <= 1)
        else $error("penumbra2_tlb: port-B contention (write/D-translate/readback overlap)");

    // Reset must establish the all-invalid invariant: the V flop vector is the
    // only thing masking stale RAM after reset, so if reset failed to clear it,
    // stale entries would read as live hits.
    assert property (@(posedge i_clk)
        i_rst |=> (way0_v_vec == '0 && way1_v_vec == '0))
        else $error("penumbra2_tlb: reset did not clear the V vectors");

endmodule

// verilator lint_on UNUSEDSIGNAL
