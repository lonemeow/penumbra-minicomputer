// Unit-test wrapper for penumbra3_id_stage.
//
// ID consumes a packed ctrl_bundle_t and emits one (the ID/EX register), both
// opaque to a C++ testbench. This wrapper decodes a raw instruction word into
// the bundle (exactly as the real IF2 enqueue path does) so the testbench can
// drive instructions, and fans the ID/EX struct out to the flat control fields
// (op_class, alu_op) the test checks. The other ID/EX fields are already flat
// ports on ID and pass straight through.
module penumbra3_id_stage_test
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // Instruction word -> decoded bundle (the IF2 enqueue path)
    input  logic [31:0]           i_ir,
    input  logic [31:0]           i_pc,
    input  logic [31:0]           i_next_pc,
    input  logic                  i_valid,
    input  logic                  i_if_fault_pending,
    input  logic [3:0]            i_if_fault_vec,
    input  logic [31:0]           i_if_fault_status,
    output logic                  o_deq_ready,

    input  logic                  i_supervisor,

    input  logic                  i_stall_in,
    input  logic                  i_bubble,
    input  logic                  i_fetch_busy,
    input  logic                  i_pipe_hold,
    output logic                  o_local_stall,

    output logic [SB_IDX_W-1:0]   o_rd_idx_a,
    output logic [SB_IDX_W-1:0]   o_rd_idx_b,
    input  logic [31:0]           i_rd_data_a,
    input  logic [31:0]           i_rd_data_b,

    output logic [3:0]            o_spr_rd_sel,
    input  logic [31:0]           i_spr_src_value,

    input  logic                  i_clr_en,
    input  logic [SB_IDX_W-1:0]   i_clr_idx,

    input  logic                  i_fwd0_en,
    input  logic [SB_IDX_W-1:0]   i_fwd0_idx,
    input  logic                  i_fwd1_en,
    input  logic [SB_IDX_W-1:0]   i_fwd1_idx,

    output logic [3:0]            o_ex_op_class,
    output logic [3:0]            o_ex_alu_op,
    output logic [31:0]           o_ex_op_a,
    output logic [31:0]           o_ex_op_b,
    output logic [31:0]           o_ex_store_data,
    output logic [SB_IDX_W-1:0]   o_ex_phys_dst,
    output logic                  o_ex_phys_dst_we,
    output logic [SB_IDX_W-1:0]   o_ex_phys_dst_aux,
    output logic                  o_ex_phys_dst_aux_we,
    output logic [SB_IDX_W-1:0]   o_ex_phys_src_a,
    output logic                  o_ex_src_a_fwdable,
    output logic [SB_IDX_W-1:0]   o_ex_phys_src_b,
    output logic                  o_ex_src_b_fwdable,
    output logic [31:0]           o_ex_pc,
    output logic [31:0]           o_ex_next_pc,
    output logic                  o_ex_valid,
    output logic                  o_ex_fault_pending,
    output logic [3:0]            o_ex_fault_vec,
    output logic [31:0]           o_ex_fault_status,
    output logic [2:0]            o_ex_bcause
);

    ctrl_bundle_t bundle;
    // Only op_class/alu_op are exposed; the rest of the ID/EX bundle is checked
    // through ID's flat ports, so its other fields are intentionally unread here.
    /* verilator lint_off UNUSEDSIGNAL */
    ctrl_bundle_t ex_bundle;
    /* verilator lint_on UNUSEDSIGNAL */

    penumbra3_decode u_decode (
        .i_ir     (i_ir),
        .o_bundle (bundle)
    );

    penumbra3_id_stage u_id (
        .i_clk               (i_clk),
        .i_rst               (i_rst),
        .i_bundle            (bundle),
        .i_pc                (i_pc),
        .i_next_pc           (i_next_pc),
        .i_valid             (i_valid),
        .i_if_fault_pending  (i_if_fault_pending),
        .i_if_fault_vec      (i_if_fault_vec),
        .i_if_fault_status   (i_if_fault_status),
        .o_deq_ready         (o_deq_ready),
        .i_supervisor        (i_supervisor),
        .i_stall_in          (i_stall_in),
        .i_bubble            (i_bubble),
        .i_fetch_busy        (i_fetch_busy),
        .i_pipe_hold         (i_pipe_hold),
        .o_local_stall       (o_local_stall),
        .o_rd_idx_a          (o_rd_idx_a),
        .o_rd_idx_b          (o_rd_idx_b),
        .i_rd_data_a         (i_rd_data_a),
        .i_rd_data_b         (i_rd_data_b),
        .o_spr_rd_sel        (o_spr_rd_sel),
        .i_spr_src_value     (i_spr_src_value),
        .i_clr_en            (i_clr_en),
        .i_clr_idx           (i_clr_idx),
        .i_fwd0_en           (i_fwd0_en),
        .i_fwd0_idx          (i_fwd0_idx),
        .i_fwd1_en           (i_fwd1_en),
        .i_fwd1_idx          (i_fwd1_idx),
        .o_ex_bundle         (ex_bundle),
        .o_ex_op_a           (o_ex_op_a),
        .o_ex_op_b           (o_ex_op_b),
        .o_ex_store_data     (o_ex_store_data),
        .o_ex_phys_dst       (o_ex_phys_dst),
        .o_ex_phys_dst_we    (o_ex_phys_dst_we),
        .o_ex_phys_dst_aux   (o_ex_phys_dst_aux),
        .o_ex_phys_dst_aux_we(o_ex_phys_dst_aux_we),
        .o_ex_phys_src_a     (o_ex_phys_src_a),
        .o_ex_src_a_fwdable  (o_ex_src_a_fwdable),
        .o_ex_phys_src_b     (o_ex_phys_src_b),
        .o_ex_src_b_fwdable  (o_ex_src_b_fwdable),
        .o_ex_pc             (o_ex_pc),
        .o_ex_next_pc        (o_ex_next_pc),
        .o_ex_valid          (o_ex_valid),
        .o_ex_fault_pending  (o_ex_fault_pending),
        .o_ex_fault_vec      (o_ex_fault_vec),
        .o_ex_fault_status   (o_ex_fault_status),
        .o_ex_bcause         (o_ex_bcause)
    );

    assign o_ex_op_class = ex_bundle.op_class;
    assign o_ex_alu_op   = ex_bundle.alu_op;

endmodule
