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

endpackage
/* verilator lint_on UNUSEDPARAM */
