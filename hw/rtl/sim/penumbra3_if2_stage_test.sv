// Unit-test wrapper for penumbra3_if2_stage.
//
// IF2 outputs the enqueue payload partly as a packed ctrl_bundle_t, which the
// C++ testbench would otherwise see as one opaque vector. This wrapper
// passes the flat enqueue/handshake/fault ports straight through and exposes
// one bundle field -- op_class -- so the testbench can confirm *which* word was
// enqueued (live vs the parked skid word vs a stale port word) using inputs
// that decode to distinct classes. The full bundle is covered by the decode
// test; IF2's job is the handshake, fault composition, and skid.
module penumbra3_if2_stage_test
    import penumbra3_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    input  logic [31:0] i_pc,
    input  logic [31:0] i_next_pc,
    input  logic        i_valid,

    input  logic [31:0] i_ir,
    input  logic        i_mem_busy,
    input  logic        i_mem_fault,
    output logic        o_fetch_re,

    input  logic        i_user_mode,
    input  logic        i_mmu_fault,
    input  logic [31:0] i_mmu_fault_status,

    input  logic        i_flush,

    input  logic        i_enq_ready,
    output logic        o_enq_valid,
    output logic [3:0]  o_enq_op_class,
    output logic [31:0] o_enq_pc,
    output logic [31:0] o_enq_next_pc,
    output logic        o_enq_fault_pending,
    output logic [3:0]  o_enq_fault_vec,
    output logic [31:0] o_enq_fault_status,

    output logic        o_stall
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
        .i_ir                (i_ir),
        .i_mem_busy          (i_mem_busy),
        .i_mem_fault         (i_mem_fault),
        .o_fetch_re          (o_fetch_re),
        .i_user_mode         (i_user_mode),
        .i_mmu_fault         (i_mmu_fault),
        .i_mmu_fault_status  (i_mmu_fault_status),
        .i_flush             (i_flush),
        .i_enq_ready         (i_enq_ready),
        .o_enq_valid         (o_enq_valid),
        .o_enq_bundle        (bundle),
        .o_enq_pc            (o_enq_pc),
        .o_enq_next_pc       (o_enq_next_pc),
        .o_enq_fault_pending (o_enq_fault_pending),
        .o_enq_fault_vec     (o_enq_fault_vec),
        .o_enq_fault_status  (o_enq_fault_status),
        .o_stall             (o_stall)
    );

    assign o_enq_op_class = bundle.op_class;

endmodule
