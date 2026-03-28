// Penumbra MMU — Memory Management Unit
//
// Orchestrates TLB lookup and bypass mode. When M=0 (reset default),
// identity maps all addresses as uncacheable. When M=1, routes
// translation through the TLB and reports misses/faults.
//
// TLB: 64-entry 2-way set-associative, 64-bit entries,
// fully software-managed (no hardware replacement or dirty tracking).
//
// Sysreg interface (dev_id=0) provides MMUCR, fault registers,
// and TLB indexed access via TLB_VPN/TLB_PTE/TLB_INDEX.

// verilator lint_off UNUSEDSIGNAL

module mmu
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── CPU interface ──────────────────────────────────────
    input  logic [31:0] i_vaddr,        // Virtual address
    input  logic [2:0]  i_access_type,  // ACC_READ / ACC_WRITE / ACC_EXEC (one-hot)
    input  logic        i_user_mode,    // 1 = user mode (from !SR.S)
    input  logic        i_req,          // Translation request

    // ── Translation output ─────────────────────────────────
    output logic [31:0] o_paddr,        // Physical address
    output logic        o_cacheable,    // PTE.C (0 in bypass mode)
    output logic        o_fault,        // Access violation / TLB miss
    output logic        o_hit,          // TLB hit (always 1 in bypass)

    // ── Sysreg interface (WRSYS/RDSYS, dev_id = 0) ────────
    input  logic [3:0]  i_sys_reg,      // Register address within MMU
    input  logic [31:0] i_sys_wdata,    // Write data
    input  logic        i_sys_we,       // Write enable
    output logic [31:0] o_sys_rdata     // Read data
);

    // ══════════════════════════════════════════════════════════
    // Control registers
    // ══════════════════════════════════════════════════════════

    // MMUCR: [0]=M (enable), [15:8]=ASID
    logic [31:0] mmucr;
    logic [31:0] fault_addr;
    logic [31:0] fault_status;

    logic        mmu_enabled;
    logic [7:0]  current_asid;

    assign mmu_enabled  = mmucr[0];
    assign current_asid = mmucr[15:8];

    // ── TLB index register ─────────────────────────────────
    logic [31:0] tlb_index_reg;
    logic [4:0]  idx_set;
    logic        idx_way;
    assign idx_set = tlb_index_reg[4:0];
    assign idx_way = tlb_index_reg[5];

    // ── TLB VPN staging register ───────────────────────────
    // Written first, then TLB_PTE write commits both to TLB
    logic [31:0] tlb_vpn_reg;

    // ══════════════════════════════════════════════════════════
    // Sysreg write logic
    // ══════════════════════════════════════════════════════════

    logic tlb_write_en;  // Pulse on TLB_PTE write → commits entry
    assign tlb_write_en = i_sys_we && (i_sys_reg == SYSREG_MMU_TLB_PTE);

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            mmucr         <= 32'b0;
            fault_addr    <= 32'b0;
            fault_status  <= 32'b0;
            tlb_index_reg <= 32'b0;
            tlb_vpn_reg   <= 32'b0;
        end else begin
            // Latch fault info on TLB miss or protection fault
            if (mmu_enabled && i_req && tlb_fault) begin
                fault_addr   <= i_vaddr;
                fault_status <= tlb_fault_status;
            end else if (mmu_enabled && i_req && !tlb_hit) begin
                // TLB miss (no matching entry)
                fault_addr   <= i_vaddr;
                fault_status <= {20'b0, i_user_mode, i_access_type, 4'b0, FAULT_TLB_MISS};
            end

            // Sysreg writes
            if (i_sys_we) begin
                case (i_sys_reg)
                    SYSREG_MMU_CR:      mmucr         <= i_sys_wdata;
                    SYSREG_MMU_TLB_IDX: tlb_index_reg <= i_sys_wdata;
                    SYSREG_MMU_TLB_VPN: tlb_vpn_reg   <= i_sys_wdata;
                    // TLB_PTE: data goes directly to TLB via tlb_write_en
                    // FAULT_ADDR, FAULT_STATUS: read-only (hardware-latched)
                    default: ;  // ignore writes to read-only or unimplemented regs
                endcase
            end
        end
    end

    // ══════════════════════════════════════════════════════════
    // Sysreg read mux
    // ══════════════════════════════════════════════════════════

    logic [31:0] tlb_read_vpn, tlb_read_pte;

    always_comb begin
        case (i_sys_reg)
            SYSREG_MMU_CR:      o_sys_rdata = mmucr;
            SYSREG_MMU_FADDR:   o_sys_rdata = fault_addr;
            SYSREG_MMU_FSTAT:   o_sys_rdata = fault_status;
            SYSREG_MMU_TLB_VPN: o_sys_rdata = tlb_read_vpn;
            SYSREG_MMU_TLB_PTE: o_sys_rdata = tlb_read_pte;
            SYSREG_MMU_TLB_IDX: o_sys_rdata = tlb_index_reg;
            default:            o_sys_rdata = 32'b0;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // TLB instance
    // ══════════════════════════════════════════════════════════

    logic [31:0] tlb_paddr;
    logic        tlb_cacheable, tlb_hit, tlb_fault;
    logic [31:0] tlb_fault_status;

    tlb u_tlb (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        // Lookup
        .i_vaddr        (i_vaddr),
        .i_access_type  (i_access_type),
        .i_user_mode    (i_user_mode),
        .i_asid         (current_asid),
        .i_lookup_en    (mmu_enabled && i_req),
        .o_paddr        (tlb_paddr),
        .o_cacheable    (tlb_cacheable),
        .o_hit          (tlb_hit),
        .o_fault        (tlb_fault),
        .o_fault_status (tlb_fault_status),
        // Indexed access
        .i_idx_set      (idx_set),
        .i_idx_way      (idx_way),
        .i_write_vpn    (tlb_vpn_reg),
        .i_write_pte    (i_sys_wdata),
        .i_write_en     (tlb_write_en),
        .o_read_vpn     (tlb_read_vpn),
        .o_read_pte     (tlb_read_pte)
    );

    // ══════════════════════════════════════════════════════════
    // Translation output mux: bypass (M=0) vs TLB (M=1)
    // ══════════════════════════════════════════════════════════

    always_comb begin
        if (mmu_enabled) begin
            o_paddr     = tlb_paddr;
            o_cacheable = tlb_cacheable;
            o_hit       = tlb_hit;
            o_fault     = tlb_fault || (i_req && !tlb_hit);
        end else begin
            // Bypass: identity map, uncacheable, no faults
            o_paddr     = i_vaddr;
            o_cacheable = 1'b0;
            o_fault     = 1'b0;
            o_hit       = 1'b1;
        end
    end

endmodule

// verilator lint_on UNUSEDSIGNAL
