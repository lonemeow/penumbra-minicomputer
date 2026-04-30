// Penumbra CPU identity — read-only CPU identification registers.
//
// Sysreg device 1 (SYSDEV_CPU). Hardwired constants describing the CPU
// core itself: ISA version, feature flags, and a 16-byte name string.
//
// Register map:
//   0      CPU_ISA      — ISA version (bits 3:0) + feature flags (bits 31:4)
//   1–4    CPU_NAME0–3  — CPU name string, 16 bytes packed LE, null-padded
//   5–15   Reserved (reads as 0; future home for CPU performance counters)
//
// Name strings are little-endian: first character in bits [7:0] of NAME0.
//
// Purely combinational — no clock, no state. On 74xx discrete this is
// literally pull-up/pull-down resistors on the sysreg data bus.
//
// Intentionally separate from machid: CPU identity is invariant across
// boards (same RTL → same answer), while machine identity (board name,
// PLL frequency) varies by instantiation.

module cpuid
    import penumbra_pkg::*;
#(
    // CPU identification
    parameter logic [3:0]   CPU_ISA_VERSION = 4'd1,     // ISA v1
    parameter logic [27:0]  CPU_FEATURES    = 28'd0,    // No optional features yet

    // CPU name: 16 bytes packed LE into 4 words, null-padded.
    // Default: "Penumbra/1" (10 chars + 6 null bytes)
    //   "Penu" = 0x756E6550   "mbra" = 0x6172626D
    //   "/1\0\0" = 0x0000312F
    parameter logic [31:0]  CPU_NAME0 = 32'h756E6550,   // "Penu"
    parameter logic [31:0]  CPU_NAME1 = 32'h6172626D,   // "mbra"
    parameter logic [31:0]  CPU_NAME2 = 32'h0000312F,   // "/1\0\0"
    parameter logic [31:0]  CPU_NAME3 = 32'h00000000    // "\0\0\0\0"
)(
    input  logic [3:0]  i_sys_reg,
    output logic [31:0] o_sys_rdata
);

    // CPU_ISA register: [3:0] = version, [31:4] = feature flags
    localparam logic [31:0] CPU_ISA_VALUE = {CPU_FEATURES, CPU_ISA_VERSION};

    always_comb begin
        case (i_sys_reg)
            SYSREG_CPU_ISA:    o_sys_rdata = CPU_ISA_VALUE;
            SYSREG_CPU_NAME0:  o_sys_rdata = CPU_NAME0;
            SYSREG_CPU_NAME1:  o_sys_rdata = CPU_NAME1;
            SYSREG_CPU_NAME2:  o_sys_rdata = CPU_NAME2;
            SYSREG_CPU_NAME3:  o_sys_rdata = CPU_NAME3;
            default:           o_sys_rdata = 32'b0;
        endcase
    end

endmodule
