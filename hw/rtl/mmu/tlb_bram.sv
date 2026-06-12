// Penumbra gen2 main TLB — BRAM-backed, registered, dual translation port
//
// 64-entry 2-way set-associative translation cache for the pipelined core.
// Storage lives in synchronous (BRAM/EBR) RAM, so a lookup is a registered
// read: drive the query at cycle T, the verdict is valid at T+1 — the same
// contract the BRAM L1 cache uses, so the TLB result lands in the tag-compare
// stage (IF2 for fetch, the MEM data-ready cycle for data) with no extra
// pipeline latency. The verdict then *holds* until the port's next operation
// launches (capture on the strobe, hold otherwise — the registered-read
// contract every RAM-shaped module here follows): a consumer stalled at its
// tag-compare cycle reads the same verdict on whichever cycle it advances.
//
// Two concurrent translations come from EBR's two ports, one copy of storage:
//   Port A — I-side translate (read-only, every fetch).
//   Port B — D-side translate, sysreg readback, and sysreg writes. These are
//            mutually exclusive in time (a load and an RDSYS are different MEM
//            instructions; a WRSYS fill is a drain-commit with the pipe empty),
//            so one R/W port serves all three.
//
// The 2-way match + permission verdict is the shared tlb_perm cone, one
// instance per translation port. V bits live in a flop vector (BRAM has no
// async clear); they are sampled alongside the address drive and registered to
// align with the BRAM data a cycle later.
//
// Entry word layout (shared MMU convention):
//   VPN word: {4'b0, VPN[19:0], ASID[7:0]}
//   PTE word: {PPN[19:0], SW[3:0], flags[7:0]}, flags = G U X W R C - V
//
// The indexed write / readback port is the decoded form of the sysreg
// interface; the wrapper (tlb_unit) owns TLB_INDEX/TLB_VPN staging and the
// pinned-vs-main routing.

// verilator lint_off UNUSEDSIGNAL

