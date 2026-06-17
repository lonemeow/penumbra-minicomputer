// Penumbra gen2 MMU — two registered translate ports, commit-time faults
//
// The gen2 counterpart of mmu.sv. It wraps tlb_unit_bram (BRAM main + flop
// pinned, two registered translation ports) and adds the MMU control the TLB
// does not own: MMUCR (enable + ASID), the sysreg read/write for MMUCR, the
// per-port bypass (identity map when the MMU is disabled or force_bypass is
// set, e.g. a vector fetch), and the architectural fault registers.
//
// Timing. Translation is registered: drive a port's query at cycle T, its
// verdict is valid at T+1 (the TLB's BRAM read) and holds until the port's
// next query (capture on the strobe, hold otherwise — the registered-read
// contract; see tlb_bram). The bypass decision is made on the same T inputs
// and registered under the same strobe, so the identity-map result lands in
// the same T+1 cycle as the TLB verdict it muxes against and holds with it.
//
// Faults. A translate port reports o_*_fault for any translation fault — a
// TLB miss or a protection denial — with the fully-composed FAULT_STATUS
// alongside (the detector composes; Decision 16). The composition uses the
// query's access-type/privilege registered here at the launch, so the status
// is self-contained at T+1. Idle and bypassing ports are fault=0 by
// construction: a consumer can never misread a port that ran nothing. The
// MMU does not check alignment (the gen2 MEM stage owns that, Decision 15),
// and it does not *take* faults: FADDR/FSTAT latch only from the external
// commit strobe the core drives at WB, so they reflect the fault that
// actually retires, never a younger one that was detected and then squashed.
// See Decisions 13, 15, and 16 in doc/internals/penumbra2/design-decisions.md.

// verilator lint_off UNUSEDSIGNAL

