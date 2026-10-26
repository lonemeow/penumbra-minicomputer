// Unit-test wrapper for penumbra3_mem2_stage: the data side as the machine
// composes it, less the uncore. MEM1 launches; penumbra3_translate (the TLB's
// D-copy) and a real penumbra3_l1 resolve a cycle later into MEM2; MEM2
// resolves the access, and its completion unit asks the uncore for what the
// L1 cannot serve. The testbench plays the uncore at the port's request and
// response (doc/internals/penumbra3/uncore-port.md).
//
// The freeze is wired the way the spine wires it: hold = load_pending |
// external stall goes to MEM1, the translate, the L1's launch and MEM2's
// slot, and the external stall alone holds the MEM2/WB register. A raw
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

    // L1-D sysreg port (CTRL, INVAL_ALL, the counters)
    input  logic [3:0]  i_l1_sys_reg,
    input  logic [31:0] i_l1_sys_wdata,
    input  logic        i_l1_sys_we,
    input  logic        i_l1_sys_re,
    output logic [31:0] o_l1_sys_rdata,

    // TLB install (D-copy, via dtranslate's write port)
    input  logic        i_tlb_wr_en,
    input  logic [4:0]  i_tlb_wr_set,
    input  logic        i_tlb_wr_way,
    input  logic        i_tlb_wr_valid,
    input  logic [31:0] i_tlb_wr_vpn_word,
    input  logic [31:0] i_tlb_wr_pte_word,

    // Core-to-uncore port, data side: the testbench is the uncore
    output logic        o_dreq_valid,
    output logic        o_dreq_cacheable,
    output logic [31:0] o_dreq_addr,
    output logic [3:0]  o_dreq_byte_en,
    input  logic        i_drsp_done,
    input  logic        i_rsp_fault,
    input  logic [UNCORE_LINE_WORDS-1:0][31:0] i_rsp_line,

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
    output logic        o_complete,
    output logic [SB_IDX_W-1:0] o_complete_dest,
    output logic [31:0] o_complete_value,
    output logic        o_complete_fault,

    output logic        o_fwd_valid,
    output logic [31:0] o_fwd_result,
    output logic [SB_IDX_W-1:0] o_fwd_dst,
    output logic        o_fwd_we,

    // L1-D store strobe and line install, as MEM2 drives them
    output logic        o_dcache_we,
    output logic [31:0] o_dcache_wdata,
    output logic        o_dcache_fill_we,
    output logic        o_dcache_fill_way,
    output logic [31:0] o_dcache_fill_paddr
);

    localparam int D_WAYS   = 2;
    localparam int TLB_WAYS = 2;

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
    logic        dc_en;
    // The sysreg launch attaches at the spine, and the L1 takes the page
    // offset alone, so these are unread here.
    /* verilator lint_off UNUSEDSIGNAL */
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
    logic [TLB_WAYS-1:0][19:0] tr_way_frame;
    logic [TLB_WAYS-1:0]       tr_way_sel;
    /* verilator lint_off PINCONNECTEMPTY */
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
        .o_way_frame       (tr_way_frame),
        .o_way_sel         (tr_way_sel),
        .o_rd_vpn_word     (),               // read-back tap, unused here
        .o_rd_pte_word     (),
        .i_wr_en           (i_tlb_wr_en),
        .i_wr_set          (i_tlb_wr_set),
        .i_wr_way          (i_tlb_wr_way),
        .i_wr_valid        (i_tlb_wr_valid),
        .i_wr_vpn_word     (i_tlb_wr_vpn_word),
        .i_wr_pte_word     (i_tlb_wr_pte_word)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // -- L1-D ----------------------------------------------------
    // The frame candidates as the MMU presents them: candidate 0 is the
    // bypass / pinned frame, which this wrapper has none of, and the rest
    // are the main TLB's ways.
    logic                 dl1_hit, dl1_enabled, dl1_extra_cycle;
    logic [31:0]          dl1_word;
    logic [0:0]           dl1_victim_way;
    logic [3:0]           dcache_byte_en;
    logic                 dcache_count_read, dcache_count_write;

    /* verilator lint_off PINCONNECTEMPTY */
    penumbra3_l1 #(
        .WAYS       (D_WAYS),
        .LINE_BYTES (UNCORE_LINE_WORDS * 4),
        .FRAMES     (TLB_WAYS + 1)
    ) u_dl1 (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_launch        (dc_en && !hold),
        .i_offset        (dc_vaddr[11:0]),
        .i_frame         ({tr_way_frame, 20'b0}),
        .i_frame_sel     ({tr_way_sel, 1'b0}),
        .o_way_hit       (),
        .o_way_word      (),
        .o_hit           (dl1_hit),
        .o_word          (dl1_word),
        .o_victim_way    (dl1_victim_way),
        .o_extra_cycle   (dl1_extra_cycle),
        .i_store         (o_dcache_we),
        .i_store_wdata   (o_dcache_wdata),
        .i_store_byte_en (dcache_byte_en),
        .i_count_read    (dcache_count_read),
        .i_count_write   (dcache_count_write),
        .i_fill_we       (o_dcache_fill_we),
        .i_fill_paddr    (o_dcache_fill_paddr),
        .i_fill_way      (o_dcache_fill_way),
        .i_fill_line     (i_rsp_line),
        .i_sys_reg       (i_l1_sys_reg),
        .i_sys_wdata     (i_l1_sys_wdata),
        .i_sys_we        (i_l1_sys_we),
        .i_sys_re        (i_l1_sys_re),
        .o_sys_rdata     (o_l1_sys_rdata),
        .o_enabled       (dl1_enabled)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // -- MEM2 resolve ---------------------------------------------
    /* verilator lint_off UNUSEDSIGNAL */
    ctrl_bundle_t   wb_bundle;       // only op_class fanned out
    dpath_payload_t wb_payload;      // value / dst / fault fanned out
    bcause_e        wb_bcause;       // not under test
    /* verilator lint_on UNUSEDSIGNAL */

    penumbra3_mem2_stage #(
        .DL1_WAYS (D_WAYS)
    ) u_mem2 (
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
        .i_dcache_hit           (dl1_hit),
        .i_dcache_rdata         (dl1_word),
        .i_dcache_victim_way    (dl1_victim_way),
        .i_dcache_enabled       (dl1_enabled),
        .o_dcache_we            (o_dcache_we),
        .o_dcache_wdata         (o_dcache_wdata),
        .o_dcache_byte_en       (dcache_byte_en),
        .o_dcache_count_read    (dcache_count_read),
        .o_dcache_count_write   (dcache_count_write),
        .o_dcache_fill_we       (o_dcache_fill_we),
        .o_dcache_fill_way      (o_dcache_fill_way),
        .o_dcache_fill_paddr    (o_dcache_fill_paddr),
        .i_dcache_extra_cycle   (dl1_extra_cycle),
        .i_sys_rdata            (32'b0),
        .o_dreq_valid           (o_dreq_valid),
        .o_dreq_cacheable       (o_dreq_cacheable),
        .o_dreq_addr            (o_dreq_addr),
        .o_dreq_byte_en         (o_dreq_byte_en),
        .i_drsp_done            (i_drsp_done),
        .i_rsp_fault            (i_rsp_fault),
        .i_rsp_line             (i_rsp_line),
        .o_load_pending         (o_load_pending),
        .o_complete             (o_complete),
        .o_complete_dest        (o_complete_dest),
        .o_complete_value       (o_complete_value),
        .o_complete_fault       (o_complete_fault),
        .o_fwd_result           (o_fwd_result),
        .o_fwd_dst              (o_fwd_dst),
        .o_fwd_we               (o_fwd_we),
        .o_fwd_valid            (o_fwd_valid),
        .i_launch_hold          (hold),
        .i_hold                 (i_ext_stall),
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
