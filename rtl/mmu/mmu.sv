// Penumbra MMU — Memory Management Unit
//
// Phase 1: Bypass mode only (M=0). Identity maps virtual to physical,
// forces uncacheable, no permission checks, no faults.
//
// Future: TLB-based translation when M=1 (64-entry 2-way SA TLB,
// software-managed, per-page R/W/X/U/C/D protection).
//
// Sysreg interface (dev_id=0) provides control registers accessible
// via MTSYS/MFSYS instructions.

// verilator lint_off UNUSEDSIGNAL

module mmu
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── CPU interface ──────────────────────────────────────
    input  logic [31:0] i_vaddr,        // Virtual address
    input  logic [1:0]  i_access_type,  // ACC_READ / ACC_WRITE / ACC_EXEC
    input  logic        i_user_mode,    // 1 = user mode (from !SR.S)
    input  logic        i_req,          // Translation request

    // ── Translation output ─────────────────────────────────
    output logic [31:0] o_paddr,        // Physical address
    output logic        o_cacheable,    // PTE.C (0 in bypass mode)
    output logic        o_fault,        // Access violation / not present
    output logic        o_hit,          // TLB hit (always 1 in bypass)

    // ── Sysreg interface (MTSYS/MFSYS, dev_id = 0) ────────
    input  logic [3:0]  i_sys_reg,      // Register address within MMU
    input  logic [31:0] i_sys_wdata,    // Write data
    input  logic        i_sys_we,       // Write enable
    output logic [31:0] o_sys_rdata     // Read data
);

    // ── Control registers ──────────────────────────────────
    // MMUCR[0] = M bit: 0 = bypass (identity map, uncached)
    //                    1 = translate (TLB lookup — future)
    logic [31:0] mmucr;
    logic [31:0] fault_addr;
    logic [31:0] fault_status;

    logic mmu_enabled;
    assign mmu_enabled = mmucr[0];

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            mmucr        <= 32'b0;
            fault_addr   <= 32'b0;
            fault_status <= 32'b0;
        end else if (i_sys_we) begin
            case (i_sys_reg)
                SYSREG_MMU_CR: mmucr <= i_sys_wdata;
                // fault_addr / fault_status are hardware-latched on faults,
                // not software-writable (OS reads them, MMU writes them)
            endcase
        end
    end

    always_comb begin
        case (i_sys_reg)
            SYSREG_MMU_CR:    o_sys_rdata = mmucr;
            SYSREG_MMU_FADDR: o_sys_rdata = fault_addr;
            SYSREG_MMU_FSTAT: o_sys_rdata = fault_status;
            default:          o_sys_rdata = 32'b0;
        endcase
    end

    always_comb begin
        if (mmu_enabled) begin
            // TODO: Implement this properly
            o_paddr = i_vaddr;
            o_cacheable = 1'b0;
            o_fault = 1'b0;
            o_hit = 1'b1;
        end else begin
            o_paddr = i_vaddr;
            o_cacheable = 1'b0;
            o_fault = 1'b0;
            o_hit = 1'b1;
        end
    end

endmodule

// verilator lint_on UNUSEDSIGNAL
