// penumbra4_mmu — translates fetch and data addresses.
//
// A translation launched in one cycle gives its verdict in the next: the
// physical address, whether the page is cacheable, and the fault, if any.
// With translation off, as MMUCR resets it (doc/system/mmu.md), the
// physical address is the virtual one and every access is uncacheable
// (doc/internals/penumbra3/uncore-port.md, "The three transactions").
//
// Both sides translate with translation off: no sysreg reaches MMUCR to
// turn it on.

module penumbra4_mmu
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
(
    input  logic        i_clk,

    // ── The fetch side ───────────────────────────────────────────
    input  logic        i_fetch_launch,
    input  vaddr_t      i_fetch_vaddr,
    output paddr_t      o_fetch_paddr,
    output logic        o_fetch_cacheable,
    output fault_type_t o_fetch_fault,

    // ── The data side ────────────────────────────────────────────
    input  logic        i_data_launch,
    input  vaddr_t      i_data_vaddr,
    output paddr_t      o_data_paddr,
    output logic        o_data_cacheable,
    output fault_type_t o_data_fault
);

    paddr_t fetch_paddr_q;
    paddr_t data_paddr_q;

    always_ff @(posedge i_clk) begin
        if (i_fetch_launch) begin
            fetch_paddr_q <= paddr_t'(i_fetch_vaddr);
        end
    end

    always_ff @(posedge i_clk) begin
        if (i_data_launch) begin
            data_paddr_q <= paddr_t'(i_data_vaddr);
        end
    end

    assign o_fetch_paddr     = fetch_paddr_q;
    assign o_fetch_cacheable = 1'b0;
    assign o_fetch_fault     = FAULT_NONE;

    assign o_data_paddr      = data_paddr_q;
    assign o_data_cacheable  = 1'b0;
    assign o_data_fault      = FAULT_NONE;

endmodule
