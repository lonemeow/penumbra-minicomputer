// Penumbra Cache Stub — pass-through placeholder
//
// Sits between the MMU and memory. In the real design this becomes
// split I/D PIPT caches (direct-mapped, write-through D-cache).
// For now it's purely combinational — every access goes straight
// to the backing memory with zero added latency.
//
// The interface is designed so a real cache drops in without
// changing cpu_top wiring.

// verilator lint_off UNUSEDSIGNAL

module cache_stub
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── CPU side (post-MMU physical address) ───────────────
    input  logic [31:0] i_paddr,        // Physical address from MMU
    input  logic [31:0] i_wdata,        // Write data (from MDR)
    input  logic        i_we,           // Data write enable
    input  logic        i_re,           // Data read enable
    input  logic        i_cacheable,    // MMU C bit (ignored in stub)
    output logic [31:0] o_rdata,        // Read data to CPU
    output logic        o_busy,         // Stall signal to CPU

    // ── Memory / bus side ──────────────────────────────────
    output logic [31:0] o_mem_addr,     // Address to memory
    output logic [31:0] o_mem_wdata,    // Write data to memory
    output logic        o_mem_we,       // Write enable to memory
    output logic        o_mem_re,       // Read enable to memory
    input  logic [31:0] i_mem_rdata,    // Read data from memory
    input  logic        i_mem_busy      // Busy from memory
);

    // Pass-through — no caching, no buffering
    assign o_mem_addr  = i_paddr;
    assign o_mem_wdata = i_wdata;
    assign o_mem_we    = i_we;
    assign o_mem_re    = i_re;
    assign o_rdata     = i_mem_rdata;
    assign o_busy      = i_mem_busy;

endmodule

// verilator lint_on UNUSEDSIGNAL
