// Unit-test wrapper for the gen3 L1 lookup: penumbra3_l1_store's registered
// set feeding penumbra3_l1_match, as each L1 composes them. The testbench
// launches a lookup, then drives the frame candidates in the resolve cycle
// as the MMU would, and drives fills and store writes directly.
module penumbra3_l1_lookup_test (
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic        i_launch,
    input  logic [7:0]  i_set,
    input  logic [1:0]  i_word,

    input  logic [2:0][19:0] i_frame,
    input  logic [2:0]       i_frame_sel,

    input  logic        i_fill_we,
    input  logic [7:0]  i_fill_set,
    input  logic        i_fill_way,
    input  logic [19:0] i_fill_tag,
    input  logic [3:0][31:0] i_fill_line,

    input  logic        i_store_we,
    input  logic [7:0]  i_store_set,
    input  logic [1:0]  i_store_word,
    input  logic        i_store_way,
    input  logic [31:0] i_store_wdata,
    input  logic [3:0]  i_store_byte_en,

    input  logic        i_inval_all,

    output logic [1:0]  o_way_hit,
    output logic        o_hit,
    output logic [31:0] o_word
);

    logic [1:0]        valid;
    logic [1:0][19:0]  tag;
    logic [1:0][31:0]  word;

    penumbra3_l1_store #(
        .WAYS       (2),
        .LINE_BYTES (16)
    ) u_store (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_launch        (i_launch),
        .i_set           (i_set),
        .i_word          (i_word),
        .o_rd_valid      (valid),
        .o_rd_tag        (tag),
        .o_rd_word       (word),
        .i_fill_we       (i_fill_we),
        .i_fill_set      (i_fill_set),
        .i_fill_way      (i_fill_way),
        .i_fill_tag      (i_fill_tag),
        .i_fill_line     (i_fill_line),
        .i_store_we      (i_store_we),
        .i_store_set     (i_store_set),
        .i_store_word    (i_store_word),
        .i_store_way     (i_store_way),
        .i_store_wdata   (i_store_wdata),
        .i_store_byte_en (i_store_byte_en),
        .i_inval_all     (i_inval_all)
    );

    penumbra3_l1_match #(
        .WAYS   (2),
        .FRAMES (3)
    ) u_match (
        .i_valid     (valid),
        .i_tag       (tag),
        .i_word      (word),
        .i_frame     (i_frame),
        .i_frame_sel (i_frame_sel),
        .o_way_hit   (o_way_hit),
        .o_hit       (o_hit),
        .o_word      (o_word)
    );

endmodule
