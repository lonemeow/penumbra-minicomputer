// Penumbra System ID — read-only CPU and machine identification registers
//
// Sysreg device 1 (SYSDEV_SYS). Provides hardwired constants that
// software can read via RDSYS to identify the CPU core, its capabilities,
// and the machine/board it is running on.
//
// Register map:
//   0      CPU_ISA      — ISA version (bits 3:0) + feature flags (bits 31:4)
//   1      MACH_FEAT    — Machine feature flags
//   2–5    CPU_NAME0–3  — CPU name string, 16 bytes packed LE, null-padded
//   6–9    MACH_NAME0–3 — Machine name string, 16 bytes packed LE, null-padded
//   10–15  Reserved (reads as 0)
//
// Name strings are little-endian: first character in bits [7:0] of NAME0,
// second in [15:8], etc. Software reads regs 2–5 (or 6–9) sequentially
// and extracts bytes to build the printable string.
//
// Purely combinational — no clock, no state. On 74xx discrete this
// is literally pull-up/pull-down resistors on the sysreg data bus.
//
// Parameters allow different machines to customise identification
// while sharing the same CPU core.

module sysid
    import penumbra_pkg::*;
#(
    // ── CPU identification ──────────────────────────────────
    parameter logic [3:0]   CPU_ISA_VERSION = 4'd1,     // ISA v1
    parameter logic [27:0]  CPU_FEATURES    = 28'd0,    // No optional features yet

    // CPU name: 16 bytes packed LE into 4 words, null-padded.
    // Default: "Penumbra/1" (10 chars + 6 null bytes)
    //   "Penu" = 0x756E6550   "mbra" = 0x6172626D
    //   "/1\0\0" = 0x0000312F
    parameter logic [31:0]  CPU_NAME0 = 32'h756E6550,   // "Penu"
    parameter logic [31:0]  CPU_NAME1 = 32'h6172626D,   // "mbra"
    parameter logic [31:0]  CPU_NAME2 = 32'h0000312F,   // "/1\0\0"
    parameter logic [31:0]  CPU_NAME3 = 32'h00000000,   // "\0\0\0\0"

    // ── Machine identification ──────────────────────────────
    parameter logic [31:0]  MACH_FEAT_VALUE = 32'd0,

    // Machine name: 16 bytes packed LE, null-padded.
    // Default: all zeros (unnamed machine). Overridden per-board.
    parameter logic [31:0]  MACH_NAME0 = 32'h00000000,
    parameter logic [31:0]  MACH_NAME1 = 32'h00000000,
    parameter logic [31:0]  MACH_NAME2 = 32'h00000000,
    parameter logic [31:0]  MACH_NAME3 = 32'h00000000
)(
    input  logic [3:0]  i_sys_reg,
    output logic [31:0] o_sys_rdata
);

    // ── CPU_ISA register: [3:0] = version, [31:4] = feature flags ──
    localparam logic [31:0] CPU_ISA_VALUE = {CPU_FEATURES, CPU_ISA_VERSION};

    // ── Read mux ────────────────────────────────────────────
    always_comb begin
        case (i_sys_reg)
            SYSREG_SYS_CPU_ISA:    o_sys_rdata = CPU_ISA_VALUE;
            SYSREG_SYS_MACH_FEAT:  o_sys_rdata = MACH_FEAT_VALUE;
            SYSREG_SYS_CPU_NAME0:  o_sys_rdata = CPU_NAME0;
            SYSREG_SYS_CPU_NAME1:  o_sys_rdata = CPU_NAME1;
            SYSREG_SYS_CPU_NAME2:  o_sys_rdata = CPU_NAME2;
            SYSREG_SYS_CPU_NAME3:  o_sys_rdata = CPU_NAME3;
            SYSREG_SYS_MACH_NAME0: o_sys_rdata = MACH_NAME0;
            SYSREG_SYS_MACH_NAME1: o_sys_rdata = MACH_NAME1;
            SYSREG_SYS_MACH_NAME2: o_sys_rdata = MACH_NAME2;
            SYSREG_SYS_MACH_NAME3: o_sys_rdata = MACH_NAME3;
            default:                o_sys_rdata = 32'b0;
        endcase
    end

endmodule
