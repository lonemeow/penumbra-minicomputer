// Penumbra TLB Unit — unified translation lookup + sysreg interface
//
// Wraps the main TLB (64-entry 2-way SA) and pinned TLB (PINNED_SLOTS-
// entry FA) behind a single interface.  The MMU connects to this
// module for both address translation and sysreg access to TLB entries.
//
// Sysreg registers handled here (device 0):
//   3 = TLB_VPN       VPN staging (shared by both TLBs)
//   4 = TLB_PTE       PTE commit (routed by TLB_INDEX[6])
//   5 = TLB_INDEX     Slot selector:
//                        bit 6 = 0 → main TLB, {way=bit5, set=bits4:0}
//                        bit 6 = 1 → pinned TLB, slot=bits[PINNED_IDX_W-1:0]
//
// Pinned TLB hit takes priority over main TLB on lookup.

// verilator lint_off UNUSEDSIGNAL

module tlb_unit
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Lookup interface ──────────────────────────────────
    input  logic [31:0] i_vaddr,
    input  logic [2:0]  i_access_type,
    input  logic        i_user_mode,
    input  logic [7:0]  i_asid,
    input  logic        i_lookup_en,

    output logic [31:0] o_paddr,
    output logic        o_cacheable,
    output logic        o_hit,
    output logic        o_fault,
    output logic [31:0] o_fault_status,

    // ── Sysreg interface (regs 3-5 only) ──────────────────
    input  logic [3:0]  i_sys_reg,
    input  logic [31:0] i_sys_wdata,
    input  logic        i_sys_we,
    output logic [31:0] o_sys_rdata
);

    // ══════════════════════════════════════════════════════════
    // Pinned TLB geometry — single source of truth
    // ══════════════════════════════════════════════════════════
    // tlb_pinned's NUM_ENTRIES is overridden from PINNED_SLOTS so the
    // index-bit slice below and the storage depth can never drift.

    localparam int PINNED_SLOTS = 8;
    localparam int PINNED_IDX_W = $clog2(PINNED_SLOTS);

    // ══════════════════════════════════════════════════════════
    // Registers
    // ══════════════════════════════════════════════════════════

    logic [31:0] tlb_index_reg;
    logic [31:0] tlb_vpn_reg;     // Shared staging register

    // ── Decode TLB_INDEX ───────────────────────────────────
    logic        select_pinned;   // bit 6 selects pinned TLB
    assign select_pinned = tlb_index_reg[6];

    // ── Write enables ──────────────────────────────────────
    logic pte_write;
    assign pte_write = i_sys_we && (i_sys_reg == SYSREG_MMU_TLB_PTE);

    logic main_write_en;
    logic pin_write_en;
    assign main_write_en = pte_write && !select_pinned;
    assign pin_write_en  = pte_write &&  select_pinned;

    // ── Register writes ────────────────────────────────────
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
    // Main TLB (64-entry, 2-way set-associative)
    // ══════════════════════════════════════════════════════════

    logic [31:0] main_paddr, main_fault_status;
    logic        main_cacheable, main_hit, main_fault;
    logic [31:0] main_read_vpn, main_read_pte;

    tlb u_main (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_vaddr        (i_vaddr),
        .i_access_type  (i_access_type),
        .i_user_mode    (i_user_mode),
        .i_asid         (i_asid),
        .i_lookup_en    (i_lookup_en),
        .o_paddr        (main_paddr),
        .o_cacheable    (main_cacheable),
        .o_hit          (main_hit),
        .o_fault        (main_fault),
        .o_fault_status (main_fault_status),
        .i_idx_set      (tlb_index_reg[4:0]),
        .i_idx_way      (tlb_index_reg[5]),
        .i_write_vpn    (tlb_vpn_reg),
        .i_write_pte    (i_sys_wdata),
        .i_write_en     (main_write_en),
        .o_read_vpn     (main_read_vpn),
        .o_read_pte     (main_read_pte)
    );

    // ══════════════════════════════════════════════════════════
    // Pinned TLB (PINNED_SLOTS entries, fully associative)
    // ══════════════════════════════════════════════════════════

    logic [31:0] pin_paddr, pin_fault_status;
    logic        pin_cacheable, pin_hit, pin_fault;
    logic [31:0] pin_read_vpn, pin_read_pte;

    tlb_pinned #(
        .NUM_ENTRIES (PINNED_SLOTS)
    ) u_pinned (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_vaddr        (i_vaddr),
        .i_access_type  (i_access_type),
        .i_user_mode    (i_user_mode),
        .i_asid         (i_asid),
        .i_lookup_en    (i_lookup_en),
        .o_paddr        (pin_paddr),
        .o_cacheable    (pin_cacheable),
        .o_hit          (pin_hit),
        .o_fault        (pin_fault),
        .o_fault_status (pin_fault_status),
        .i_idx          (tlb_index_reg[PINNED_IDX_W-1:0]),
        .i_write_vpn    (tlb_vpn_reg),
        .i_write_pte    (i_sys_wdata),
        .i_write_en     (pin_write_en),
        .o_read_vpn     (pin_read_vpn),
        .o_read_pte     (pin_read_pte)
    );

    // ══════════════════════════════════════════════════════════
    // Combined lookup: pinned hit takes priority
    // ══════════════════════════════════════════════════════════

    always_comb begin
        if (pin_hit) begin
            o_paddr        = pin_paddr;
            o_cacheable    = pin_cacheable;
            o_hit          = 1'b1;
            o_fault        = pin_fault;
            o_fault_status = pin_fault_status;
        end else begin
            o_paddr        = main_paddr;
            o_cacheable    = main_cacheable;
            o_hit          = main_hit;
            o_fault        = main_fault;
            o_fault_status = main_fault_status;
        end
    end

    // ══════════════════════════════════════════════════════════
    // Sysreg read mux — TLB_INDEX[6] selects which TLB to read
    // ══════════════════════════════════════════════════════════

    always_comb begin
        case (i_sys_reg)
            SYSREG_MMU_TLB_VPN: o_sys_rdata = select_pinned ? pin_read_vpn : main_read_vpn;
            SYSREG_MMU_TLB_PTE: o_sys_rdata = select_pinned ? pin_read_pte : main_read_pte;
            SYSREG_MMU_TLB_IDX: o_sys_rdata = tlb_index_reg;
            default:            o_sys_rdata = 32'b0;
        endcase
    end

endmodule

// verilator lint_on UNUSEDSIGNAL
