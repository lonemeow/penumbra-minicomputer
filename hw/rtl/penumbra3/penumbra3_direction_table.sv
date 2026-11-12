// penumbra3_direction_table -- the fetch-domain bimodal direction table.
//
// ENTRIES saturating counters (dir_counter_t) in block RAM, indexed by the
// fetch PC's word address. IF1 launches the read with the fetch, so the
// counter for the word IF2 pre-decodes lands beside it. Training writes the
// counter EX computed for a resolved conditional branch, registered off the
// fetch loop.
//
// The counters power up zero (strongly not-taken), as the block RAM
// configures; a table trains within a few executions of each branch.
//
// A training write never holds up the read. When the two meet at one entry
// -- a short loop's branch fetched again as its previous pass trains it --
// the read takes the counter being written, from registers beside the block
// RAM, so the branch is predicted with what it has just learned. The block
// RAM's own result for that read is never used, which is what lets
// no_rw_check spare it the read-old-data emulation yosys would otherwise
// build (a delayed write and a mux on its output); the bypass needs only the
// written counter and a registered compare of the two indices.
module penumbra3_direction_table
    import penumbra3_pkg::*;
#(
    parameter  int ENTRIES = 1024,
    localparam int INDEX_BITS = $clog2(ENTRIES)
) (
    input  logic                  i_clk,

    input  logic                  i_read_en,     // the fetch launches
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0]           i_fetch_pc,    // only the index bits select
    /* verilator lint_on UNUSEDSIGNAL */
    output dir_counter_t          o_counter,     // beside IF2's word

    input  logic                  i_train_en,
    input  logic [INDEX_BITS-1:0] i_train_index,
    input  dir_counter_t          i_train_counter
);

    build_check #(
        .COND ((1 << INDEX_BITS) == ENTRIES),
        .MSG  ("penumbra3_direction_table: ENTRIES must be a power of two")
    ) u_check_entries ();

    (* ram_style = "block", no_rw_check *) dir_counter_t counter_mem [ENTRIES];

    logic [INDEX_BITS-1:0] read_index;
    assign read_index = i_fetch_pc[2 +: INDEX_BITS];

    dir_counter_t ram_counter_q;
    logic         bypass_q;            // the read met the training write at its entry
    dir_counter_t bypass_counter_q;    // the counter that write stored
    always_ff @(posedge i_clk) begin
        if (i_read_en) begin
            ram_counter_q    <= counter_mem[read_index];
            bypass_q         <= i_train_en && i_train_index == read_index;
            bypass_counter_q <= i_train_counter;
        end
        if (i_train_en)
            counter_mem[i_train_index] <= i_train_counter;
    end
    assign o_counter = bypass_q ? bypass_counter_q : ram_counter_q;

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    // The invariant no_rw_check relies on: a read that met the training
    // write at its entry gives the written counter, never the block RAM's
    // own result.
    assert property (@(posedge i_clk)
        (i_read_en && i_train_en && i_train_index == read_index)
            |=> o_counter == $past(i_train_counter))
        else $error("penumbra3_direction_table: a read that met the write took the block RAM's result");

endmodule