// keep_hierarchy: hold this boundary through synth_ecp5 so the translate
// verdict cone reads with real signal names in timing reports and places as
// a unit. Paired across the TLB cone modules (mmu_bram / tlb_unit_bram /
// tlb_bram / tlb_perm).
(* keep_hierarchy = "yes" *)
module mmu_bram
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Port A: I-side translate (drive at T, verdict at T+1) ──
    input  logic [31:0] i_a_vaddr,
    input  logic [2:0]  i_a_access_type,   // typically ACC_EXEC
    input  logic        i_a_user_mode,
    input  logic        i_a_req,            // translation requested this cycle
    input  logic        i_a_force_bypass,   // identity-map (vector fetch)
    output logic [31:0] o_a_paddr,
    output logic        o_a_cacheable,
    output logic        o_a_hit,
    output logic        o_a_fault,           // any translation fault (miss / protection)
    output logic [31:0] o_a_fault_status,    // composed; FAULT_NONE when no fault

    // ── Port B: D-side translate (same registered contract) ──
    input  logic [31:0] i_b_vaddr,
    input  logic [2:0]  i_b_access_type,    // ACC_READ / ACC_WRITE
    input  logic        i_b_user_mode,
    input  logic        i_b_req,
    input  logic        i_b_force_bypass,
    output logic [31:0] o_b_paddr,
    output logic        o_b_cacheable,
    output logic        o_b_hit,
    output logic        o_b_fault,           // any translation fault (miss / protection)
    output logic [31:0] o_b_fault_status,    // composed; FAULT_NONE when no fault

    // ── Commit-time fault latch (core drives at WB) ──
    input  logic        i_fault_commit,     // latch FADDR/FSTAT this cycle
    input  logic [31:0] i_fault_vaddr,
    input  logic [31:0] i_fault_status,

    // ── Sysreg interface (WRSYS/RDSYS, dev_id = 0) ──
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    input  logic        i_sys_re,           // registered TLB readback launch
    output logic [31:0] o_sys_rdata
);

    // ══════════════════════════════════════════════════════════
    // Control registers
    // ══════════════════════════════════════════════════════════
    logic [31:0] mmucr;                       // [0]=M (enable), [15:8]=ASID
    logic [31:0] fault_addr, fault_status;

    logic        mmu_enabled;
    logic [7:0]  current_asid;
    assign mmu_enabled  = mmucr[0];
    assign current_asid = mmucr[15:8];

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            mmucr        <= 32'b0;
            fault_addr   <= 32'b0;
            fault_status <= 32'b0;
        end else begin
            if (i_sys_we && i_sys_reg == SYSREG_MMU_CR)
                mmucr <= i_sys_wdata;
            // Fault registers latch only what the core commits at WB.
            if (i_fault_commit) begin
                fault_addr   <= i_fault_vaddr;
                fault_status <= i_fault_status;
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Per-port translate enable + bypass (T), registered to T+1
    // ══════════════════════════════════════════════════════════
    // A port translates through the TLB when the MMU is on and bypass is not
    // forced; otherwise it identity-maps (bypass). The bypass decision and the
    // vaddr ride a register to meet the TLB's T+1 verdict.
    logic a_translate, b_translate;
    logic a_bypass, b_bypass;
    assign a_translate = i_a_req && mmu_enabled && !i_a_force_bypass;
    assign b_translate = i_b_req && mmu_enabled && !i_b_force_bypass;
    assign a_bypass    = i_a_req && (i_a_force_bypass || !mmu_enabled);
    assign b_bypass    = i_b_req && (i_b_force_bypass || !mmu_enabled);

    // Captured on the query strobe and held otherwise, matching the TLB's
    // verdict-hold contract — so the muxed port output stays coherent for a
    // consumer that advances later than T+1. Besides the bypass decision and
    // vaddr, the capture keeps the query's access-type/privilege (the status
    // composition needs them at T+1) and whether a *translate* verdict is
    // live — the qualifier that keeps idle and bypassing ports at fault=0.
    logic        a_bypass_q, b_bypass_q;
    logic        a_xlate_q,  b_xlate_q;
    logic [31:0] a_vaddr_q, b_vaddr_q;
    logic [2:0]  a_acc_q, b_acc_q;
    logic        a_usr_q, b_usr_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            a_bypass_q <= 1'b0;
            b_bypass_q <= 1'b0;
            a_xlate_q  <= 1'b0;
            b_xlate_q  <= 1'b0;
        end else begin
            if (i_a_req) begin
                a_bypass_q <= a_bypass;
                a_xlate_q  <= a_translate;
            end
            if (i_b_req) begin
                b_bypass_q <= b_bypass;
                b_xlate_q  <= b_translate;
            end
        end
        if (i_a_req) begin
            a_vaddr_q <= i_a_vaddr;
            a_acc_q   <= i_a_access_type;
            a_usr_q   <= i_a_user_mode;
        end
        if (i_b_req) begin
            b_vaddr_q <= i_b_vaddr;
            b_acc_q   <= i_b_access_type;
            b_usr_q   <= i_b_user_mode;
        end
    end

    // ══════════════════════════════════════════════════════════
    // TLB unit (registered, dual-port)
    // ══════════════════════════════════════════════════════════
    logic [31:0] tlb_a_paddr, tlb_b_paddr;
    logic        tlb_a_cacheable, tlb_a_hit, tlb_a_fault;
    logic        tlb_b_cacheable, tlb_b_hit, tlb_b_fault;
    logic [31:0] tlb_rdata;

    tlb_unit_bram u_tlb (
        .i_clk(i_clk), .i_rst(i_rst), .i_asid(current_asid),
        .i_a_vaddr(i_a_vaddr), .i_a_access_type(i_a_access_type),
        .i_a_user_mode(i_a_user_mode), .i_a_lookup_en(a_translate),
        .o_a_paddr(tlb_a_paddr), .o_a_cacheable(tlb_a_cacheable),
        .o_a_hit(tlb_a_hit), .o_a_fault(tlb_a_fault),
        .i_b_vaddr(i_b_vaddr), .i_b_access_type(i_b_access_type),
        .i_b_user_mode(i_b_user_mode), .i_b_lookup_en(b_translate),
        .o_b_paddr(tlb_b_paddr), .o_b_cacheable(tlb_b_cacheable),
        .o_b_hit(tlb_b_hit), .o_b_fault(tlb_b_fault),
        .i_sys_reg(i_sys_reg), .i_sys_wdata(i_sys_wdata),
        .i_sys_we(i_sys_we), .i_sys_re(i_sys_re), .o_sys_rdata(tlb_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Per-port verdict (T+1): bypass identity-maps (uncacheable, hits, no
    // fault); a live translate presents the TLB result with the fault
    // composed here — covering both the miss (TLB hit=0) and the protection
    // denial — from the query info captured at the launch. A port with no
    // live translate (idle, or bypassing) is fault=0 by construction, and
    // the status is FAULT_NONE whenever there is no fault (self-qualifying).
    // ══════════════════════════════════════════════════════════
    always_comb begin
        o_a_paddr     = a_bypass_q ? a_vaddr_q : tlb_a_paddr;
        o_a_cacheable = a_bypass_q ? 1'b0      : tlb_a_cacheable;
        o_a_hit       = a_bypass_q | (a_xlate_q & tlb_a_hit);
        o_a_fault     = a_xlate_q & (tlb_a_fault | ~tlb_a_hit);
        o_a_fault_status = o_a_fault
            ? compose_fault_status(a_usr_q, a_acc_q, tlb_a_hit ? FAULT_PROT : FAULT_TLB_MISS)
            : 32'b0;
    end

    always_comb begin
        o_b_paddr     = b_bypass_q ? b_vaddr_q : tlb_b_paddr;
        o_b_cacheable = b_bypass_q ? 1'b0      : tlb_b_cacheable;
        o_b_hit       = b_bypass_q | (b_xlate_q & tlb_b_hit);
        o_b_fault     = b_xlate_q & (tlb_b_fault | ~tlb_b_hit);
        o_b_fault_status = o_b_fault
            ? compose_fault_status(b_usr_q, b_acc_q, tlb_b_hit ? FAULT_PROT : FAULT_TLB_MISS)
            : 32'b0;
    end

    // ══════════════════════════════════════════════════════════
    // Sysreg read mux — MMUCR/FADDR/FSTAT here, TLB regs from the unit.
    // Inputs are held by MEM across the 2-cycle RDSYS, so this stays
    // combinational (the TLB's own readback is registered internally).
    // ══════════════════════════════════════════════════════════
    always_comb begin
        case (i_sys_reg)
            SYSREG_MMU_CR:    o_sys_rdata = mmucr;
            SYSREG_MMU_FADDR: o_sys_rdata = fault_addr;
            SYSREG_MMU_FSTAT: o_sys_rdata = fault_status;
            default:          o_sys_rdata = tlb_rdata;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════
    // A port is either translating or bypassing, never both — they split on
    // force_bypass / enable from the same request.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(a_translate && a_bypass) && !(b_translate && b_bypass))
        else $error("mmu_bram: a port both translates and bypasses");

endmodule

// verilator lint_on UNUSEDSIGNAL