module tlb_bram
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic [7:0]  i_asid,            // current ASID (from MMUCR)

    // ── Port A: I-side translate (drive at T, verdict at T+1) ──
    input  logic [31:0] i_a_vaddr,
    input  logic [2:0]  i_a_access_type,
    input  logic        i_a_user_mode,
    input  logic        i_a_lookup_en,
    output logic [31:0] o_a_paddr,
    output logic        o_a_cacheable,
    output logic        o_a_hit,
    output logic        o_a_fault,

    // ── Port B: D-side translate (same registered contract) ──
    input  logic [31:0] i_b_vaddr,
    input  logic [2:0]  i_b_access_type,
    input  logic        i_b_user_mode,
    input  logic        i_b_lookup_en,
    output logic [31:0] o_b_paddr,
    output logic        o_b_cacheable,
    output logic        o_b_hit,
    output logic        o_b_fault,

    // ── Indexed write / readback (sysreg, via port B) ──
    input  logic [4:0]  i_idx_set,
    input  logic        i_idx_way,
    input  logic [31:0] i_write_vpn,
    input  logic [31:0] i_write_pte,
    input  logic        i_write_en,        // commit an entry this cycle
    input  logic        i_read_en,         // launch a readback this cycle
    output logic [31:0] o_read_vpn,        // valid the cycle after i_read_en
    output logic [31:0] o_read_pte
);

    localparam int SETS = 32;

    // ══════════════════════════════════════════════════════════
    // Storage — one 64-bit {pte, vpn} word per (set, way).
    // way{0,1}_mem are dual-port: port A read, port B read/write.
    // V bits are a flop vector (no BRAM async clear; V==0 masks stale
    // RAM after reset until software writes a valid PTE).
    // ══════════════════════════════════════════════════════════
    logic [63:0] way0_mem [0:SETS-1];
    logic [63:0] way1_mem [0:SETS-1];
    logic [SETS-1:0] way0_v_vec, way1_v_vec;

    // ── Port A address + query capture (cycle T) ───────────────
    logic [4:0] a_set;
    assign a_set = i_a_vaddr[16:12];

    logic [63:0] way0_a, way1_a;       // registered RAM read (cycle T+1)
    logic [19:0] a_vpn_q;
    logic [11:0] a_page_q;
    logic [7:0]  a_asid_q;
    logic [2:0]  a_acc_q;
    logic        a_usr_q, a_le_q;
    logic        a_v0_q, a_v1_q;

    // The query capture shares the RAM read's clock-enable so the verdict
    // holds between lookups; a_le_q is sticky once the first verdict exists
    // (it only masks post-reset garbage, port A has no other duty).
    always_ff @(posedge i_clk) begin
        if (i_a_lookup_en) begin
            way0_a  <= way0_mem[a_set];
            way1_a  <= way1_mem[a_set];
            a_v0_q  <= way0_v_vec[a_set];
            a_v1_q  <= way1_v_vec[a_set];
            a_vpn_q  <= i_a_vaddr[31:12];
            a_page_q <= i_a_vaddr[11:0];
            a_asid_q <= i_asid;
            a_acc_q  <= i_a_access_type;
            a_usr_q  <= i_a_user_mode;
        end
        if (i_rst)
            a_le_q <= 1'b0;
        else if (i_a_lookup_en)
            a_le_q <= 1'b1;
    end

    tlb_perm u_perm_a (
        .i_lookup_en   (a_le_q),
        .i_vpn         (a_vpn_q),
        .i_asid        (a_asid_q),
        .i_access_type (a_acc_q),
        .i_user_mode   (a_usr_q),
        .i_page_off    (a_page_q),
        .i_w0_vpn_word (way0_a[31:0]),
        .i_w0_pte_word (way0_a[63:32]),
        .i_w0_v        (a_v0_q),
        .i_w1_vpn_word (way1_a[31:0]),
        .i_w1_pte_word (way1_a[63:32]),
        .i_w1_v        (a_v1_q),
        .o_paddr       (o_a_paddr),
        .o_cacheable   (o_a_cacheable),
        .o_hit         (o_a_hit),
        .o_fault       (o_a_fault)
    );

    // ── Port B address mux + query capture (cycle T) ───────────
    // A D-translate reads at the EA-derived set; a readback reads at the
    // sysreg index. They never co-occur, so the read address is muxed.
    logic [4:0] b_rd_set;
    logic       b_ren;
    assign b_rd_set = i_b_lookup_en ? i_b_vaddr[16:12] : i_idx_set;
    assign b_ren    = i_b_lookup_en | i_read_en;

    logic [63:0] way0_b, way1_b;       // registered RAM read (cycle T+1)
    logic [19:0] b_vpn_q;
    logic [11:0] b_page_q;
    logic [7:0]  b_asid_q;
    logic [2:0]  b_acc_q;
    logic        b_usr_q, b_le_q;
    logic        b_v0_q, b_v1_q;
    // Readback capture
    logic        b_read_q, b_idx_way_q, b_rb_v_q;

    // Same hold rule as port A, per duty: the translate-query capture gates
    // on the lookup, the readback capture on the readback launch, and the
    // shared way/V regs on either (b_ren). b_le_q updates on any port-B
    // operation — a translate verdict holds across idle cycles but is
    // superseded by a readback, which reloads the shared way regs (the two
    // never overlap within one in-flight MEM instruction).
    always_ff @(posedge i_clk) begin
        if (b_ren) begin
            way0_b <= way0_mem[b_rd_set];
            way1_b <= way1_mem[b_rd_set];
            b_v0_q <= way0_v_vec[b_rd_set];
            b_v1_q <= way1_v_vec[b_rd_set];
        end
        if (i_b_lookup_en) begin
            b_vpn_q  <= i_b_vaddr[31:12];
            b_page_q <= i_b_vaddr[11:0];
            b_asid_q <= i_asid;
            b_acc_q  <= i_b_access_type;
            b_usr_q  <= i_b_user_mode;
        end
        if (i_rst)
            b_le_q <= 1'b0;
        else if (b_ren)
            b_le_q <= i_b_lookup_en;
        b_read_q <= i_read_en;
        if (i_read_en) begin
            b_idx_way_q <= i_idx_way;
            b_rb_v_q    <= i_idx_way ? way1_v_vec[i_idx_set] : way0_v_vec[i_idx_set];
        end
    end

    tlb_perm u_perm_b (
        .i_lookup_en   (b_le_q),
        .i_vpn         (b_vpn_q),
        .i_asid        (b_asid_q),
        .i_access_type (b_acc_q),
        .i_user_mode   (b_usr_q),
        .i_page_off    (b_page_q),
        .i_w0_vpn_word (way0_b[31:0]),
        .i_w0_pte_word (way0_b[63:32]),
        .i_w0_v        (b_v0_q),
        .i_w1_vpn_word (way1_b[31:0]),
        .i_w1_pte_word (way1_b[63:32]),
        .i_w1_v        (b_v1_q),
        .o_paddr       (o_b_paddr),
        .o_cacheable   (o_b_cacheable),
        .o_hit         (o_b_hit),
        .o_fault       (o_b_fault)
    );

    // ── Readback select (cycle T+1) — V overridden from the flop vec ──
    logic [63:0] rb_word;
    assign rb_word    = b_idx_way_q ? way1_b : way0_b;
    assign o_read_vpn = rb_word[31:0];
    assign o_read_pte = {rb_word[63:33], b_rb_v_q};

    // ── Writes (port B) + V vector reset ───────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            way0_v_vec <= '0;
            way1_v_vec <= '0;
        end else if (i_write_en) begin
            if (i_idx_way == 1'b0) begin
                way0_mem[i_idx_set]    <= {i_write_pte, i_write_vpn};
                way0_v_vec[i_idx_set]  <= i_write_pte[TLB_V];
            end else begin
                way1_mem[i_idx_set]    <= {i_write_pte, i_write_vpn};
                way1_v_vec[i_idx_set]  <= i_write_pte[TLB_V];
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
        else $error("tlb_bram: port-B contention (write/D-translate/readback overlap)");

    // Reset must establish the all-invalid invariant. The BRAM has no async
    // clear, so the V flop vector is the only thing masking stale entry data
    // after reset — if reset failed to clear it, stale RAM would read as live
    // hits. (Not disabled iff i_rst: this checks the reset behaviour itself.)
    assert property (@(posedge i_clk)
        i_rst |=> (way0_v_vec == '0 && way1_v_vec == '0))
        else $error("tlb_bram: reset did not clear the V vectors");

endmodule

// verilator lint_on UNUSEDSIGNAL
