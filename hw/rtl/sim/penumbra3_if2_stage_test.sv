// Unit-test wrapper for penumbra3_if2_stage.
//
// IF2 outputs the enqueue payload partly as a packed ctrl_bundle_t, which the
// C++ testbench would otherwise see as one opaque vector. This wrapper
// passes the flat enqueue/handshake/fault ports straight through and exposes
// one bundle field -- op_class -- so the testbench can confirm *which* word was
// enqueued (way 0's, way 1's or the delivered one) using inputs that decode to
// distinct classes. The full bundle is covered by the decode test; IF2's job
// is the select, the handshake and the fault composition.
module penumbra3_if2_stage_test
    import penumbra3_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic [31:0] i_pc,
    input  logic [31:0] i_next_pc,
    input  logic        i_valid,

    input  logic [1:0]       i_way_hit,
    input  logic [1:0][31:0] i_way_word,
    input  logic             i_hit,

    input  logic        i_cacheable,
    input  logic        i_user_mode,
    input  logic        i_mmu_fault,
    input  logic [31:0] i_mmu_fault_status,

    input  logic        i_replayed,
    input  logic        i_deliver,
    input  logic [31:0] i_deliver_word,
    input  logic        i_deliver_fault,
    output logic        o_miss,

    input  logic        i_flush,

    input  logic        i_enq_ready,
    output logic        o_enq_valid,
    output logic        o_enq_fire,
    output logic [3:0]  o_enq_op_class,
    output logic [31:0] o_enq_pc,
    output logic [31:0] o_enq_next_pc,
    output logic        o_enq_fault_pending,
    output logic [3:0]  o_enq_fault_vec,
    output logic [31:0] o_enq_fault_status,

    output logic        o_count_read,
    output logic        o_hold
);

    // Only op_class is exposed below; the full bundle is covered by the decode
    // test, so its other fields are intentionally unread here.
    /* verilator lint_off UNUSEDSIGNAL */
    ctrl_bundle_t bundle;
    /* verilator lint_on UNUSEDSIGNAL */

    penumbra3_if2_stage u_if2 (
        .i_clk               (i_clk),
        .i_rst               (i_rst),
        .i_pc                (i_pc),
        .i_next_pc           (i_next_pc),
        .i_valid             (i_valid),
        .i_way_hit           (i_way_hit),
        .i_way_word          (i_way_word),
        .i_hit               (i_hit),
        .i_cacheable         (i_cacheable),
        .i_user_mode         (i_user_mode),
        .i_mmu_fault         (i_mmu_fault),
        .i_mmu_fault_status  (i_mmu_fault_status),
        .i_replayed          (i_replayed),
        .i_deliver           (i_deliver),
        .i_deliver_word      (i_deliver_word),
        .i_deliver_fault     (i_deliver_fault),
        .o_miss              (o_miss),
        .i_flush             (i_flush),
        .i_enq_ready         (i_enq_ready),
        .o_enq_valid         (o_enq_valid),
        .o_enq_fire          (o_enq_fire),
        .o_enq_bundle        (bundle),
        .o_enq_pc            (o_enq_pc),
        .o_enq_next_pc       (o_enq_next_pc),
        .o_enq_fault_pending (o_enq_fault_pending),
        .o_enq_fault_vec     (o_enq_fault_vec),
        .o_enq_fault_status  (o_enq_fault_status),
        .o_count_read        (o_count_read),
        .o_hold              (o_hold)
    );

    assign o_enq_op_class = bundle.op_class;

endmodule
