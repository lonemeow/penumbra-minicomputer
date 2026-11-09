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
// Per-port verdict (resolve cycle): bypass > pinned > main. Bypass (MMUCR.M=0)
// identity-maps to an uncacheable physical address with no checks. Otherwise
// a pinned hit overrides the main-TLB verdict (pinned-hit-wins). The pinned
// lookup runs on the live (launch-cycle) vaddr and its resolved verdict is
// registered to the resolve cycle, keeping the fully-associative compare off
// the MEM2 main-TLB cone; the bypass identity paddr rides the same
// launch->resolve register.
//
// Pinned-hit-wins lives here, not inside tlb_verdict: the shared tlb_pinned
// produces a resolved verdict (paddr / cacheable / fault), while tlb_verdict
// would want the raw matched PTE -- and the shared cell keeps the one
// interface every core uses, so the override is composed at this wrapper
// instead. Each translate runs main-TLB-only (its pinned input tied off).
//
// Sysreg read-back of a main-TLB entry taps the D-copy's registered set: during
// an RDSYS to TLB_VPN/TLB_PTE the read index is steered to the addressed set
// (the D-side is idle -- an RDSYS is not a memory access), and the registered
// output lands one cycle later, on the RDSYS response cycle.
//
// The sysreg interface is MMU_CONFIG version 2 (doc/system/mmu.md): TLB_INDEX
// numbers a main slot way * SETS + set and selects the pinned TLB with bit 31,
// a slot past either TLB's end selecting nothing; the three invalidation
// registers start penumbra3_tlb_inval's sweep. While it runs o_sys_busy
// holds fetch, so the WRSYS that started it blocks until the next
// instruction's fetch sees the result, and the sweep has the D-copy's read
// port to itself. Out of reset the TLB holds whatever its block RAM came up
// with: the architecture leaves it undefined, and software invalidates it
// before it sets M.

module penumbra3_mmu
    import penumbra_pkg::*;
