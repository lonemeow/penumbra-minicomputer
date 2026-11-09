// penumbra3_scoreboard -- whether a source operand still has a late writer in
// flight.
//
// A late writer is an instruction whose result the forward legs cannot
// carry to a reader issued behind it (penumbra3_pkg::late_result), or a
// divmul's second result, which no leg carries. Nothing is stored here: the
// caller lists each cycle, from the pipeline's registered state, every late
// writer in flight that has yet to write the regfile, and a source is
// pending while one of them writes its entry. So a chain of writers to one
// entry keeps it pending until the youngest has written it, an older
// writer's write never releases a younger one's readers, and a flushed
// writer stops counting once the flush has cleared its slot. The regfile's
// reads are write-first, so a writer leaves the list in the cycle it writes.
module penumbra3_scoreboard #(
    parameter int IDX_BITS = 5,
    parameter int WRITERS  = 8
) (
    input  logic [WRITERS-1:0]               i_wr_en,
    input  logic [WRITERS-1:0][IDX_BITS-1:0] i_wr_idx,

    input  logic [IDX_BITS-1:0]              i_src0_idx,
    input  logic [IDX_BITS-1:0]              i_src1_idx,
    output logic                             o_src0_pending,
    output logic                             o_src1_pending
);

    always_comb begin
        o_src0_pending = 1'b0;
        o_src1_pending = 1'b0;
        for (int w = 0; w < WRITERS; w++) begin
            if (i_wr_en[w] && i_wr_idx[w] == i_src0_idx) o_src0_pending = 1'b1;
            if (i_wr_en[w] && i_wr_idx[w] == i_src1_idx) o_src1_pending = 1'b1;
        end
    end

endmodule
