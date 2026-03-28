// Penumbra shared constants package
//
// In SystemVerilog, a `package` is like a C header file — it defines
// constants, types, and enums that multiple modules can import.
//
// Usage in a module:
//   import penumbra_pkg::*;    // like C's: using namespace penumbra;
//   Then use REG_ZERO, REG_SP, etc. directly.

/* verilator lint_off UNUSEDPARAM */
package penumbra_pkg;

    // ── Register addresses ──────────────────────────────────────
    localparam logic [3:0] REG_ZERO = 4'd0;   // R0:  hardwired zero
    localparam logic [3:0] REG_LR   = 4'd13;  // R13: link register (by convention)
    localparam logic [3:0] REG_SP   = 4'd14;  // R14: stack pointer (USP/KSP banked)
    localparam logic [3:0] REG_PC   = 4'd15;  // R15: program counter (read-only alias)

    // ── ALU operations ──────────────────────────────────────────
    localparam logic [4:0] ALU_ADD    = 5'b00000;
    localparam logic [4:0] ALU_SUB    = 5'b00001;
    localparam logic [4:0] ALU_AND    = 5'b00010;
    localparam logic [4:0] ALU_OR     = 5'b00011;
    localparam logic [4:0] ALU_XOR    = 5'b00100;
    localparam logic [4:0] ALU_SHL    = 5'b00101;
    localparam logic [4:0] ALU_SHR    = 5'b00110;
    localparam logic [4:0] ALU_SAR    = 5'b00111;
    localparam logic [4:0] ALU_PASS_A = 5'b01000;
    localparam logic [4:0] ALU_PASS_B = 5'b01001;
    localparam logic [4:0] ALU_NOT    = 5'b01010;
    localparam logic [4:0] ALU_MUL    = 5'b01011;
    localparam logic [4:0] ALU_MULU   = 5'b01100;
    localparam logic [4:0] ALU_DIV    = 5'b01101;
    localparam logic [4:0] ALU_DIVU   = 5'b01110;
    localparam logic [4:0] ALU_MOD    = 5'b01111;
    localparam logic [4:0] ALU_MODU   = 5'b10000;

    // ── Status register bit positions ───────────────────────────
    // Condition flags in [3:0], system bits in [31:30].
    // Bits [29:4] are reserved (read as zero, ignored on write).
    localparam logic [4:0] SR_N = 5'd0;   // Negative flag
    localparam logic [4:0] SR_Z = 5'd1;   // Zero flag
    localparam logic [4:0] SR_C = 5'd2;   // Carry flag
    localparam logic [4:0] SR_V = 5'd3;   // Overflow flag
    localparam logic [4:0] SR_I = 5'd30;  // Interrupt enable (1=enabled)
    localparam logic [4:0] SR_S = 5'd31;  // Supervisor mode (1=supervisor)

    // ── Condition codes (from Format B branch instructions) ─────
    localparam logic [3:0] COND_AL = 4'b0000;  // Always
    localparam logic [3:0] COND_EQ = 4'b0001;  // Equal            (Z=1)
    localparam logic [3:0] COND_NE = 4'b0010;  // Not equal        (Z=0)
    localparam logic [3:0] COND_CS = 4'b0011;  // Carry set / HS   (C=1)
    localparam logic [3:0] COND_CC = 4'b0100;  // Carry clear / LO (C=0)
    localparam logic [3:0] COND_MI = 4'b0101;  // Minus / negative  (N=1)
    localparam logic [3:0] COND_PL = 4'b0110;  // Plus / positive   (N=0)
    localparam logic [3:0] COND_VS = 4'b0111;  // Overflow set      (V=1)
    localparam logic [3:0] COND_VC = 4'b1000;  // Overflow clear    (V=0)
    localparam logic [3:0] COND_HI = 4'b1001;  // Unsigned higher   (C=1 & Z=0)
    localparam logic [3:0] COND_LS = 4'b1010;  // Unsigned lower/same (C=0 | Z=1)
    localparam logic [3:0] COND_GE = 4'b1011;  // Signed >=         (N=V)
    localparam logic [3:0] COND_LT = 4'b1100;  // Signed <          (N!=V)
    localparam logic [3:0] COND_GT = 4'b1101;  // Signed >          (Z=0 & N=V)
    localparam logic [3:0] COND_LE = 4'b1110;  // Signed <=         (Z=1 | N!=V)
    localparam logic [3:0] COND_BL = 4'b1111;  // Branch-and-link   (always, + save LR)

    // ── MMU access types (one-hot, matches R/W/X flag positions) ──
    localparam logic [2:0] ACC_READ  = 3'b001;  // bit 0 = R
    localparam logic [2:0] ACC_WRITE = 3'b010;  // bit 1 = W
    localparam logic [2:0] ACC_EXEC  = 3'b100;  // bit 2 = X

    // ── Sysreg device IDs ──────────────────────────────────────
    localparam logic [3:0] SYSDEV_MMU = 4'd0;   // MMU (TLB, fault regs)
    localparam logic [3:0] SYSDEV_SYS = 4'd1;   // System ID (read-only)

    // ── SYS sysreg addresses (dev_id = 1) ───────────────────
    localparam logic [3:0] SYSREG_SYS_MACHID = 4'd0;  // Machine ID (read-only)

    // ── MMU sysreg addresses (dev_id = 0) ─────────────────────
    localparam logic [3:0] SYSREG_MMU_CR       = 4'd0;  // MMUCR: [0]=M (enable), [15:8]=ASID
    localparam logic [3:0] SYSREG_MMU_FADDR    = 4'd1;  // Faulting virtual address (read-only)
    localparam logic [3:0] SYSREG_MMU_FSTAT    = 4'd2;  // Fault status (read-only)
    localparam logic [3:0] SYSREG_MMU_TLB_VPN  = 4'd3;  // TLB upper: {4'b0, VPN[19:0], ASID[7:0]}
    localparam logic [3:0] SYSREG_MMU_TLB_PTE  = 4'd4;  // TLB lower: {PPN[19:0], SW[3:0], flags[7:0]}
    localparam logic [3:0] SYSREG_MMU_TLB_IDX  = 4'd5;  // TLB slot: {26'b0, way[0], set[4:0]}

    // ── Exception vector numbers ────────────────────────────────
    // Vector address = {26'b0, vector_num, 2'b00} (word-aligned table at 0x00)
    localparam logic [3:0] VEC_RESET     = 4'd0;   // 0x00 — Reset
    localparam logic [3:0] VEC_IRQ       = 4'd1;   // 0x04 — External interrupt
    localparam logic [3:0] VEC_TLB_MISS  = 4'd2;   // 0x08 — TLB miss (no matching entry)
    localparam logic [3:0] VEC_TLB_PROT  = 4'd3;   // 0x0C — TLB protection fault
    localparam logic [3:0] VEC_PRIV      = 4'd4;   // 0x10 — Privilege violation (future)
    localparam logic [3:0] VEC_SYSCALL   = 4'd5;   // 0x14 — SYSCALL (future)
    localparam logic [3:0] VEC_BREAK     = 4'd6;   // 0x18 — BREAK (software breakpoint)

    // ── MMU fault status encoding ─────────────────────────────
    // FAULT_STATUS[3:0] = fault type
    localparam logic [3:0] FAULT_TLB_MISS = 4'b0001;
    localparam logic [3:0] FAULT_PROT     = 4'b0010;
    // FAULT_STATUS[7:4] = reserved (gap for future fault types)
    // FAULT_STATUS[11:8] = faulting access info
    localparam int FSTAT_R   = 8;   // Faulting access was read
    localparam int FSTAT_W   = 9;   // Faulting access was write
    localparam int FSTAT_X   = 10;  // Faulting access was execute
    localparam int FSTAT_USR = 11;  // Faulting access was user mode

    // ── TLB entry bit positions (64-bit entry) ────────────────
    // Upper word (TLB_VPN sysreg): {4'b0, VPN[19:0], ASID[7:0]}
    // Lower word (TLB_PTE sysreg): {PPN[19:0], SW[7:0], G, U, X, W, R, C, rsvd, V}
    localparam int TLB_V   = 0;   // Valid
    localparam int TLB_C   = 2;   // Cacheable
    localparam int TLB_R   = 3;   // Read permission
    localparam int TLB_W   = 4;   // Write permission
    localparam int TLB_X   = 5;   // Execute permission
    localparam int TLB_U   = 6;   // User-accessible
    localparam int TLB_G   = 7;   // Global (skip ASID match)

endpackage
/* verilator lint_on UNUSEDPARAM */
