// penumbra3_direction_table -- the fetch-domain bimodal direction table.
//
// ENTRIES two-bit saturating counters in block RAM, indexed by the fetch
// PC's word address. IF1 launches the read with the fetch, so the counter
// for the word IF2 pre-decodes lands beside it; its high bit calls a
// conditional branch taken. Training writes the counter EX computed for a
// resolved conditional branch, registered off the fetch loop.
//
// The counters power up zero (strongly not-taken), as the block RAM
// configures; a table trains within a few executions of each branch.
//
// A training write suppresses that cycle's read, so the RTL states the two
// ports never meet and synthesis adds no bypass after the EBR. The read
// output then holds, and IF2 uses the previous fetch's counter: one
// prediction may be wrong, which EX catches like any other.
module penumbra3_direction_table #(
    parameter  int ENTRIES = 1024,
    localparam int INDEX_BITS = $clog2(ENTRIES)
) (
    input  logic                  i_clk,

    input  logic                  i_read_en,     // the fetch launches
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0]           i_fetch_pc,    // only the index bits select
    /* verilator lint_on UNUSEDSIGNAL */
    output logic [1:0]            o_counter,     // registered, beside IF2's word

    input  logic                  i_train_en,
    input  logic [INDEX_BITS-1:0] i_train_index,
    input  logic [1:0]            i_train_counter
);

    build_check #(
        .COND ((1 << INDEX_BITS) == ENTRIES),
        .MSG  ("penumbra3_direction_table: ENTRIES must be a power of two")
    ) u_check_entries ();

    (* ram_style = "block" *) logic [1:0] counter_mem [ENTRIES];

    always_ff @(posedge i_clk) begin
        if (i_read_en && !i_train_en)
            o_counter <= counter_mem[i_fetch_pc[2 +: INDEX_BITS]];
        if (i_train_en)
            counter_mem[i_train_index] <= i_train_counter;
    end

endmodule