#(
    parameter int SETS           = 256,
    parameter int WAYS           = 2,
    parameter int PINNED_ENTRIES = 8
) (
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Instruction-side translate (launch / resolve) ────────────
    input  logic        i_i_lookup_en,
    input  logic [31:0] i_i_vaddr,
    input  logic        i_i_user_mode,
    input  logic        i_i_hold,           // freeze the launch->resolve register
    output logic [31:0] o_i_paddr,
    output logic        o_i_cacheable,
    output logic        o_i_fault,          // any translation fault (miss | prot)
    output logic [31:0] o_i_fault_status,
    output logic [WAYS:0][19:0] o_i_frame,  // frame candidates (see the resolve below)
    output logic [WAYS:0]       o_i_frame_sel,

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
    output logic [WAYS:0][19:0] o_d_frame,
    output logic [WAYS:0]       o_d_frame_sel,

    // ── Commit-time fault latch (core drives at WB) ──────────────
    input  logic        i_fault_commit,
    input  logic [31:0] i_fault_vaddr,
    input  logic [31:0] i_fault_status,

    // ── Sysreg interface (WRSYS/RDSYS, device 0) ─────────────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    input  logic        i_sys_re,
    output logic [31:0] o_sys_rdata,
    output logic        o_sys_busy          // registered; fetch waits on it
);

    // Several leaf outputs are deliberately unconsumed (the D-side fault status
    // is composed downstream in MEM2; the I-copy read-back is unused -- the
    // D-copy serves sysreg read-back). The empty pin connections are intentional.
    /* verilator lint_off PINCONNECTEMPTY */

    localparam int SET_BITS = $clog2(SETS);
    localparam int WAY_BITS = $clog2(WAYS);

    // The geometry MMU_CONFIG version 2 can report, in the shape this MMU
    // decodes: power-of-two ways, so a main slot's way is a bit field.
    build_check #(
        .COND (SETS >= 2 && SETS <= (1 << 20) && (SETS & (SETS - 1)) == 0),
        .MSG  ("penumbra3_mmu: SETS must be a power of two from 2 to 2^20")
    ) u_check_sets ();
    build_check #(
        .COND (WAYS >= 2 && WAYS <= 16 && (WAYS & (WAYS - 1)) == 0),
        .MSG  ("penumbra3_mmu: WAYS must be a power of two from 2 to 16")
    ) u_check_ways ();
    build_check #(
        .COND (PINNED_ENTRIES >= 1 && PINNED_ENTRIES <= 63),
        .MSG  ("penumbra3_mmu: PINNED_ENTRIES must be 1 to 63")
    ) u_check_pinned ();

    // MMU_CONFIG: version 2, this geometry, 4 KB pages, 8-bit ASIDs.
    localparam int ASID_BITS = 8;
    localparam int PAGE_LOG2 = 12;
    typedef logic [3:0] cfg_ver_t;
    typedef logic [5:0] cfg_pinned_t;
    typedef logic [4:0] cfg_field_t;
    localparam logic [31:0] MMU_CONFIG_VALUE = {2'b00,
                                                cfg_field_t'(ASID_BITS),
                                                cfg_field_t'(PAGE_LOG2),
                                                cfg_field_t'(WAYS),
                                                cfg_field_t'(SET_BITS),
                                                cfg_pinned_t'(PINNED_ENTRIES),
                                                cfg_ver_t'(2)};

    // ── Control / fault / staging registers ──────────────────────
    logic [31:0] mmucr_q;        // [0]=M (enable), [15:8]=ASID
    logic [31:0] faddr_q, fstat_q;
    logic [31:0] vpn_stage_q;    // TLB_VPN holding register (committed on TLB_PTE write)
    logic [31:0] idx_q;          // TLB_INDEX
    logic [3:0]  rd_reg_q;       // held read selector (RDSYS response is one cycle late)

    // TLB_INDEX: bit 31 selects the pinned TLB, bits 30:0 number a slot.
    typedef logic [WAY_BITS-1:0] way_t;
    typedef logic [30:0]         slot_t;
    localparam slot_t PINNED_SLOTS = slot_t'(PINNED_ENTRIES);
    logic                idx_pinned;
    logic [30:0]         idx_slot;
    logic                idx_main_exists, idx_pinned_exists;
    logic [SET_BITS-1:0] idx_set;
    way_t                idx_way;
    assign idx_pinned        = idx_q[31];
    assign idx_slot          = idx_q[30:0];
    assign idx_main_exists   = (idx_slot >> (SET_BITS + WAY_BITS)) == '0;
    assign idx_pinned_exists = idx_slot < PINNED_SLOTS;
    assign idx_set           = idx_q[SET_BITS-1:0];
    assign idx_way           = idx_q[SET_BITS +: WAY_BITS];

    logic        mmu_m;
    logic [7:0]  mmu_asid;
    assign mmu_m    = mmucr_q[0];
    assign mmu_asid = mmucr_q[15:8];

    // ── Sysreg writes ────────────────────────────────────────────
    // A TLB_PTE write to a slot that does not exist changes nothing.
    logic tlb_commit, commit_main, commit_pinned;
    assign tlb_commit    = i_sys_we & (i_sys_reg == SYSREG_MMU_TLB_PTE);
    assign commit_pinned = tlb_commit &  idx_pinned & idx_pinned_exists;
    assign commit_main   = tlb_commit & ~idx_pinned & idx_main_exists;

    logic inval_va, inval_asid, inval_all;
    assign inval_va   = i_sys_we & (i_sys_reg == SYSREG_MMU_TLB_INVAL_VA);
    assign inval_asid = i_sys_we & (i_sys_reg == SYSREG_MMU_TLB_INVAL_ASID);
    assign inval_all  = i_sys_we & (i_sys_reg == SYSREG_MMU_TLB_INVAL_ALL);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            mmucr_q     <= 32'b0;       // M=0: flat/bypass at reset
            vpn_stage_q <= 32'b0;
            idx_q       <= 32'b0;
        end else if (i_sys_we) begin
            case (i_sys_reg)
                SYSREG_MMU_CR:      mmucr_q     <= i_sys_wdata;
                SYSREG_MMU_TLB_VPN: vpn_stage_q <= i_sys_wdata;
                SYSREG_MMU_TLB_IDX: idx_q       <= i_sys_wdata;
                // TLB_PTE commits below; the invalidations start the sweep
                default: ;
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

    // ── The invalidation sweep ───────────────────────────────────
    logic                  sweep_busy, sweep_rd_en;
    logic [SET_BITS-1:0]   sweep_rd_set, sweep_wr_set;
    logic [WAYS-1:0]       sweep_wr_pte_en;
    logic [WAYS-1:0][31:0] sweep_wr_pte_word;
    logic [WAYS-1:0][31:0] d_rb_vpn, d_rb_pte;

    penumbra3_tlb_inval #(
        .SETS (SETS),
        .WAYS (WAYS)
    ) u_inval (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_inval_va    (inval_va),
        .i_inval_asid  (inval_asid),
        .i_inval_all   (inval_all),
        .i_target      (i_sys_wdata[27:0]),
        .o_busy        (sweep_busy),
        .o_rd_en       (sweep_rd_en),
        .o_rd_set      (sweep_rd_set),
        .i_rd_vpn_word (d_rb_vpn),
        .i_rd_pte_word (d_rb_pte),
        .o_wr_set      (sweep_wr_set),
        .o_wr_pte_en   (sweep_wr_pte_en),
        .o_wr_pte_word (sweep_wr_pte_word)
    );
    assign o_sys_busy = sweep_busy;

    // ── Shared write fan-out (both main-TLB copies) ──────────────
    // The sweep's writes, or an install into the selected slot's way.
    logic [SET_BITS-1:0]   wr_set;
    logic [WAYS-1:0]       wr_vpn_en, wr_pte_en;
    logic [31:0]           wr_vpn_word;
    logic [WAYS-1:0][31:0] wr_pte_word;
    always_comb begin
        if (sweep_busy) begin
            wr_set      = sweep_wr_set;
            wr_vpn_en   = '0;
            wr_vpn_word = vpn_stage_q;
            wr_pte_en   = sweep_wr_pte_en;
            wr_pte_word = sweep_wr_pte_word;
        end else begin
            wr_set      = idx_set;
            wr_vpn_word = vpn_stage_q;
            for (int w = 0; w < WAYS; w++) begin
                wr_vpn_en[w]   = commit_main && (idx_way == way_t'(w));
                wr_pte_en[w]   = commit_main && (idx_way == way_t'(w));
                wr_pte_word[w] = i_sys_wdata;
            end
        end
    end

    // ── D-copy read steer: the sweep, or a main-TLB read-back ────
    // Either places its set where penumbra3_translate's index derivation
    // reads it. The D-side is idle for both -- an RDSYS is not a memory
    // access, and a sweep holds fetch over a drained back end -- so neither
    // collides with a live data translation. The sweep also lifts the
    // copy's read hold, so its read is never frozen.
    logic        readback_main;
    logic [31:0] steer_vaddr;
    logic [31:0] d_vaddr_eff;
    logic        d_hold_eff;
    assign readback_main = i_sys_re
                         & ((i_sys_reg == SYSREG_MMU_TLB_VPN) | (i_sys_reg == SYSREG_MMU_TLB_PTE))
                         & ~idx_pinned & idx_main_exists;
    always_comb begin
        steer_vaddr = 32'b0;
        if (sweep_rd_en) steer_vaddr[12 +: SET_BITS] = sweep_rd_set;
        else             steer_vaddr[12 +: SET_BITS] = idx_set;
    end
    assign d_vaddr_eff = (sweep_rd_en || readback_main) ? steer_vaddr : i_d_vaddr;
    assign d_hold_eff  = i_d_hold && !sweep_rd_en;

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
        // Indexed sysreg access: the slot bits alone, since commit_pinned is
        // gated on idx_pinned_exists
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
    logic [WAYS-1:0][19:0] d_way_frame, i_way_frame;
    logic [WAYS-1:0]       d_way_sel, i_way_sel;

    penumbra3_translate #(.SETS(SETS), .WAYS(WAYS)) u_d (
        .i_clk             (i_clk),
        .i_rst             (i_rst),
        .i_lookup_en       (i_d_lookup_en),
        .i_vaddr           (d_vaddr_eff),
        .i_access_type     (i_d_access_type),
        .i_user_mode       (i_d_user_mode),
        .i_asid            (mmu_asid),
        .i_hold            (d_hold_eff),
        .i_pinned_hit      (1'b0),              // pinned override done in this wrapper
        .i_pinned_pte_word (32'b0),
        .o_paddr           (d_main_paddr),
        .o_cacheable       (d_main_cacheable),
        .o_hit             (d_main_hit),
        .o_miss_fault      (/* recomposed below with pinned/bypass */),
        .o_prot_fault      (d_main_prot),
        .o_way_frame       (d_way_frame),
        .o_way_sel         (d_way_sel),
        .o_rd_vpn_word     (d_rb_vpn),
        .o_rd_pte_word     (d_rb_pte),
        .i_wr_set          (wr_set),
        .i_wr_vpn_en       (wr_vpn_en),
        .i_wr_vpn_word     (wr_vpn_word),
        .i_wr_pte_en       (wr_pte_en),
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
        .o_way_frame       (i_way_frame),
        .o_way_sel         (i_way_sel),
        .o_rd_vpn_word     (/* read-back uses the D-copy */),
        .o_rd_pte_word     (),
        .i_wr_set          (wr_set),           // the same write to both copies
        .i_wr_vpn_en       (wr_vpn_en),
        .i_wr_vpn_word     (wr_vpn_word),
        .i_wr_pte_en       (wr_pte_en),
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

    logic bypass_d;
    assign bypass_d = ~mmu_m;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            {d_bypass_q, d_lookup_q, i_bypass_q, i_lookup_q} <= 4'b0;
            d_phit_q <= 1'b0; i_phit_q <= 1'b0;
        end else begin
            if (!i_d_hold) begin
                d_bypass_q <= bypass_d;
                d_lookup_q <= i_d_lookup_en;
                d_vaddr_q  <= i_d_vaddr;
                d_phit_q   <= pin_d_hit;
                d_ppaddr_q <= pin_d_paddr;
                d_pcache_q <= pin_d_cacheable;
                d_pprot_q  <= pin_d_fault;
            end
            if (!i_i_hold) begin
                i_bypass_q <= bypass_d;
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

    // ── Frame candidates (both ports) ────────────────────────────
    // The frame o_*_paddr carries, split so a physically tagged L1 can
    // compare against all of them while the main-TLB match resolves:
    // candidate 0 is the frame bypass or a pinned hit supplies, both
    // registered at launch and so known early in the resolve cycle; the
    // rest are the main-TLB ways' frames. The one-hot select follows the
    // same bypass > pinned > main priority, and is empty on a miss.
    logic d_early, i_early;
    assign d_early = d_bypass_q | d_phit_q;
    assign i_early = i_bypass_q | i_phit_q;

    assign o_d_frame[0]  = d_bypass_q ? d_vaddr_q[31:12] : d_ppaddr_q[31:12];
    assign o_i_frame[0]  = i_bypass_q ? i_vaddr_q[31:12] : i_ppaddr_q[31:12];
    assign o_d_frame_sel = {d_way_sel & {WAYS{~d_early}}, d_early};
    assign o_i_frame_sel = {i_way_sel & {WAYS{~i_early}}, i_early};
    always_comb
        for (int w = 0; w < WAYS; w++) begin
            o_d_frame[w + 1] = d_way_frame[w];
            o_i_frame[w + 1] = i_way_frame[w];
        end

    // ── Sysreg read response (one cycle after the RDSYS launch) ──
    // A slot that does not exist reads zero.
    logic [31:0] tlb_rb_vpn, tlb_rb_pte;
    always_comb begin
        if (idx_pinned && idx_pinned_exists) begin
            tlb_rb_vpn = pin_read_vpn;
            tlb_rb_pte = pin_read_pte;
        end else if (!idx_pinned && idx_main_exists) begin
            tlb_rb_vpn = d_rb_vpn[idx_way];
            tlb_rb_pte = d_rb_pte[idx_way];
        end else begin
            tlb_rb_vpn = 32'b0;
            tlb_rb_pte = 32'b0;
        end
    end
    always_comb begin
        case (rd_reg_q)
            SYSREG_MMU_CR:      o_sys_rdata = mmucr_q;
            SYSREG_MMU_FADDR:   o_sys_rdata = faddr_q;
            SYSREG_MMU_FSTAT:   o_sys_rdata = fstat_q;
            SYSREG_MMU_TLB_VPN: o_sys_rdata = tlb_rb_vpn;
            SYSREG_MMU_TLB_PTE: o_sys_rdata = tlb_rb_pte;
            SYSREG_MMU_TLB_IDX: o_sys_rdata = idx_q;
            SYSREG_MMU_CONFIG:  o_sys_rdata = MMU_CONFIG_VALUE;
            default:            o_sys_rdata = 32'b0;   // the invalidations read zero
        endcase
    end

    // ── Assertions (sim-only; Verilator --assert) ────────────────
    // bypass / pinned / main are a strict priority -- a protection fault is only
    // raised on a real (non-bypass) access.
    always_comb
        assert (!o_d_prot_fault || (!d_bypass_q && d_lookup_q))
            else $error("penumbra3_mmu: D protection fault under bypass / no lookup");

    // A sweep holds fetch over a drained back end: no install, no read-back
    // and no lookup can arrive while it runs.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(sweep_busy && (tlb_commit || i_sys_re || i_d_lookup_en || i_i_lookup_en)))
        else $error("penumbra3_mmu: TLB access during a sweep");

    // A miss and a prot fault are mutually exclusive on the same port.
    always_comb
        assert (!(o_d_miss_fault && o_d_prot_fault))
            else $error("penumbra3_mmu: D miss and prot asserted together");

    // On a lookup, the frame candidates select exactly the frame the paddr
    // carries.
    always_comb
        assert ($onehot0(o_d_frame_sel) && $onehot0(o_i_frame_sel))
            else $error("penumbra3_mmu: frame select not one-hot");
    for (genvar gk = 0; gk <= WAYS; gk++) begin : g_frame_check
        always_comb begin
            assert (i_rst || !d_lookup_q || !o_d_frame_sel[gk]
                    || o_d_frame[gk] == o_d_paddr[31:12])
                else $error("penumbra3_mmu: D frame candidate disagrees with paddr");
            assert (i_rst || !i_lookup_q || !o_i_frame_sel[gk]
                    || o_i_frame[gk] == o_i_paddr[31:12])
                else $error("penumbra3_mmu: I frame candidate disagrees with paddr");
        end
    end

    /* verilator lint_on PINCONNECTEMPTY */
endmodule
