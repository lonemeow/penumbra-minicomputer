// Unit-test wrapper for penumbra3_mem1_stage.
//
// Decodes a raw instruction word into the control bundle and assembles a
// minimal datapath payload from flat inputs (value = the effective address),
// so the testbench drives an access and checks MEM1's job: the launch drives
// (translation query, cache index, sysreg read), alignment, store lane prep,
// and the MEM1/MEM2 register under the handshake. The struct outputs are fanned
// out to the flat fields the testbench reads.
module penumbra3_mem1_stage_test
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // Access in: instruction word -> bundle, plus the payload's live fields
    input  logic [31:0] i_ir,
    input  logic [31:0] i_ea,            // payload.value (effective address)
    input  logic [SB_IDX_W-1:0] i_phys_dst,
    input  logic        i_phys_dst_we,
    input  logic        i_fault_pending, // an upstream fault riding the slot
    input  logic [31:0] i_store_data,
    input  logic        i_valid,
    input  logic        i_hold,
    input  logic        i_bubble,

    // Launch drives
    output logic        o_translate_en,
    output logic [31:0] o_translate_vaddr,
    output logic [2:0]  o_translate_acc_type,
    output logic        o_dcache_en,
    output logic [31:0] o_dcache_vaddr,
    output logic        o_sys_re,
    output logic [3:0]  o_sys_dev,
    output logic [3:0]  o_sys_reg,

    // MEM1/MEM2 register (flattened)
    output logic        o_mem2_valid,
    output logic        o_mem2_align_fault,
    output logic [3:0]  o_mem2_byte_en,
    output logic [31:0] o_mem2_store_wdata,
    output logic [3:0]  o_mem2_op_class,
    output logic [31:0] o_mem2_ea
);

    ctrl_bundle_t   bundle;
    dpath_payload_t payload;

    penumbra3_decode u_decode (.i_ir (i_ir), .o_bundle (bundle));

    always_comb begin
        payload              = '0;
        payload.value        = i_ea;
        payload.phys_dst     = i_phys_dst;
        payload.phys_dst_we  = i_phys_dst_we;
        payload.fault_pending = i_fault_pending;
    end

    /* verilator lint_off UNUSEDSIGNAL */
    ctrl_bundle_t   mem2_bundle;     // only op_class fanned out
    dpath_payload_t mem2_payload;    // only value fanned out
    bcause_e        mem2_bcause;     // not under test
    /* verilator lint_on UNUSEDSIGNAL */

    penumbra3_mem1_stage u_mem1 (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_bundle           (bundle),
        .i_payload          (payload),
        .i_store_data       (i_store_data),
        .i_valid            (i_valid),
        .i_bcause           (BCAUSE_NONE),
        .i_hold             (i_hold),
        .i_bubble           (i_bubble),
        .o_translate_en     (o_translate_en),
        .o_translate_vaddr  (o_translate_vaddr),
        .o_translate_acc_type (o_translate_acc_type),
        .o_dcache_en        (o_dcache_en),
        .o_dcache_vaddr     (o_dcache_vaddr),
        .o_sys_re           (o_sys_re),
        .o_sys_dev          (o_sys_dev),
        .o_sys_reg          (o_sys_reg),
        .o_mem2_bundle      (mem2_bundle),
        .o_mem2_payload     (mem2_payload),
        .o_mem2_store_wdata (o_mem2_store_wdata),
        .o_mem2_byte_en     (o_mem2_byte_en),
        .o_mem2_align_fault (o_mem2_align_fault),
        .o_mem2_valid       (o_mem2_valid),
        .o_mem2_bcause      (mem2_bcause)
    );

    assign o_mem2_op_class = mem2_bundle.op_class;
    assign o_mem2_ea       = mem2_payload.value;

endmodule
