// penumbra3_return_stack -- the fetch-domain return-address ring.
//
// A plain ring of DEPTH return addresses. A push (a call enqueued at IF2)
// advances the top and writes the return address there, overwriting the
// oldest entry once the ring is full; a pop (a return enqueued at IF2)
// predicts the top slot and steps back. Nothing counts entries: an
// overflowed or desynced ring (longjmp, a hand-built return) keeps
// predicting, and a stale slot costs the same EX redirect a refusal would
// while sometimes being right. The measurements behind the shape are in
// doc/internals/penumbra3/branch-prediction.md.
//
// o_top comes off registers, so a return's target is ready early in IF2.
// A push and a pop in one cycle cannot happen: a slot is one instruction.
module penumbra3_return_stack #(
    parameter int DEPTH = 8
) (
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic        i_push,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] i_push_addr,    // word-aligned; bits [1:0] are not kept
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic        i_pop,
    output logic [31:0] o_top
);

    localparam int PTR_BITS = $clog2(DEPTH);

    build_check #(
        .COND ((1 << PTR_BITS) == DEPTH),
        .MSG  ("penumbra3_return_stack: DEPTH must be a power of two")
    ) u_check_depth ();

    // Return addresses are word-aligned; the slots keep bits [31:2].
    (* ram_style = "registers" *) logic [29:0] slot_q [DEPTH];
    logic [PTR_BITS-1:0] top_q, above_top, below_top;
    assign above_top = top_q + 1'b1;
    assign below_top = top_q - 1'b1;

    assign o_top = {slot_q[top_q], 2'b00};

    always_ff @(posedge i_clk) begin
        if (i_push)
            slot_q[above_top] <= i_push_addr[31:2];

        if (i_rst)       top_q <= '0;
        else if (i_push) top_q <= above_top;
        else if (i_pop)  top_q <= below_top;
    end

    always_comb
        assert (!(i_push && i_pop))
            else $error("penumbra3_return_stack: push and pop in one cycle");

endmodule
