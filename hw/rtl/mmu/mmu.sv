// Penumbra MMU — Memory Management Unit
//
// Orchestrates TLB lookup and bypass mode. When M=0 (reset default),
// identity maps all addresses as uncacheable. When M=1, routes
// translation through the TLB unit and reports misses/faults.
//
// TLB details (main + pinned) are encapsulated in tlb_unit.sv.
// This module handles: MMUCR, fault latching, alignment checks,
// bypass logic, and sysreg routing for registers 0-2.

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
    input  logic        i_force_bypass, // Override: identity map this request (vector fetch)
    input  logic [1:0]  i_mem_size,     // Access size: 00=byte, 01=half, 10=word
    input  logic        i_bus_fault,    // Bus fault (no device at physical address)

    // ── Translation output ─────────────────────────────────
    output logic [31:0] o_paddr,        // Physical address
    output logic        o_cacheable,    // PTE.C (0 in bypass mode)
    output logic        o_fault,        // Access violation / TLB miss / alignment
    output logic        o_hit,          // TLB hit (always 1 in bypass)
    output logic        o_align,        // Current fault is alignment (for vector select)

    // ── Sysreg interface (WRSYS/RDSYS, dev_id = 0) ────────
    input  logic [3:0]  i_sys_reg,      // Register address within MMU
    input  logic [31:0] i_sys_wdata,    // Write data
    input  logic        i_sys_we,       // Write enable
    output logic [31:0] o_sys_rdata     // Read data
);

    // ══════════════════════════════════════════════════════════
    // Control registers (regs 0-2)
    // ══════════════════════════════════════════════════════════

    // MMUCR: [0]=M (enable), [15:8]=ASID
    logic [31:0] mmucr;
    logic [31:0] fault_addr;
    logic [31:0] fault_status;

    logic        mmu_enabled;
    logic [7:0]  current_asid;

    assign mmu_enabled  = mmucr[0];
    assign current_asid = mmucr[15:8];

    // ══════════════════════════════════════════════════════════
    // TLB unit (main + pinned, sysregs 3-8)
    // ══════════════════════════════════════════════════════════

    logic        lookup_en;
    assign lookup_en = mmu_enabled && i_req && !i_force_bypass;

    logic [31:0] tlb_paddr, tlb_fault_status, tlb_rdata;
    logic        tlb_cacheable, tlb_hit, tlb_fault;

    tlb_unit u_tlb_unit (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_vaddr        (i_vaddr),
        .i_access_type  (i_access_type),
        .i_user_mode    (i_user_mode),
        .i_asid         (current_asid),
        .i_lookup_en    (lookup_en),
        .o_paddr        (tlb_paddr),
        .o_cacheable    (tlb_cacheable),
        .o_hit          (tlb_hit),
        .o_fault        (tlb_fault),
        .o_fault_status (tlb_fault_status),
        .i_sys_reg      (i_sys_reg),
        .i_sys_wdata    (i_sys_wdata),
        .i_sys_we       (i_sys_we),
        .o_sys_rdata    (tlb_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Sysreg write logic (regs 0-2 only; 3-8 handled by tlb_unit)
    // ══════════════════════════════════════════════════════════

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            mmucr        <= 32'b0;
            fault_addr   <= 32'b0;
            fault_status <= 32'b0;
        end else begin
            // Latch fault info — alignment > TLB prot > TLB miss > bus fault.
            // Gated by !i_force_bypass so vector fetches don't overwrite
            // fault info from the original exception.
            if (i_req && !i_force_bypass && misaligned) begin
                fault_addr   <= i_vaddr;
                fault_status <= {20'b0, i_user_mode, i_access_type, 4'b0, FAULT_ALIGN};
            end else if (mmu_enabled && i_req && !i_force_bypass && tlb_fault) begin
                fault_addr   <= i_vaddr;
                fault_status <= tlb_fault_status;
            end else if (mmu_enabled && i_req && !i_force_bypass && !tlb_hit) begin
                fault_addr   <= i_vaddr;
                fault_status <= {20'b0, i_user_mode, i_access_type, 4'b0, FAULT_TLB_MISS};
            end else if (i_bus_fault && !i_force_bypass) begin
                fault_addr   <= i_vaddr;
                fault_status <= {20'b0, i_user_mode, i_access_type, 4'b0, FAULT_BUS};
            end

            // MMUCR write
            if (i_sys_we && i_sys_reg == SYSREG_MMU_CR)
                mmucr <= i_sys_wdata;
        end
    end

    // ══════════════════════════════════════════════════════════
    // Sysreg read mux — regs 0-2 from here, 3-8 from tlb_unit
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
    // Alignment check — fires regardless of MMU enable/bypass.
    // ══════════════════════════════════════════════════════════

    logic misaligned;
    always_comb begin
        case (i_mem_size)
            2'b10:   misaligned = (i_vaddr[1:0] != 2'b00);  // word
            2'b01:   misaligned = i_vaddr[0];                // half
            default: misaligned = 1'b0;                      // byte: always OK
        endcase
    end

    assign o_align = i_req && misaligned;

    // ══════════════════════════════════════════════════════════
    // Translation output mux: alignment > TLB > bypass
    // ══════════════════════════════════════════════════════════

    always_comb begin
        if (i_req && misaligned) begin
            o_paddr     = i_vaddr;
            o_cacheable = 1'b0;
            o_fault     = 1'b1;
            o_hit       = 1'b0;
        end else if (mmu_enabled && !i_force_bypass) begin
            o_paddr     = tlb_paddr;
            o_cacheable = tlb_cacheable;
            o_hit       = tlb_hit;
            o_fault     = tlb_fault || (i_req && !tlb_hit);
        end else begin
            o_paddr     = i_vaddr;
            o_cacheable = 1'b0;
            o_fault     = 1'b0;
            o_hit       = 1'b1;
        end
    end

endmodule

// verilator lint_on UNUSEDSIGNAL
