// Penumbra Condition Evaluator — branch condition check
//
// Purely combinational: takes 4 flag bits (Z, N, C, V) and a 4-bit
// condition code, outputs a single bit: 1 = condition met, 0 = not met.
//
// Used by the micro-sequencer's BRT/BRF to decide whether a branch is taken.
// In discrete hardware, this is a 16:1 multiplexer (74x150 or equivalent)
// fed by a few gates that compute the 16 possible condition results.
//
// For C/C++ programmers:
//   This is a pure function:  bool check(flags, cond_code)
//   Each condition is a simple boolean expression over the flags.
//   The case statement is just a 16-way switch.

module cond_eval
    import penumbra_pkg::*;
(
    input  logic i_flag_z,     // Zero flag
    input  logic i_flag_n,     // Negative flag
    input  logic i_flag_c,     // Carry flag
    input  logic i_flag_v,     // Overflow flag
    input  logic [3:0] i_cond, // Condition code (from IR branch field)

    output logic o_taken       // 1 = condition met, 0 = not met
);

    always_comb begin
        case (i_cond)
            COND_AL: o_taken = 1'b1;
            COND_EQ: o_taken = i_flag_z;
            COND_NE: o_taken = !i_flag_z;
            COND_CS: o_taken = i_flag_c;
            COND_CC: o_taken = !i_flag_c;
            COND_MI: o_taken = i_flag_n;
            COND_PL: o_taken = !i_flag_n;
            COND_VS: o_taken = i_flag_v;
            COND_VC: o_taken = !i_flag_v;
            COND_HI: o_taken = i_flag_c && !i_flag_z;
            COND_LS: o_taken = !i_flag_c || i_flag_z;
            COND_GE: o_taken = i_flag_n == i_flag_v;
            COND_LT: o_taken = i_flag_n != i_flag_v;
            COND_GT: o_taken = !i_flag_z && (i_flag_n == i_flag_v);
            COND_LE: o_taken = i_flag_z || (i_flag_n != i_flag_v);
            COND_BL: o_taken = 1'b1;
            default: o_taken = 1'b0;
        endcase
    end

endmodule
