// penumbra3_scoreboard -- per-register pending-bit tracker.
//
// A flop per architectural register that can have a write in flight that is
// not yet forwardable: loads, divmul, and serializing ops set their
// destination's bit at issue; completion clears it. ALU results are
// bypassed and never set a bit (so dependent ALU chains never stall). The
// two source-operand read ports are combinational -- the registered bits
// feed the issue gate's hazard compare, which is the only consumer.
module penumbra3_scoreboard #(
    parameter int NREGS = 22
) (
    input  logic                     i_clk,
    input  logic                     i_rst,

    // Set on issue (destination), clear on completion. Two set ports: a
    // dual-destination op (divmul) sets both its primary (Rd) and aux (Rdh)
    // bits in the same issue cycle.
    input  logic                     i_set_en,
    input  logic [$clog2(NREGS)-1:0] i_set_idx,
    input  logic                     i_set2_en,
    input  logic [$clog2(NREGS)-1:0] i_set2_idx,
    input  logic                     i_clr_en,
    input  logic [$clog2(NREGS)-1:0] i_clr_idx,

    // Combinational read for the two source operands
    input  logic [$clog2(NREGS)-1:0] i_src0_idx,
    input  logic [$clog2(NREGS)-1:0] i_src1_idx,
    output logic                     o_src0_pending,
    output logic                     o_src1_pending
);

    localparam int IDX_BITS = $clog2(NREGS);

    // Sized to the full index space so a variable index can never read out
    // of range; entries past NREGS-1 stay 0 (never pending).
    logic [(1<<IDX_BITS)-1:0] pending_q;

    assign o_src0_pending = pending_q[i_src0_idx];
    assign o_src1_pending = pending_q[i_src1_idx];

    always_ff @(posedge i_clk) begin
        if (i_rst)
            pending_q <= '0;
        else begin
            // Clear before set: a register that both completes and is
            // re-issued the same cycle ends pending (the new producer wins).
            if (i_clr_en)  pending_q[i_clr_idx]  <= 1'b0;
            if (i_set_en)  pending_q[i_set_idx]  <= 1'b1;
            if (i_set2_en) pending_q[i_set2_idx] <= 1'b1;
        end
    end

endmodule
