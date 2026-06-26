// penumbra3_issue -- the ID issue gate (hazard interlock).
//
// Decides whether the instruction in ID may issue this cycle, from
// registered inputs only: the source operands' scoreboard pending bits, what
// the forwarding network can supply this cycle, and the back-end hold. This
// is reg#-equality on already-registered fields -- the shallow compare every
// in-order core uses, never a long path -- and the cone P0.1 measures (P0.2
// shares it). It reads only flops: no cache busy/hit reaches here, which is
// what keeps the gen2 floor cone from ever forming.
module penumbra3_issue (
    input  logic i_valid,         // an instruction is present in ID
    input  logic i_pipe_hold,     // back-end back-pressure (registered: load_pending, ...)

    // Source 0
    input  logic i_src0_used,     // the instruction reads source 0
    input  logic i_src0_pending,  // source 0's scoreboard bit (flop)
    input  logic i_src0_fwd,      // source 0 forwardable this cycle

    // Source 1
    input  logic i_src1_used,
    input  logic i_src1_pending,
    input  logic i_src1_fwd,

    output logic o_can_issue
);

    // A source is blocked when it is read, its scoreboard bit is pending,
    // and it cannot be forwarded this cycle.
    logic src0_blocked, src1_blocked;
    assign src0_blocked = i_src0_used && i_src0_pending && !i_src0_fwd;
    assign src1_blocked = i_src1_used && i_src1_pending && !i_src1_fwd;

    // Issue when valid, the pipe is not holding, and neither used source is
    // blocked. Flat AND over flop inputs -- this is the critical cone tail.
    assign o_can_issue = i_valid && !i_pipe_hold && !src0_blocked && !src1_blocked;

endmodule
