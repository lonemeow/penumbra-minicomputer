// Penumbra gen2 TLB unit — BRAM main + flop pinned, two registered ports
//
// The gen2 equivalent of tlb_unit: it pairs the BRAM-backed main TLB
// (tlb_bram) with the fully-associative flop pinned TLB (tlb_pinned), and
// presents two registered translation ports (A = I-side, B = D-side) plus the
// indexed sysreg interface. Pinned-hit-wins, as in the single-cycle unit.
//
// Timing — the one subtlety. The main TLB is a registered (BRAM) lookup: drive
// the query at cycle T, its verdict is valid at T+1. The pinned TLB is
// combinational: its verdict for the query presented *this* cycle is available
// the same cycle. To combine them for one instruction, the pinned verdict is
// captured at the launch edge (registered here) so it rides to T+1 alongside
// the main TLB's verdict for that same query — rather than reading the pinned
// combinationally at T+1, which on the I-side would answer for the next fetch's
// PC (the I-side query advances every cycle). Both ports register the pinned
// verdict uniformly.
//
// The combined verdict holds from T+1 until the port's *next lookup* (capture
// on the strobe, hold otherwise — the registered-read contract, see tlb_bram).
// A stalled consumer therefore reads the same verdict on whichever cycle it
// advances. On port B the hold is really "until the port's next *operation*":
// a sysreg readback reloads tlb_bram's shared port-B way registers and
// supersedes a main-won verdict — but a readback comes from a later RDSYS in
// MEM, which can only launch after the translate's own instruction has
// advanced out of MEM, its verdict consumed.
//
// Sysreg readback is the easy case: RDSYS back-pressures MEM, so TLB_INDEX and
// the selected register are held stable across the two access cycles, so the
// readback mux stays combinational — the main TLB's VPN/PTE come from
// tlb_bram's registered readback (launched by i_sys_re), pinned and index
// readback are combinational off the held index.

// verilator lint_off UNUSEDSIGNAL

