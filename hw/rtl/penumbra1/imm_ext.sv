// Penumbra Immediate Extractor — 16-bit to 32-bit extension
//
// Takes the 16-bit immediate field from the IR and extends it to 32 bits
// based on the imm_mode control signal from the micro-word.
//
// Modes:
//   00 (zero-extend):    0x0000_XXXX  — used by LLI, INC, logical immediates
//   01 (sign-extend):    0xFFFF_XXXX or 0x0000_XXXX  — used by load/store offsets
//   10 (shift-left-16):  0xXXXX_0000  — used by LUI
//
// Purely combinational — no clock, no state.
//
// For C/C++ programmers:
//   Zero-extend:     (uint32_t)(uint16_t)imm
//   Sign-extend:     (int32_t)(int16_t)imm
//   Shift-left-16:   (uint32_t)imm << 16

module imm_ext (
    input  logic [15:0] i_imm16,      // 16-bit immediate from IR[15:0]
    input  logic [1:0]  i_mode,        // Extension mode from micro-word

    output logic [31:0] o_imm32        // 32-bit extended result → B-mux
);

    localparam logic [1:0] MODE_ZERO_EXT  = 2'b00;
    localparam logic [1:0] MODE_SIGN_EXT  = 2'b01;
    localparam logic [1:0] MODE_SHIFT_L16 = 2'b10;

    always_comb begin
        case (i_mode)
            MODE_ZERO_EXT:  o_imm32 = { 16'b0, i_imm16 };
            MODE_SIGN_EXT:  o_imm32 = { {16{i_imm16[15]}}, i_imm16 };
            MODE_SHIFT_L16: o_imm32 = { i_imm16, 16'b0 };
            default:        o_imm32 = 32'b0;
        endcase
    end

endmodule
