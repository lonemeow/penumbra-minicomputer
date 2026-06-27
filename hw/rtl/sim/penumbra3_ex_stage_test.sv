// Unit-test wrapper for penumbra3_ex_stage.
//
// EX consumes the packed ctrl_bundle_t (the ID/EX register) and emits one (the
// EX/MEM1 register), both opaque to a C++ testbench. This wrapper decodes a raw
// instruction word into the bundle -- exactly as the IF2 enqueue path does -- so
// the testbench drives instructions, and fans the EX/MEM1 struct out to the flat
// op_class field the test checks. Every other EX port (the operands, the
// physical-source forwarding qualifiers, the back-end forward sources, the flag
// bypass sources, the handshake) is already flat and passes straight through, so
// the test can exercise the operand-forwarding network directly.
//
// The incoming-bubble cause is tied to BCAUSE_NONE: bcause attribution is a
// perfctr concern owned by the spine, not EX behavior under test here.
module penumbra3_ex_stage_test
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // Instruction word -> decoded bundle (the IF2 enqueue path)
    input  logic [31:0]           i_ir,
    input  logic [31:0]           i_op_a,
    input  logic [31:0]           i_op_b,
    input  logic [31:0]           i_store_data,
    input  logic [SB_IDX_W-1:0]   i_phys_dst,
    input  logic                  i_phys_dst_we,
    input  logic [SB_IDX_W-1:0]   i_phys_dst_aux,
    input  logic                  i_phys_dst_aux_we,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,
    input  logic                  i_valid,
    input  logic                  i_fault_pending,
    input  logic [3:0]            i_fault_vec,
    input  logic [31:0]           i_fault_status,

    input  logic [SB_IDX_W-1:0]   i_phys_src_a,
    input  logic                  i_src_a_fwdable,
    input  logic [SB_IDX_W-1:0]   i_phys_src_b,
    input  logic                  i_src_b_fwdable,

    input  logic [31:0]           i_mem2_result,
    input  logic [SB_IDX_W-1:0]   i_mem2_phys_dst,
    input  logic                  i_mem2_dst_we,
    input  logic                  i_mem2_valid,
    input  logic [31:0]           i_wb_result,
    input  logic [SB_IDX_W-1:0]   i_wb_phys_dst,
    input  logic                  i_wb_dst_we,
    input  logic                  i_wb_valid,

    input  logic [3:0]            i_sr_flags,
    input  logic [3:0]            i_mem2_flags,
    input  logic                  i_mem2_writes_flags,
    input  logic [3:0]            i_wb_flags,
    input  logic                  i_wb_writes_flags,
    input  logic [31:0]           i_sr_committed,

    input  logic                  i_irq_inject,
    input  logic [3:0]            i_irq_vec,

    input  logic                  i_stall_in,
    input  logic                  i_wb_active,
    input  logic                  i_bubble,
    output logic                  o_local_stall,
    output logic                  o_dc_commit,
    output logic                  o_funit_stall,

    output logic                  o_branch_taken,
    output logic [31:0]           o_branch_target,

    output logic [3:0]            o_mem1_op_class,
    output logic [31:0]           o_mem1_result,
    output logic [31:0]           o_mem1_result_aux,
    output logic [31:0]           o_mem1_store_data,
    output logic [3:0]            o_mem1_flag_value,
    output logic [SB_IDX_W-1:0]   o_mem1_phys_dst,
    output logic                  o_mem1_phys_dst_we,
    output logic [SB_IDX_W-1:0]   o_mem1_phys_dst_aux,
    output logic                  o_mem1_phys_dst_aux_we,
    output logic [31:0]           o_mem1_pc,
    output logic                  o_mem1_valid,
    output logic                  o_mem1_fault_pending,
    output logic [3:0]            o_mem1_fault_vec,
    output logic [31:0]           o_mem1_fault_status
);

    ctrl_bundle_t bundle;
    // Only op_class is fanned out; the rest of the EX/MEM1 bundle rides through
    // EX's flat ports, so its other fields are intentionally unread here. The
    // bcause output is likewise not under test.
    /* verilator lint_off UNUSEDSIGNAL */
    ctrl_bundle_t mem1_bundle;
    bcause_e      mem1_bcause;
    /* verilator lint_on UNUSEDSIGNAL */

    penumbra3_decode u_decode (
        .i_ir     (i_ir),
        .o_bundle (bundle)
    );

    penumbra3_ex_stage u_ex (
        .i_clk                  (i_clk),
        .i_rst                  (i_rst),
        .i_bundle               (bundle),
        .i_op_a                 (i_op_a),
        .i_op_b                 (i_op_b),
        .i_store_data           (i_store_data),
        .i_phys_dst             (i_phys_dst),
        .i_phys_dst_we          (i_phys_dst_we),
        .i_phys_dst_aux         (i_phys_dst_aux),
        .i_phys_dst_aux_we      (i_phys_dst_aux_we),
        .i_pc                   (i_pc),
        .i_next_pc              (i_next_pc),
        .i_valid                (i_valid),
        .i_fault_pending        (i_fault_pending),
        .i_fault_vec            (i_fault_vec),
        .i_fault_status         (i_fault_status),
        .i_bcause               (BCAUSE_NONE),
        .i_phys_src_a           (i_phys_src_a),
        .i_src_a_fwdable        (i_src_a_fwdable),
        .i_phys_src_b           (i_phys_src_b),
        .i_src_b_fwdable        (i_src_b_fwdable),
        .i_mem2_result          (i_mem2_result),
        .i_mem2_phys_dst        (i_mem2_phys_dst),
        .i_mem2_dst_we          (i_mem2_dst_we),
        .i_mem2_valid           (i_mem2_valid),
        .i_wb_result            (i_wb_result),
        .i_wb_phys_dst          (i_wb_phys_dst),
        .i_wb_dst_we            (i_wb_dst_we),
        .i_wb_valid             (i_wb_valid),
        .i_sr_flags             (i_sr_flags),
        .i_mem2_flags           (i_mem2_flags),
        .i_mem2_writes_flags    (i_mem2_writes_flags),
        .i_wb_flags             (i_wb_flags),
        .i_wb_writes_flags      (i_wb_writes_flags),
        .i_sr_committed         (i_sr_committed),
        .i_irq_inject           (i_irq_inject),
        .i_irq_vec              (i_irq_vec),
        .i_stall_in             (i_stall_in),
        .i_wb_active            (i_wb_active),
        .i_bubble               (i_bubble),
        .o_local_stall          (o_local_stall),
        .o_dc_commit            (o_dc_commit),
        .o_funit_stall          (o_funit_stall),
        .o_branch_taken         (o_branch_taken),
        .o_branch_target        (o_branch_target),
        .o_mem1_bundle          (mem1_bundle),
        .o_mem1_result          (o_mem1_result),
        .o_mem1_result_aux      (o_mem1_result_aux),
        .o_mem1_store_data      (o_mem1_store_data),
        .o_mem1_flag_value      (o_mem1_flag_value),
        .o_mem1_phys_dst        (o_mem1_phys_dst),
        .o_mem1_phys_dst_we     (o_mem1_phys_dst_we),
        .o_mem1_phys_dst_aux    (o_mem1_phys_dst_aux),
        .o_mem1_phys_dst_aux_we (o_mem1_phys_dst_aux_we),
        .o_mem1_pc              (o_mem1_pc),
        .o_mem1_valid           (o_mem1_valid),
        .o_mem1_bcause          (mem1_bcause),
        .o_mem1_fault_pending   (o_mem1_fault_pending),
        .o_mem1_fault_vec       (o_mem1_fault_vec),
        .o_mem1_fault_status    (o_mem1_fault_status)
    );

    assign o_mem1_op_class = mem1_bundle.op_class;

endmodule
