// Penumbra gen2 MMU — two registered translate ports, commit-time faults
//
// The gen2 counterpart of mmu.sv. It wraps tlb_unit_bram (BRAM main + flop
// pinned, two registered translation ports) and adds the MMU control the TLB
// does not own: MMUCR (enable + ASID), the sysreg read/write for MMUCR, the
// per-port bypass (identity map when the MMU is disabled or force_bypass is
// set, e.g. a vector fetch), and the architectural fault registers.
//
// Timing. Translation is registered at the verdict: the async TLB
// (tlb_unit_bram → tlb_bram, LUTRAM) resolves the query combinationally in the
// access launch cycle T, the bypass mux and fault composition run on the same
// live T inputs, and the whole verdict is captured in one output register on
// the port's request strobe — valid at T+1 and held until the next query
// (capture on the strobe, hold otherwise — the registered-read contract). This
// runs the translate cone in the otherwise-idle launch cycle rather than
// stacking it on the cycle that consumes it.
//
// Faults. A translate port reports o_*_fault for any translation fault — a
// TLB miss or a protection denial — with the fully-composed FAULT_STATUS
// alongside (the detector composes; Decision 16). The composition uses the
// live query's access-type/privilege, registered with the rest of the verdict,
// so the status is self-contained at T+1. Idle and bypassing ports are fault=0 by
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
    // Per-port translate-mode + bypass (combinational)
    // ══════════════════════════════════════════════════════════
    // The port MODE: translate through the TLB when the MMU is on and bypass is
    // not forced, else identity-map (bypass).
    //
    // Port A (I-side) drops the per-request gate. It has no sysreg duty, so its
    // TLB read address is always the fetch PC, and the verdict register's
    // capture enable (i_a_req, below) is what holds an idle/stalled cycle's
    // verdict rather than latching a spurious one. Dropping i_a_req from the
    // mode keeps it off the combinational verdict cone — and so off the late
    // fetch-request path that carries the IF2->IF1 back-pressure from a busy
    // I-cache; i_a_req then gates only the register CE.
    //
    // Port B (D-side) keeps i_b_req: its lookup enable doubles as the port-B
    // read-address select (D-translate vaddr vs the sysreg readback index, in
    // tlb_bram) and backs the port-B contention guard, so it must mean "a
    // D-translate is happening this cycle", not just the mode. The D-side
    // request is not the critical late signal anyway — its EA comes from a
    // registered EX result — so leaving it gated costs nothing.
    logic a_translate, b_translate;
    logic a_bypass, b_bypass;
    assign a_translate = mmu_enabled && !i_a_force_bypass;
    assign b_translate = i_b_req && mmu_enabled && !i_b_force_bypass;
    assign a_bypass    = i_a_force_bypass || !mmu_enabled;
    assign b_bypass    = i_b_req && (i_b_force_bypass || !mmu_enabled);

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
    // Per-port verdict — composed combinationally this cycle (the launch
    // cycle), then registered once below. Bypass identity-maps (uncacheable,
    // hits, no fault); a translate presents the TLB result with the fault
    // composed here — miss (TLB hit=0) or protection denial — from the live
    // query. A bypassing port is fault=0 by construction; an idle or stalled
    // port simply holds its prior verdict, since the register captures only on
    // i_*_req. The status is FAULT_NONE whenever there is no fault.
    // ══════════════════════════════════════════════════════════
    logic [31:0] a_paddr_n, b_paddr_n;
    logic        a_cacheable_n, a_hit_n, a_fault_n;
    logic        b_cacheable_n, b_hit_n, b_fault_n;
    logic [31:0] a_fault_status_n, b_fault_status_n;

    always_comb begin
        a_paddr_n        = a_bypass ? i_a_vaddr : tlb_a_paddr;
        a_cacheable_n    = a_bypass ? 1'b0      : tlb_a_cacheable;
        a_hit_n          = a_bypass | (a_translate & tlb_a_hit);
        a_fault_n        = a_translate & (tlb_a_fault | ~tlb_a_hit);
        a_fault_status_n = a_fault_n
            ? compose_fault_status(i_a_user_mode, i_a_access_type, tlb_a_hit ? FAULT_PROT : FAULT_TLB_MISS)
            : 32'b0;
    end

    always_comb begin
        b_paddr_n        = b_bypass ? i_b_vaddr : tlb_b_paddr;
        b_cacheable_n    = b_bypass ? 1'b0      : tlb_b_cacheable;
        b_hit_n          = b_bypass | (b_translate & tlb_b_hit);
        b_fault_n        = b_translate & (tlb_b_fault | ~tlb_b_hit);
        b_fault_status_n = b_fault_n
            ? compose_fault_status(i_b_user_mode, i_b_access_type, tlb_b_hit ? FAULT_PROT : FAULT_TLB_MISS)
            : 32'b0;
    end

    // ══════════════════════════════════════════════════════════
    // Verdict register — the single pipeline register for translation.
    // This is where the async-TLB launch-cycle cone is cut: the combinational
    // verdict above is captured on the port's request strobe (i_*_req) and held
    // otherwise, so it lands the cycle after the query and stays stable for a
    // consumer that advances later — the registered-read hold contract the rest
    // of the memory system relies on. The hit/fault bits must reset low so an
    // idle port presents no spurious verdict before its first lookup.
    // ══════════════════════════════════════════════════════════
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            o_a_hit          <= 1'b0;
            o_a_fault        <= 1'b0;
            o_b_hit          <= 1'b0;
            o_b_fault        <= 1'b0;
        end else begin
            if (i_a_req) begin
                o_a_paddr        <= a_paddr_n;
                o_a_cacheable    <= a_cacheable_n;
                o_a_hit          <= a_hit_n;
                o_a_fault        <= a_fault_n;
                o_a_fault_status <= a_fault_status_n;
            end
            if (i_b_req) begin
                o_b_paddr        <= b_paddr_n;
                o_b_cacheable    <= b_cacheable_n;
                o_b_hit          <= b_hit_n;
                o_b_fault        <= b_fault_n;
                o_b_fault_status <= b_fault_status_n;
            end
        end
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
