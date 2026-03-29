// Penumbra PC Unit — program counter register, adder, and EPC latch
//
// The PC is a separate hardware register with its own adder, independent
// of the main ALU. It provides:
//   - PC register: 32-bit, updated on clock edge when i_pc_load is asserted
//   - PC+4 output: for sequential fetch and BL return address (→ regfile)
//   - PC+offset output: for branch targets (PC + sign_extend(offset22 << 2))
//   - Exception PC (EPC): latched at exception entry, readable on A-bus via a_src=10
//
// The pc_mux (separate module) selects which value loads into the PC register.
// This module just provides the register, adder outputs, and EPC latch.
//
// On FPGA, the two additions (PC+4 and PC+4+offset) are independent adders —
// cheap in LUTs. A discrete build would share one adder with a B-input mux
// and compute PC+4 first, then add offset in a second phase.

module pc_reg
    import penumbra_pkg::*;
#(
    parameter logic [31:0] RESET_PC = 32'hFFFF_E000  // Boot ROM base
)
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── PC load control ──────────────────────────────────────
    input  logic        i_pc_load,      // Load PC from i_pc_next on clock edge
    input  logic [31:0] i_pc_next,      // Next PC value (from pc_mux)

    // ── Branch offset input ──────────────────────────────────
    input  logic [21:0] i_offset22,     // Raw 22-bit offset from field extractor
                                        // Branch target = PC + 4 + sign_extend(offset22 << 2)

    // ── Exception entry ──────────────────────────────────────
    input  logic        i_except_entry, // Pulse: snapshot PC into EPC

    // ── Outputs ──────────────────────────────────────────────
    output logic [31:0] o_pc,           // Current PC value (→ I-cache, R15 read mux)
    output logic [31:0] o_pc_plus4,     // PC + 4 (→ pc_mux input, BL return addr)
    output logic [31:0] o_pc_offset,    // PC + 4 + sign_extend(offset22 << 2) (→ pc_mux)
    output logic [31:0] o_epc           // Exception PC (EPC) — saved at exception entry (→ A-bus)
);

    // ── PC register ──────────────────────────────────────────
    logic [31:0] pc;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            pc <= RESET_PC;
        else if (i_pc_load)
            pc <= i_pc_next;
    end

    assign o_pc = pc;

    // ── PC + 4 (sequential next) ─────────────────────────────
    assign o_pc_plus4 = pc + 32'd4;

    // ── PC + sign_extend(offset22 << 2) (branch target) ────
    // The offset22 field is a signed word offset. Shifting left by 2
    // converts to a byte offset, then sign-extending to 32 bits.
    // The offset is relative to the branch instruction itself (not PC+4).
    // The assembler encodes: offset22 = (target - PC) >> 2.
    logic [31:0] offset_extended;
    assign offset_extended = {{8{i_offset22[21]}}, i_offset22, 2'b00};
    assign o_pc_offset = pc + offset_extended;

    // ── Exception PC (EPC) — saved at exception entry ────────
    logic [31:0] epc;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            epc <= 32'b0;
        else if (i_except_entry)
            epc <= pc;
    end

    assign o_epc = epc;

endmodule
