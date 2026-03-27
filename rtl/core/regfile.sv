// Penumbra Register File — 16-register, 2-read, 1-write, 32-bit
//
// Architecture:
//   R0:      Hardwired to zero (reads always return 0, writes are ignored)
//   R1–R13:  General-purpose registers
//   R14:     Stack pointer, banked: USP (user) or KSP (supervisor)
//            Selected by i_supervisor — when SR.S=1, R14 reads/writes KSP
//   R15:     Reads return i_pc (from the separate PC register)
//            R15 is not stored here — it's a read-only alias
//
// Ports:
//   Two combinational read ports (A, B) — active all the time, no clock needed
//   One synchronous write port — latches data on rising clock edge
//
// For C/C++ programmers:
//   Think of this as: uint32_t regs[16] with special cases for indices 0, 14, 15.
//   Reads are like: return (addr == 0) ? 0 : (addr == 15) ? pc : regs[addr];
//   Writes are like: if (addr != 0 && clk_edge) regs[addr] = data;
//   The "two read ports" means two independent lookups happening simultaneously.

module regfile
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // Read port A (active-low — i.e., always valid, no enable needed)
    input  logic [3:0]  i_rd_addr_a,   // 4-bit register address (0-15)
    output logic [31:0] o_rd_data_a,   // Read data for port A → A-bus

    // Read port B
    input  logic [3:0]  i_rd_addr_b,   // 4-bit register address (0-15)
    output logic [31:0] o_rd_data_b,   // Read data for port B → B-mux

    // Write port (synchronous — write happens on rising clock edge)
    input  logic [3:0]  i_wr_addr,     // 4-bit register address (0-15)
    input  logic [31:0] i_wr_data,     // Data to write (from W-mux)
    input  logic        i_wr_en,       // Write enable (from micro-word, F-bit gated)

    // Special inputs
    input  logic [31:0] i_pc,          // PC value — returned when reading R15
    input  logic        i_supervisor,  // SR.S bit — selects KSP (1) vs USP (0) for R14

    // Debug read port (active all the time, no side effects)
    input  logic [3:0]  i_dbg_addr,    // Debug register address
    output logic [31:0] o_dbg_data     // Debug register value
);

    // ── Storage ─────────────────────────────────────────────────
    // R0 is not stored (always reads as 0).
    // R1–R13: 13 general-purpose registers.
    // R14 is two physical registers: USP and KSP.
    // R15 is not stored (reads return i_pc).
    //
    // In SystemVerilog, `logic [31:0] regs [1:13]` declares an array
    // of 13 × 32-bit registers, indexed 1 through 13.
    // This is like: uint32_t regs[14]; // but we only use indices 1-13

    logic [31:0] regs [1:13];
    logic [31:0] usp;          // User stack pointer (R14 when SR.S=0)
    logic [31:0] ksp;          // Kernel stack pointer (R14 when SR.S=1)

    // ── Read logic (combinational) ──────────────────────────────
    // Both read ports work identically — they're just two independent
    // multiplexers selecting from the same set of registers.
    //
    // This function reads one register given its 4-bit address.
    // `function` in SystemVerilog is like a C inline function —
    // it's purely combinational logic, synthesized as a mux tree.
    //
    // The read priority is:
    //   addr == 0  → return 0           (R0 hardwired zero)
    //   addr == 15 → return i_pc        (R15 is the PC)
    //   addr == 14 → return ksp or usp  (banked by supervisor mode)
    //   else       → return regs[addr]  (general-purpose R1-R13)

    function automatic logic [31:0] read_reg(input logic [3:0] addr);
        if (addr == REG_ZERO)
            read_reg = 32'd0;
        else if (addr == REG_SP)
            read_reg = i_supervisor ? ksp : usp;
        else if (addr == REG_PC)
            read_reg = i_pc;
        else
            read_reg = regs[addr];
    endfunction

    assign o_rd_data_a = read_reg(i_rd_addr_a);
    assign o_rd_data_b = read_reg(i_rd_addr_b);
    assign o_dbg_data  = read_reg(i_dbg_addr);

    // ── Write logic (synchronous) ───────────────────────────────
    // `always_ff @(posedge i_clk)` = "on every rising clock edge, do this"
    //
    // Writes to R0 and R15 are silently ignored (R0 is always 0, R15 is PC).
    // Writes to R14 go to either USP or KSP depending on i_supervisor.
    // All other writes go to regs[addr].

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            // Reset: zero all registers
            // In SystemVerilog, a for loop inside always_ff unrolls into
            // parallel reset logic — it's not sequential like C.
            for (int i = 1; i <= 13; i++) begin
                regs[i] <= 32'd0;
            end
            usp <= 32'd0;
            ksp <= 32'd0;
        end else if (i_wr_en) begin
            // Normal write — handle special addresses
            if (i_wr_addr == REG_ZERO || i_wr_addr == REG_PC) begin
                // Writes to R0 and R15 are silently discarded
            end else if (i_wr_addr == REG_SP) begin
                // R14 writes go to the active bank
                if (i_supervisor)
                    ksp <= i_wr_data;
                else
                    usp <= i_wr_data;
            end else begin
                regs[i_wr_addr] <= i_wr_data;
            end
        end
    end

endmodule
