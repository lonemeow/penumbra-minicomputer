// penumbra3_mmu -- gen3 memory management unit (I + D translation).
//
// Wraps two penumbra3_translate paths -- an instruction copy and a data copy --
// over one shared install/refill write stream, so the duplicated sync-BRAM TLB
// storage stays hardware-coherent (every WRSYS install fans to both). A single
// dual-port tlb_pinned (common/) serves the always-resident mappings for both
// ports. The architectural contract -- the software-managed TLB, the sysreg
// device-0 interface, and commit-time fault registers -- is unchanged from
// earlier generations (see doc/system/mmu.md); only the MEM1-launch /
// MEM2-verdict pipelining is gen3.
//
// Per-port verdict (resolve cycle): bypass > pinned > main. Bypass (MMUCR.M=0,
// or the I-port's per-fetch force_bypass for vector reads) identity-maps to an
// uncacheable physical address with no checks. Otherwise a pinned hit overrides
// the main-TLB verdict (pinned-hit-wins). The pinned lookup runs on the live
// (launch-cycle) vaddr and its resolved verdict is registered to the resolve
// cycle, keeping the fully-associative compare off the MEM2 main-TLB cone; the
// bypass identity paddr rides the same launch->resolve register.
//
// Pinned-hit-wins lives here, not inside tlb_verdict: the shared tlb_pinned
// produces a resolved verdict (paddr / cacheable / fault), while tlb_verdict
// would want the raw matched PTE -- and tlb_pinned is frozen for gen1/gen2, so
// the override is composed at this wrapper instead. Each translate runs
// main-TLB-only (its pinned input tied off).
//
// Sysreg read-back of a main-TLB entry taps the D-copy's registered set: during
// an RDSYS to TLB_VPN/TLB_PTE the read index is steered to the addressed set
// (the D-side is idle -- an RDSYS is not a memory access), and the registered
// output lands one cycle later, on the RDSYS response cycle.

module penumbra3_mmu
    import penumbra_pkg::*;
