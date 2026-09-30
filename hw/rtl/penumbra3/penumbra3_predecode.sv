// penumbra3_predecode -- the IF2 control-transfer pre-decode.
//
// Pure combinational. Classifies a fetched word as the branch predictor
// needs it and computes a direct transfer's target from the word and its
// PC. IF2 runs one per cache way, on each way's word straight off the
// block RAM, so the classification and the target add run beside the tag
// compare and the hit only selects among finished results. The full
// word->bundle decode stays after the way mux.
//
// A direct transfer (B, BL, Bcc) carries its target; the prediction
// redirects on it when it is unconditional or the direction table calls it
// taken. A return (JMP R13) redirects to the return stack's top. BL and JALR
// push the return address; JALR's own target is a register, so it pushes
// without redirecting, as does every other JMP. EX verifies every
// prediction, so none of this has to be right, only cheap.
module penumbra3_predecode
    import penumbra_pkg::*;
(
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_word,         // the low spare bits classify nothing
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] i_pc,

    output logic        o_direct,       // B, BL or Bcc: the target is o_target
    output logic        o_conditional,  // Bcc: taken only as the direction table says
    output logic        o_call,         // BL or JALR: push PC + 4
    output logic        o_return,       // JMP R13: pop, redirect to the top
    output logic [31:0] o_target
);

    logic [1:0]  fmt;
    logic [3:0]  cond, l_op, l_rd;
    logic [21:0] offset22;
    assign fmt      = i_word[31:30];
    assign cond     = i_word[29:26];
    assign l_op     = i_word[29:26];
    assign l_rd     = i_word[25:22];
    assign offset22 = i_word[25:4];

    logic is_jmp, is_jalr;
    assign is_jmp  = (fmt == FMT_L) && (l_op == OP_L_JMP);
    assign is_jalr = (fmt == FMT_L) && (l_op == OP_L_JALR);

    assign o_direct      = (fmt == FMT_B);
    assign o_conditional = o_direct && (cond != COND_AL) && (cond != COND_BL);
    assign o_call        = (o_direct && (cond == COND_BL)) || is_jalr;
    assign o_return      = is_jmp && (l_rd == REG_LR);

    // The offset counts words from the branch itself.
    assign o_target = i_pc + {{8{offset22[21]}}, offset22, 2'b00};

endmodule