module tlb_unit_bram
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
    output logic [31:0] o_a_fault_status,

    // ── Port B: D-side translate (same registered contract) ──
    input  logic [31:0] i_b_vaddr,
    input  logic [2:0]  i_b_access_type,
    input  logic        i_b_user_mode,
    input  logic        i_b_lookup_en,
    output logic [31:0] o_b_paddr,
    output logic        o_b_cacheable,
    output logic        o_b_hit,
    output logic        o_b_fault,
    output logic [31:0] o_b_fault_status,

    // ── Sysreg interface (regs 3-5) ──
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    input  logic        i_sys_re,          // readback launch (registered main readback)
    output logic [31:0] o_sys_rdata        // valid the cycle after i_sys_re for TLB_VPN/PTE
);

    localparam int PINNED_SLOTS = 8;
    localparam int PINNED_IDX_W = $clog2(PINNED_SLOTS);

    // ══════════════════════════════════════════════════════════
    // Sysreg staging registers + write/read routing
    // ══════════════════════════════════════════════════════════
    logic [31:0] tlb_index_reg, tlb_vpn_reg;
    logic        select_pinned;
    assign select_pinned = tlb_index_reg[6];

    logic pte_write, main_write_en, pin_write_en;
    assign pte_write     = i_sys_we && (i_sys_reg == SYSREG_MMU_TLB_PTE);
    assign main_write_en = pte_write && !select_pinned;
    assign pin_write_en  = pte_write &&  select_pinned;

    // Main TLB readback launches only for a main-TLB VPN/PTE read.
    logic main_read_en;
    assign main_read_en = i_sys_re && !select_pinned
                       && ((i_sys_reg == SYSREG_MMU_TLB_VPN) ||
                           (i_sys_reg == SYSREG_MMU_TLB_PTE));

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            tlb_index_reg <= 32'b0;
            tlb_vpn_reg   <= 32'b0;
        end else if (i_sys_we) begin
            case (i_sys_reg)
                SYSREG_MMU_TLB_IDX: tlb_index_reg <= i_sys_wdata;
                SYSREG_MMU_TLB_VPN: tlb_vpn_reg   <= i_sys_wdata;
                default: ;
            endcase
        end
    end

    // ══════════════════════════════════════════════════════════
    // Main TLB (BRAM, dual-port, registered)
    // ══════════════════════════════════════════════════════════
    logic [31:0] main_a_paddr, main_a_fstatus;
    logic        main_a_cacheable, main_a_hit, main_a_fault;
    logic [31:0] main_b_paddr, main_b_fstatus;
    logic        main_b_cacheable, main_b_hit, main_b_fault;
    logic [31:0] main_read_vpn, main_read_pte;

    tlb_bram u_main (
        .i_clk(i_clk), .i_rst(i_rst), .i_asid(i_asid),
        .i_a_vaddr(i_a_vaddr), .i_a_access_type(i_a_access_type),
        .i_a_user_mode(i_a_user_mode), .i_a_lookup_en(i_a_lookup_en),
        .o_a_paddr(main_a_paddr), .o_a_cacheable(main_a_cacheable),
        .o_a_hit(main_a_hit), .o_a_fault(main_a_fault),
        .o_a_fault_status(main_a_fstatus),
        .i_b_vaddr(i_b_vaddr), .i_b_access_type(i_b_access_type),
        .i_b_user_mode(i_b_user_mode), .i_b_lookup_en(i_b_lookup_en),
        .o_b_paddr(main_b_paddr), .o_b_cacheable(main_b_cacheable),
        .o_b_hit(main_b_hit), .o_b_fault(main_b_fault),
        .o_b_fault_status(main_b_fstatus),
        .i_idx_set(tlb_index_reg[4:0]), .i_idx_way(tlb_index_reg[5]),
        .i_write_vpn(tlb_vpn_reg), .i_write_pte(i_sys_wdata),
        .i_write_en(main_write_en), .i_read_en(main_read_en),
        .o_read_vpn(main_read_vpn), .o_read_pte(main_read_pte)
    );

    // ══════════════════════════════════════════════════════════
    // Pinned TLB (flops, dual-lookup, combinational)
    // ══════════════════════════════════════════════════════════
    logic [31:0] pin_a_paddr, pin_a_fstatus;
    logic        pin_a_cacheable, pin_a_hit, pin_a_fault;
    logic [31:0] pin_b_paddr, pin_b_fstatus;
    logic        pin_b_cacheable, pin_b_hit, pin_b_fault;
    logic [31:0] pin_read_vpn, pin_read_pte;

    tlb_pinned #(
        .NUM_ENTRIES   (PINNED_SLOTS),
        .DUAL_TRANSLATE(1'b1)
    ) u_pinned (
        .i_clk(i_clk), .i_rst(i_rst), .i_asid(i_asid),
        .i_vaddr(i_a_vaddr), .i_access_type(i_a_access_type),
        .i_user_mode(i_a_user_mode), .i_lookup_en(i_a_lookup_en),
        .o_paddr(pin_a_paddr), .o_cacheable(pin_a_cacheable),
        .o_hit(pin_a_hit), .o_fault(pin_a_fault), .o_fault_status(pin_a_fstatus),
        .i_b_vaddr(i_b_vaddr), .i_b_access_type(i_b_access_type),
        .i_b_user_mode(i_b_user_mode), .i_b_lookup_en(i_b_lookup_en),
        .o_b_paddr(pin_b_paddr), .o_b_cacheable(pin_b_cacheable),
        .o_b_hit(pin_b_hit), .o_b_fault(pin_b_fault), .o_b_fault_status(pin_b_fstatus),
        .i_idx(tlb_index_reg[PINNED_IDX_W-1:0]),
        .i_write_vpn(tlb_vpn_reg), .i_write_pte(i_sys_wdata),
        .i_write_en(pin_write_en),
        .o_read_vpn(pin_read_vpn), .o_read_pte(pin_read_pte)
    );

    // ══════════════════════════════════════════════════════════
    // Register the pinned verdict to align it with the main TLB's
    // registered verdict (see header). Hit bits reset so no spurious
    // pinned hit is presented before the first real lookup; the data is
    // consumed only when the hit bit is set, so it needs no reset.
    // ══════════════════════════════════════════════════════════
    logic [31:0] pin_a_paddr_q, pin_a_fstatus_q;
    logic        pin_a_cacheable_q, pin_a_hit_q, pin_a_fault_q;
    logic [31:0] pin_b_paddr_q, pin_b_fstatus_q;
    logic        pin_b_cacheable_q, pin_b_hit_q, pin_b_fault_q;

    // Captured on the lookup strobe and held otherwise, so the combined
    // verdict obeys the registered-read hold contract (see tlb_bram header).
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            pin_a_hit_q <= 1'b0;
            pin_b_hit_q <= 1'b0;
        end else begin
            if (i_a_lookup_en) pin_a_hit_q <= pin_a_hit;
            if (i_b_lookup_en) pin_b_hit_q <= pin_b_hit;
        end
        if (i_a_lookup_en) begin
            pin_a_paddr_q     <= pin_a_paddr;
            pin_a_cacheable_q <= pin_a_cacheable;
            pin_a_fault_q     <= pin_a_fault;
            pin_a_fstatus_q   <= pin_a_fstatus;
        end
        if (i_b_lookup_en) begin
            pin_b_paddr_q     <= pin_b_paddr;
            pin_b_cacheable_q <= pin_b_cacheable;
            pin_b_fault_q     <= pin_b_fault;
            pin_b_fstatus_q   <= pin_b_fstatus;
        end
    end

    // ══════════════════════════════════════════════════════════
    // Combine — pinned-hit-wins, at T+1
    // ══════════════════════════════════════════════════════════
    always_comb begin
        if (pin_a_hit_q) begin
            o_a_paddr        = pin_a_paddr_q;
            o_a_cacheable    = pin_a_cacheable_q;
            o_a_hit          = 1'b1;
            o_a_fault        = pin_a_fault_q;
            o_a_fault_status = pin_a_fstatus_q;
        end else begin
            o_a_paddr        = main_a_paddr;
            o_a_cacheable    = main_a_cacheable;
            o_a_hit          = main_a_hit;
            o_a_fault        = main_a_fault;
            o_a_fault_status = main_a_fstatus;
        end
    end

    always_comb begin
        if (pin_b_hit_q) begin
            o_b_paddr        = pin_b_paddr_q;
            o_b_cacheable    = pin_b_cacheable_q;
            o_b_hit          = 1'b1;
            o_b_fault        = pin_b_fault_q;
            o_b_fault_status = pin_b_fstatus_q;
        end else begin
            o_b_paddr        = main_b_paddr;
            o_b_cacheable    = main_b_cacheable;
            o_b_hit          = main_b_hit;
            o_b_fault        = main_b_fault;
            o_b_fault_status = main_b_fstatus;
        end
    end

    // ══════════════════════════════════════════════════════════
    // Sysreg read mux (combinational; inputs held by MEM across the
    // two access cycles, so valid at T+1 — the main TLB VPN/PTE come
    // from tlb_bram's registered readback, pinned/index off held inputs)
    // ══════════════════════════════════════════════════════════
    always_comb begin
        case (i_sys_reg)
            SYSREG_MMU_TLB_VPN: o_sys_rdata = select_pinned ? pin_read_vpn : main_read_vpn;
            SYSREG_MMU_TLB_PTE: o_sys_rdata = select_pinned ? pin_read_pte : main_read_pte;
            SYSREG_MMU_TLB_IDX: o_sys_rdata = tlb_index_reg;
            default:            o_sys_rdata = 32'b0;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════
    // A commit targets the main TLB or the pinned TLB, never both — the
    // TLB_INDEX[6] select splits them. Structural today, asserted so a
    // future refactor of the routing trips the SVA.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(main_write_en && pin_write_en))
        else $error("tlb_unit_bram: main and pinned write enables both asserted");

endmodule

// verilator lint_on UNUSEDSIGNAL
