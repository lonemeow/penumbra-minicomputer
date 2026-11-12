// Unit-test wrapper for penumbra3_branch_predictor.
//
// The predictor takes the enqueued slot as a packed ctrl_bundle_t and gives
// its prediction as a packed prediction_t, both opaque to a C++ testbench.
// This wrapper decodes a raw instruction word into the bundle, as IF2's
// enqueue does, and fans the prediction out to flat fields. The two L1-I ways'
// words arrive as two ports. Everything else passes straight through.
module penumbra3_branch_predictor_test
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic        i_fetch_en,
    input  logic [31:0] i_fetch_addr,

    input  logic [31:0] i_slot_pc,
    input  logic [1:0]  i_way_hit,
    input  logic [31:0] i_way0_word,
    input  logic [31:0] i_way1_word,

    input  logic        i_enq_fire,
    input  logic        i_enq_fault,
    input  logic [31:0] i_enq_word,
    input  logic [31:0] i_enq_next_pc,

    output logic        o_pred_taken,
    output logic [31:0] o_pred_target,
    output dir_counter_t o_pred_counter,

    input  logic        i_train,
    input  logic [31:0] i_train_pc,
    input  dir_counter_t i_train_counter
);

    ctrl_bundle_t enq_bundle;
    penumbra3_decode u_decode (
        .i_ir     (i_enq_word),
        .o_bundle (enq_bundle)
    );

    prediction_t prediction;
    assign o_pred_taken   = prediction.taken;
    assign o_pred_target  = prediction.target;
    assign o_pred_counter = prediction.counter;

    penumbra3_branch_predictor #(
        .WAYS        (2),
        .DIR_ENTRIES (1024),
        .RAS_DEPTH   (8)
    ) u_predictor (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_fetch_en      (i_fetch_en),
        .i_fetch_addr    (i_fetch_addr),
        .i_slot_pc       (i_slot_pc),
        .i_way_hit       (i_way_hit),
        .i_way_word      ({i_way1_word, i_way0_word}),
        .i_enq_fire      (i_enq_fire),
        .i_enq_fault     (i_enq_fault),
        .i_enq_bundle    (enq_bundle),
        .i_enq_next_pc   (i_enq_next_pc),
        .o_prediction    (prediction),
        .i_train         (i_train),
        .i_train_pc      (i_train_pc),
        .i_train_counter (i_train_counter)
    );

endmodule
