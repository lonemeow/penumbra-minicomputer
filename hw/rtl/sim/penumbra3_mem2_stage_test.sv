// Unit-test wrapper for penumbra3_mem2_stage, composed with the real MEM1 stage
// and dtranslate so the launch -> verdict -> resolve alignment is exercised end
// to end (not the two halves in isolation).
//
// MEM1 drives the translation launch; dtranslate resolves the verdict a cycle
// later into MEM2; MEM2 resolves the access. The freeze is wired the way the
// spine will wire it: hold = load_pending | external stall, fanned to MEM1,
// MEM2, and dtranslate together. The D-cache resolve and the bus-master fill
// are driven by the testbench (those leaves attach at the spine). A raw
// instruction word feeds penumbra3_decode for the bundle; the payload's live
// fields come in flat (value = the effective address).
module penumbra3_mem2_stage_test
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // Access in
    input  logic [31:0] i_ir,
    input  logic [31:0] i_ea,
    input  logic [SB_IDX_W-1:0] i_phys_dst,
    input  logic        i_phys_dst_we,
    input  logic        i_fault_pending,
    input  logic [31:0] i_store_data,
    input  logic        i_valid,
    input  logic        i_user_mode,
    input  logic [7:0]  i_asid,

    // D-cache resolve (driven by the testbench)
    input  logic        i_dcache_hit,
    input  logic [31:0] i_dcache_rdata,

    // TLB install (D-copy, via dtranslate's write port)
    input  logic        i_tlb_wr_en,
    input  logic [4:0]  i_tlb_wr_set,
    input  logic        i_tlb_wr_way,
    input  logic        i_tlb_wr_valid,
    input  logic [31:0] i_tlb_wr_vpn_word,
    input  logic [31:0] i_tlb_wr_pte_word,

    // Bus-master fill response
    input  logic        i_fill_done,
    input  logic [31:0] i_fill_data,
    input  logic        i_fill_fault,

    // External stall (WB back-pressure) + flush
    input  logic        i_ext_stall,
    input  logic        i_bubble,

    // MEM2/WB register (flattened) + back-end + forward + store
    output logic        o_wb_valid,
    output logic [3:0]  o_wb_op_class,
    output logic [31:0] o_wb_value,
    output logic [SB_IDX_W-1:0] o_wb_phys_dst,
    output logic        o_wb_fault_pending,
    output logic [3:0]  o_wb_fault_vec,

    output logic        o_load_pending,
    output logic        o_launch_fill,
    output logic [31:0] o_fill_paddr,
    output logic        o_complete,
    output logic [SB_IDX_W-1:0] o_complete_dest,
    output logic [31:0] o_complete_value,
    output logic        o_complete_fault,

    output logic        o_fwd_valid,
    output logic [31:0] o_fwd_result,
    output logic [SB_IDX_W-1:0] o_fwd_dst,
    output logic        o_fwd_we,

    output logic        o_dcache_we,
    output logic [31:0] o_dcache_paddr,
    output logic [31:0] o_dcache_wdata
);

    // -- Bundle + payload assembly --------------------------------
    ctrl_bundle_t   bundle;
    dpath_payload_t payload;
    penumbra3_decode u_decode (.i_ir (i_ir), .o_bundle (bundle));
    always_comb begin
        payload               = '0;
        payload.value         = i_ea;
        payload.phys_dst      = i_phys_dst;
        payload.phys_dst_we   = i_phys_dst_we;
        payload.fault_pending = i_fault_pending;
    end

    // -- Spine-style freeze: registered load_pending OR external stall --
    logic hold;
    assign hold = o_load_pending | i_ext_stall;

    // -- MEM1 launch ----------------------------------------------
    logic        tr_en;
    logic [31:0] tr_vaddr;
    logic [2:0]  tr_acc;
    // The cache + sysreg launch attach at the spine, so they are unread here.
    /* verilator lint_off UNUSEDSIGNAL */
    logic        dc_en;
    logic [31:0] dc_vaddr;
    logic        sys_re;
    logic [3:0]  sys_dev, sys_reg;
    /* verilator lint_on UNUSEDSIGNAL */

    ctrl_bundle_t   mem2_bundle;
    dpath_payload_t mem2_payload;
    logic [31:0]    mem2_store_wdata;
    logic [3:0]     mem2_byte_en;
    logic           mem2_align_fault;
    logic           mem2_valid;
    bcause_e        mem2_bcause;

    penumbra3_mem1_stage u_mem1 (
        .i_clk                (i_clk),
        .i_rst                (i_rst),
        .i_bundle             (bundle),
        .i_payload            (payload),
        .i_store_data         (i_store_data),
        .i_valid              (i_valid),
        .i_bcause             (BCAUSE_NONE),
        .i_hold               (hold),
        .i_bubble             (i_bubble),
        .o_translate_en       (tr_en),
        .o_translate_vaddr    (tr_vaddr),
        .o_translate_acc_type (tr_acc),
        .o_dcache_en          (dc_en),
        .o_dcache_vaddr       (dc_vaddr),
        .o_sys_re             (sys_re),
        .o_sys_dev            (sys_dev),
        .o_sys_reg            (sys_reg),
        .o_mem2_bundle        (mem2_bundle),
        .o_mem2_payload       (mem2_payload),
        .o_mem2_store_wdata   (mem2_store_wdata),
        .o_mem2_byte_en       (mem2_byte_en),
        .o_mem2_align_fault   (mem2_align_fault),
        .o_mem2_valid         (mem2_valid),
        .o_mem2_bcause        (mem2_bcause)
    );

    // -- Address translation (MEM1 launch / MEM2 verdict) ---------
    logic [31:0] tr_paddr;
    logic        tr_cacheable, tr_hit, tr_miss_fault, tr_prot_fault;
    penumbra3_translate u_dtranslate (
        .i_clk             (i_clk),
        .i_rst             (i_rst),
        .i_lookup_en       (tr_en),
        .i_vaddr           (tr_vaddr),
        .i_access_type     (tr_acc),
        .i_user_mode       (i_user_mode),
        .i_asid            (i_asid),
        .i_hold            (hold),
        .i_pinned_hit      (1'b0),
        .i_pinned_pte_word (32'b0),
        .o_paddr           (tr_paddr),
        .o_cacheable       (tr_cacheable),
        .o_hit             (tr_hit),
        .o_miss_fault      (tr_miss_fault),
        .o_prot_fault      (tr_prot_fault),
        .i_wr_en           (i_tlb_wr_en),
        .i_wr_set          (i_tlb_wr_set),
        .i_wr_way          (i_tlb_wr_way),
        .i_wr_valid        (i_tlb_wr_valid),
        .i_wr_vpn_word     (i_tlb_wr_vpn_word),
        .i_wr_pte_word     (i_tlb_wr_pte_word)
    );

    // -- MEM2 resolve ---------------------------------------------
    /* verilator lint_off UNUSEDSIGNAL */
    ctrl_bundle_t   wb_bundle;       // only op_class fanned out
    dpath_payload_t wb_payload;      // value / dst / fault fanned out
    bcause_e        wb_bcause;       // not under test
    logic [3:0]     dcache_byte_en_unused;
    logic           fill_cacheable_unused;
    /* verilator lint_on UNUSEDSIGNAL */

    penumbra3_mem2_stage u_mem2 (
        .i_clk                  (i_clk),
        .i_rst                  (i_rst),
        .i_bundle               (mem2_bundle),
        .i_payload              (mem2_payload),
        .i_store_wdata          (mem2_store_wdata),
        .i_byte_en              (mem2_byte_en),
        .i_align_fault          (mem2_align_fault),
        .i_valid                (mem2_valid),
        .i_bcause               (mem2_bcause),
        .i_user_mode            (i_user_mode),
        .i_translate_paddr      (tr_paddr),
        .i_translate_cacheable  (tr_cacheable),
        .i_translate_hit        (tr_hit),
        .i_translate_miss_fault (tr_miss_fault),
        .i_translate_prot_fault (tr_prot_fault),
        .i_dcache_hit           (i_dcache_hit),
        .i_dcache_rdata         (i_dcache_rdata),
        .o_dcache_paddr         (o_dcache_paddr),
        .o_dcache_we            (o_dcache_we),
        .o_dcache_wdata         (o_dcache_wdata),
        .o_dcache_byte_en       (dcache_byte_en_unused),
        .i_sys_rdata            (32'b0),
        .o_launch_fill          (o_launch_fill),
        .o_fill_paddr           (o_fill_paddr),
        .o_fill_cacheable       (fill_cacheable_unused),
        .i_fill_done            (i_fill_done),
        .i_fill_data            (i_fill_data),
        .i_fill_fault           (i_fill_fault),
        .o_load_pending         (o_load_pending),
        .o_complete             (o_complete),
        .o_complete_dest        (o_complete_dest),
        .o_complete_value       (o_complete_value),
        .o_complete_fault       (o_complete_fault),
        .o_fwd_result           (o_fwd_result),
        .o_fwd_dst              (o_fwd_dst),
        .o_fwd_we               (o_fwd_we),
        .o_fwd_valid            (o_fwd_valid),
        .i_hold                 (hold),
        .i_bubble               (i_bubble),
        .o_wb_bundle            (wb_bundle),
        .o_wb_payload           (wb_payload),
        .o_wb_valid             (o_wb_valid),
        .o_wb_bcause            (wb_bcause)
    );

    assign o_wb_op_class      = wb_bundle.op_class;
    assign o_wb_value         = wb_payload.value;
    assign o_wb_phys_dst      = wb_payload.phys_dst;
    assign o_wb_fault_pending = wb_payload.fault_pending;
    assign o_wb_fault_vec     = wb_payload.fault_vec;

endmodule
