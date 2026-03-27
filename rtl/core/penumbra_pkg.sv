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

endpackage
/* verilator lint_on UNUSEDPARAM */