#(
    parameter int SETS           = 32,
    parameter int WAYS           = 2,
    parameter int PINNED_ENTRIES = 8
) (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Instruction-side translate (launch / resolve) ────────────
    input  logic        i_i_lookup_en,
    input  logic [31:0] i_i_vaddr,
    input  logic        i_i_user_mode,
    input  logic        i_i_force_bypass,   // vector fetch: physical read
    input  logic        i_i_hold,           // freeze the launch->resolve register
    output logic [31:0] o_i_paddr,
    output logic        o_i_cacheable,
    output logic        o_i_fault,          // any translation fault (miss | prot)
    output logic [31:0] o_i_fault_status,

    // ── Data-side translate (launch / resolve) ───────────────────
    input  logic        i_d_lookup_en,
    input  logic [31:0] i_d_vaddr,
    input  logic [2:0]  i_d_access_type,
    input  logic        i_d_user_mode,
    input  logic        i_d_hold,
    output logic [31:0] o_d_paddr,
    output logic        o_d_cacheable,
    output logic        o_d_hit,
    output logic        o_d_miss_fault,
    output logic        o_d_prot_fault,

    // ── Commit-time fault latch (core drives at WB) ──────────────
    input  logic        i_fault_commit,
    input  logic [31:0] i_fault_vaddr,
    input  logic [31:0] i_fault_status,

    // ── Sysreg interface (WRSYS/RDSYS, device 0) ─────────────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    input  logic        i_sys_re,
    output logic [31:0] o_sys_rdata
);

    // Several leaf outputs are deliberately unconsumed (the D-side fault status
    // is composed downstream in MEM2; the I-copy read-back is unused -- the
    // D-copy serves sysreg read-back). The empty pin connections are intentional.
    /* verilator lint_off PINCONNECTEMPTY */

    localparam int SET_BITS = $clog2(SETS);

    // ── Control / fault / staging registers ──────────────────────
    logic [31:0] mmucr_q;        // [0]=M (enable), [15:8]=ASID
    logic [31:0] faddr_q, fstat_q;
    logic [31:0] vpn_stage_q;    // TLB_VPN holding register (committed on TLB_PTE write)
    logic [6:0]  idx_q;          // TLB_INDEX: [6]=pinned, [5]=way, [4:0]=set / [2:0]=pinned slot
    logic [3:0]  rd_reg_q;       // held read selector (RDSYS response is one cycle late)

    logic        mmu_m;
    logic [7:0]  mmu_asid;
    assign mmu_m    = mmucr_q[0];
    assign mmu_asid = mmucr_q[15:8];

    // ── Sysreg writes ────────────────────────────────────────────
    logic tlb_commit, commit_main, commit_pinned;
    assign tlb_commit   = i_sys_we & (i_sys_reg == SYSREG_MMU_TLB_PTE);
    assign commit_pinned = tlb_commit &  idx_q[6];
    assign commit_main   = tlb_commit & ~idx_q[6];

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            mmucr_q     <= 32'b0;       // M=0: flat/bypass at reset
            vpn_stage_q <= 32'b0;
            idx_q       <= 7'b0;
        end else if (i_sys_we) begin
            case (i_sys_reg)
                SYSREG_MMU_CR:      mmucr_q     <= i_sys_wdata;
                SYSREG_MMU_TLB_VPN: vpn_stage_q <= i_sys_wdata;
                SYSREG_MMU_TLB_IDX: idx_q       <= i_sys_wdata[6:0];
                default: ;             // TLB_PTE write is the commit, handled below
            endcase
        end
    end

    // Commit-time fault registers (latch the retiring fault only).
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            faddr_q <= 32'b0;
            fstat_q <= 32'b0;
        end else if (i_fault_commit) begin
            faddr_q <= i_fault_vaddr;
            fstat_q <= i_fault_status;
        end
    end

    // Held read selector: the RDSYS launches in MEM1 and the response is read in
    // MEM2, so the selector that drives the response mux is registered here.
    always_ff @(posedge i_clk) begin
        if (i_rst)          rd_reg_q <= 4'b0;
        else if (i_sys_re)  rd_reg_q <= i_sys_reg;
    end

    // ── Shared install / refill write fan-out (both main-TLB copies) ─
    logic                wr_en;
    logic [SET_BITS-1:0] wr_set;
    logic [$clog2(WAYS)-1:0] wr_way;
    logic                wr_valid;
    logic [31:0]         wr_vpn_word, wr_pte_word;
    assign wr_en       = commit_main;
    assign wr_set      = idx_q[SET_BITS-1:0];
    assign wr_way      = idx_q[5];
    assign wr_valid    = i_sys_wdata[TLB_V];
    assign wr_vpn_word = vpn_stage_q;
    assign wr_pte_word = i_sys_wdata;

    // ── Main-TLB read-back: steer the D-copy read index during RDSYS ─
    logic        readback_main;
    logic [31:0] d_vaddr_eff;
    assign readback_main = i_sys_re
                         & ((i_sys_reg == SYSREG_MMU_TLB_VPN) | (i_sys_reg == SYSREG_MMU_TLB_PTE))
                         & ~idx_q[6];
    // Place the addressed set in vaddr[16:12] so penumbra3_translate's index
    // derivation reads it; the D-side is idle during an RDSYS so this never
    // collides with a live data translation.
    assign d_vaddr_eff = readback_main ? {{(32-12-SET_BITS){1'b0}}, idx_q[SET_BITS-1:0], 12'b0}
                                       : i_d_vaddr;

    // ════════════════════════════════════════════════════════════
    // Pinned TLB (shared, dual lookup port) -- live at launch
    // ════════════════════════════════════════════════════════════
    logic [31:0] pin_d_paddr, pin_i_paddr;
    logic        pin_d_cacheable, pin_d_hit, pin_d_fault;
    logic        pin_i_cacheable, pin_i_hit, pin_i_fault;
    logic [31:0] pin_read_vpn, pin_read_pte;
    tlb_pinned #(
        .NUM_ENTRIES    (PINNED_ENTRIES),
        .DUAL_TRANSLATE (1'b1)
    ) u_pinned (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        // Port A = data side
        .i_vaddr         (i_d_vaddr),
        .i_access_type   (i_d_access_type),
        .i_user_mode     (i_d_user_mode),
        .i_asid          (mmu_asid),
        .i_lookup_en     (i_d_lookup_en),
        .o_paddr         (pin_d_paddr),
        .o_cacheable     (pin_d_cacheable),
        .o_hit           (pin_d_hit),
        .o_fault         (pin_d_fault),
        .o_fault_status  (/* MMU composes I-side status; D bits suffice */),
        // Port B = instruction side (execute access)
        .i_b_vaddr       (i_i_vaddr),
        .i_b_access_type (ACC_EXEC),
        .i_b_user_mode   (i_i_user_mode),
        .i_b_lookup_en   (i_i_lookup_en),
        .o_b_paddr       (pin_i_paddr),
        .o_b_cacheable   (pin_i_cacheable),
        .o_b_hit         (pin_i_hit),
        .o_b_fault       (pin_i_fault),
        .o_b_fault_status(),
        // Indexed sysreg access
        .i_idx           (idx_q[$clog2(PINNED_ENTRIES)-1:0]),
        .i_write_vpn     (vpn_stage_q),
        .i_write_pte     (i_sys_wdata),
        .i_write_en      (commit_pinned),
        .o_read_vpn      (pin_read_vpn),
        .o_read_pte      (pin_read_pte)
    );

    // ════════════════════════════════════════════════════════════
    // Main-TLB translate paths (I-copy + D-copy), main-TLB-only
    // ════════════════════════════════════════════════════════════
    logic [31:0]         d_main_paddr, i_main_paddr;
    logic                d_main_cacheable, d_main_hit, d_main_prot;
    logic                i_main_cacheable, i_main_hit, i_main_prot;
    logic [WAYS-1:0][31:0] d_rb_vpn, d_rb_pte;

    penumbra3_translate #(.SETS(SETS), .WAYS(WAYS)) u_d (
        .i_clk             (i_clk),
        .i_rst             (i_rst),
        .i_lookup_en       (i_d_lookup_en),
        .i_vaddr           (d_vaddr_eff),
        .i_access_type     (i_d_access_type),
        .i_user_mode       (i_d_user_mode),
        .i_asid            (mmu_asid),
        .i_hold            (i_d_hold),
        .i_pinned_hit      (1'b0),              // pinned override done in this wrapper
        .i_pinned_pte_word (32'b0),
        .o_paddr           (d_main_paddr),
        .o_cacheable       (d_main_cacheable),
        .o_hit             (d_main_hit),
        .o_miss_fault      (/* recomposed below with pinned/bypass */),
        .o_prot_fault      (d_main_prot),
        .o_rd_vpn_word     (d_rb_vpn),
        .o_rd_pte_word     (d_rb_pte),
        .i_wr_en           (wr_en),
        .i_wr_set          (wr_set),
        .i_wr_way          (wr_way),
        .i_wr_valid        (wr_valid),
        .i_wr_vpn_word     (wr_vpn_word),
        .i_wr_pte_word     (wr_pte_word)
    );

    penumbra3_translate #(.SETS(SETS), .WAYS(WAYS)) u_i (
        .i_clk             (i_clk),
        .i_rst             (i_rst),
        .i_lookup_en       (i_i_lookup_en),
        .i_vaddr           (i_i_vaddr),
        .i_access_type     (ACC_EXEC),
        .i_user_mode       (i_i_user_mode),
        .i_asid            (mmu_asid),
        .i_hold            (i_i_hold),
        .i_pinned_hit      (1'b0),
        .i_pinned_pte_word (32'b0),
        .o_paddr           (i_main_paddr),
        .o_cacheable       (i_main_cacheable),
        .o_hit             (i_main_hit),
        .o_miss_fault      (/* recomposed below */),
        .o_prot_fault      (i_main_prot),
        .o_rd_vpn_word     (/* read-back uses the D-copy */),
        .o_rd_pte_word     (),
        .i_wr_en           (wr_en),            // shared fan-out: same install to both copies
        .i_wr_set          (wr_set),
        .i_wr_way          (wr_way),
        .i_wr_valid        (wr_valid),
        .i_wr_vpn_word     (wr_vpn_word),
        .i_wr_pte_word     (wr_pte_word)
    );

    // ── Launch->resolve registers: bypass + pinned verdict ───────
    // Aligns the pinned (live-lookup) verdict and the bypass identity paddr to
    // the resolve cycle, where the main-TLB BRAM output lands.
    logic        d_bypass_q, d_lookup_q;
    logic [31:0] d_vaddr_q;
    logic        d_phit_q, d_pcache_q, d_pprot_q;
    logic [31:0] d_ppaddr_q;
    logic        i_bypass_q, i_lookup_q, i_user_q;
    logic [31:0] i_vaddr_q;
    logic        i_phit_q, i_pcache_q, i_pprot_q;
    logic [31:0] i_ppaddr_q;

    logic d_bypass_d, i_bypass_d;
    assign d_bypass_d = ~mmu_m;
    assign i_bypass_d = ~mmu_m | i_i_force_bypass;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            {d_bypass_q, d_lookup_q, i_bypass_q, i_lookup_q} <= 4'b0;
            d_phit_q <= 1'b0; i_phit_q <= 1'b0;
        end else begin
            if (!i_d_hold) begin
                d_bypass_q <= d_bypass_d;
                d_lookup_q <= i_d_lookup_en;
                d_vaddr_q  <= i_d_vaddr;
                d_phit_q   <= pin_d_hit;
                d_ppaddr_q <= pin_d_paddr;
                d_pcache_q <= pin_d_cacheable;
                d_pprot_q  <= pin_d_fault;
            end
            if (!i_i_hold) begin
                i_bypass_q <= i_bypass_d;
                i_lookup_q <= i_i_lookup_en;
                i_user_q   <= i_i_user_mode;
                i_vaddr_q  <= i_i_vaddr;
                i_phit_q   <= pin_i_hit;
                i_ppaddr_q <= pin_i_paddr;
                i_pcache_q <= pin_i_cacheable;
                i_pprot_q  <= pin_i_fault;
            end
        end
    end

    // ── D-side resolved verdict (bypass > pinned > main) ─────────
    assign o_d_hit        = d_lookup_q & (d_bypass_q | d_phit_q | d_main_hit);
    assign o_d_miss_fault = d_lookup_q & ~d_bypass_q & ~d_phit_q & ~d_main_hit;
    assign o_d_prot_fault = ~d_bypass_q & (d_phit_q ? d_pprot_q : d_main_prot);
    assign o_d_paddr      = d_bypass_q ? d_vaddr_q
                          : d_phit_q   ? d_ppaddr_q
                          :              d_main_paddr;
    assign o_d_cacheable  = d_bypass_q ? 1'b0
                          : d_phit_q   ? d_pcache_q
                          :              d_main_cacheable;

    // ── I-side resolved verdict + composed fault status ──────────
    logic i_miss, i_prot;
    assign i_miss = i_lookup_q & ~i_bypass_q & ~i_phit_q & ~i_main_hit;
    assign i_prot = ~i_bypass_q & (i_phit_q ? i_pprot_q : i_main_prot);
    assign o_i_paddr     = i_bypass_q ? i_vaddr_q
                         : i_phit_q   ? i_ppaddr_q
                         :              i_main_paddr;
    assign o_i_cacheable = i_bypass_q ? 1'b0
                         : i_phit_q   ? i_pcache_q
                         :              i_main_cacheable;
    assign o_i_fault     = i_miss | i_prot;
    assign o_i_fault_status = i_miss ? compose_fault_status(i_user_q, ACC_EXEC, FAULT_TLB_MISS)
                            : i_prot ? compose_fault_status(i_user_q, ACC_EXEC, FAULT_PROT)
                            :          32'b0;

    // ── Sysreg read response (one cycle after the RDSYS launch) ──
    logic [31:0] tlb_rb_vpn, tlb_rb_pte;
    assign tlb_rb_vpn = idx_q[6] ? pin_read_vpn : d_rb_vpn[idx_q[5]];
    assign tlb_rb_pte = idx_q[6] ? pin_read_pte : d_rb_pte[idx_q[5]];
    always_comb begin
        case (rd_reg_q)
            SYSREG_MMU_CR:      o_sys_rdata = mmucr_q;
            SYSREG_MMU_FADDR:   o_sys_rdata = faddr_q;
            SYSREG_MMU_FSTAT:   o_sys_rdata = fstat_q;
            SYSREG_MMU_TLB_VPN: o_sys_rdata = tlb_rb_vpn;
            SYSREG_MMU_TLB_PTE: o_sys_rdata = tlb_rb_pte;
            SYSREG_MMU_TLB_IDX: o_sys_rdata = {25'b0, idx_q};
            default:            o_sys_rdata = 32'b0;
        endcase
    end

    // ── Assertions (sim-only; Verilator --assert) ────────────────
    // bypass / pinned / main are a strict priority -- a protection fault is only
    // raised on a real (non-bypass) access.
    always_comb
        assert (!o_d_prot_fault || (!d_bypass_q && d_lookup_q))
            else $error("penumbra3_mmu: D protection fault under bypass / no lookup");

    // A miss and a prot fault are mutually exclusive on the same port.
    always_comb
        assert (!(o_d_miss_fault && o_d_prot_fault))
            else $error("penumbra3_mmu: D miss and prot asserted together");

    /* verilator lint_on PINCONNECTEMPTY */
endmodule
