// Penumbra Field Extractor — decode IR into component fields
//
// Purely combinational: extracts all instruction fields from the 32-bit IR.
// The outputs are always valid — downstream logic selects which fields
// are meaningful based on the format (IR[31:30]).
//
// This is just wiring — no logic gates, no muxes, no decisions.
// In C terms: a bunch of bit-shifts and masks applied to a uint32_t.
// In discrete hardware: literally just wires from IR latch pins to
// the appropriate bus lines.
//
// Format encodings (IR[31:30]):
//   00 = Format R: register-register ALU and system ops
//   01 = Format L: immediate operations
//   10 = Format M: memory load/store
//   11 = Format B: branch

module field_ext (
    input  logic [31:0] i_ir,          // Instruction register

    // ── Common fields ───────────────────────────────────────────
    output logic [1:0]  o_format,      // IR[31:30] — instruction format

    // ── Format R fields ─────────────────────────────────────────
    output logic [4:0]  o_r_op,        // IR[29:25] — Format R opcode (5-bit)
    output logic [3:0]  o_r_rd,        // IR[24:21] — destination register
    output logic [3:0]  o_r_rs,        // IR[20:17] — source register
    output logic        o_r_f,         // IR[16]    — flag-only bit (suppresses write)
    output logic [3:0]  o_r_sys_dev,   // IR[15:12] — WRSYS/RDSYS device field
    output logic [3:0]  o_r_sys_reg,   // IR[11:8]  — WRSYS/RDSYS register field

    // ── Format L fields ─────────────────────────────────────────
    output logic [3:0]  o_l_op,        // IR[29:26] — Format L opcode (4-bit)
    output logic [3:0]  o_l_rd,        // IR[25:22] — destination register

    // ── Format M fields ─────────────────────────────────────────
    output logic        o_m_load,      // IR[29]    — 1=load, 0=store
    output logic [1:0]  o_m_size,      // IR[28:27] — 00=byte, 01=half, 10=word
    output logic        o_m_sign_ext,  // IR[26]    — sign-extend on load
    output logic [3:0]  o_m_rd,        // IR[25:22] — data register
    output logic [3:0]  o_m_rb,        // IR[21:18] — base register

    // ── Format B fields ─────────────────────────────────────────
    output logic [3:0]  o_b_cond,      // IR[29:26] — branch condition code

    // ── Immediate / offset fields ───────────────────────────────
    // These overlap in the IR but are named by their usage.
    output logic [15:0] o_imm16,       // IR[15:0]  — 16-bit immediate (Format L)
    output logic [15:0] o_m_offset16,  // IR[17:2]  — 16-bit signed offset (Format M)
    output logic [21:0] o_b_offset22   // IR[25:4]  — 22-bit signed offset (Format B)
);

    assign o_format     = i_ir[31:30];

    // Format R
    assign o_r_op       = i_ir[29:25];
    assign o_r_rd       = i_ir[24:21];
    assign o_r_rs       = i_ir[20:17];
    assign o_r_f        = i_ir[16];
    assign o_r_sys_dev  = i_ir[15:12];
    assign o_r_sys_reg  = i_ir[11:8];

    // Format L
    assign o_l_op       = i_ir[29:26];
    assign o_l_rd       = i_ir[25:22];

    // Format M
    assign o_m_load     = i_ir[29];
    assign o_m_size     = i_ir[28:27];
    assign o_m_sign_ext = i_ir[26];
    assign o_m_rd       = i_ir[25:22];
    assign o_m_rb       = i_ir[21:18];

    // Format B
    assign o_b_cond     = i_ir[29:26];

    // Immediates / offsets
    assign o_imm16      = i_ir[15:0];
    assign o_m_offset16 = i_ir[17:2];
    assign o_b_offset22 = i_ir[25:4];

endmodule
