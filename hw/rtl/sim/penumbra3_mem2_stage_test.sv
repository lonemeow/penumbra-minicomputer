// Unit-test wrapper for penumbra3_mem2_stage: the data side as the machine
// composes it, less the uncore. MEM1 launches; penumbra3_translate (the TLB's
// D-copy) and a real penumbra3_l1 resolve a cycle later into MEM2; MEM2
// resolves the access, and the L1 carries what it had no word for to the
// uncore. The testbench plays the uncore at the port's request and response
// (doc/internals/penumbra3/uncore-port.md), and the store buffer at MEM2's
// store port.
//
// The freeze is wired the way the spine wires it: hold = mem_pending |
// external stall goes to MEM1, the translate, the L1's launch and MEM2's
// slot, and the external stall alone holds the MEM2/WB register. The data
// side discards no lookup. A raw
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
    input  logic [31:0] i_pc,
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

    // TLB install (D-copy, via dtranslate's write port); the PTE word's V
    // is the entry's valid bit
    input  logic        i_tlb_wr_en,
    input  logic [4:0]  i_tlb_wr_set,
    input  logic        i_tlb_wr_way,
    input  logic [31:0] i_tlb_wr_vpn_word,
    input  logic [31:0] i_tlb_wr_pte_word,

    // Core-to-uncore port, data side: the testbench is the uncore
    output logic        o_dreq_valid,
    output logic        o_dreq_cacheable,
    output logic [31:0] o_dreq_addr,
    output logic        o_dreq_we,
    output logic [3:0]  o_dreq_byte_en,
    output logic [31:0] o_dreq_wdata,
    input  logic        i_drsp_done,
    input  logic        i_rsp_fault,
    input  logic [UNCORE_LINE_WORDS-1:0][31:0] i_rsp_line,

    // Store buffer: the testbench is the buffer
    output logic        o_store_enq,
    output logic [31:0] o_store_addr,
    input  logic        i_store_full,

    // External stall (WB back-pressure) + flush
    input  logic        i_ext_stall,
    input  logic        i_bubble,

    // MEM2/WB register (flattened) + back-end + forward + store
    output logic        o_wb_valid,
    output logic [2:0]  o_wb_bcause,
    output logic [3:0]  o_wb_op_class,
    output logic [31:0] o_wb_value,
    output logic [SB_IDX_W-1:0] o_wb_phys_dst,
    output logic        o_wb_fault_pending,
    output logic [3:0]  o_wb_fault_vec,

    output logic        o_mem_pending,
    output logic        o_complete,
    output logic        o_complete_we,
    output logic [SB_IDX_W-1:0] o_complete_dest,
    output logic [31:0] o_complete_value,
    output logic        o_complete_fault,
    output logic [31:0] o_complete_pc,
    output logic [31:0] o_complete_vaddr,
    output logic [31:0] o_complete_status,

    output logic        o_fwd_valid,
    output logic [31:0] o_fwd_result,
    output logic [SB_IDX_W-1:0] o_fwd_dst,
    output logic        o_fwd_we,

    // L1-D store strobe, as MEM2 drives it; a store entering the buffer
    // takes the same data and lanes
    output logic        o_dcache_we,
    output logic [31:0] o_dcache_wdata,
    output logic [3:0]  o_dcache_byte_en
);

    localparam int D_WAYS   = 2;
    localparam int TLB_WAYS = 2;

    // -- Bundle + payload assembly --------------------------------
    ctrl_bundle_t   bundle;
    dpath_payload_t payload;
    penumbra3_decode u_decode (.i_ir (i_ir), .o_bundle (bundle));
    always_comb begin
        payload               = '0;
        payload.pc            = i_pc;
        payload.value         = i_ea;
        payload.phys_dst      = i_phys_dst;
        payload.phys_dst_we   = i_phys_dst_we;
        payload.fault_pending = i_fault_pending;
    end

    // -- Spine-style freeze: registered mem_pending OR external stall --
    logic hold;
    assign hold = o_mem_pending | i_ext_stall;

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
    // The TLB is undefined after reset and nothing here invalidates it: it
    // starts empty because simulation starts block RAM at zero.
    logic [31:0] tr_paddr;
    logic        tr_cacheable, tr_hit, tr_miss_fault, tr_prot_fault;
    logic [TLB_WAYS-1:0][19:0] tr_way_frame;
    logic [TLB_WAYS-1:0]       tr_way_sel;
    typedef logic tlb_way_t;
    logic [TLB_WAYS-1:0]       tlb_wr_way_en;
    logic [TLB_WAYS-1:0][31:0] tlb_wr_pte_word;
    always_comb
        for (int w = 0; w < TLB_WAYS; w++) begin
            tlb_wr_way_en[w]   = i_tlb_wr_en && (i_tlb_wr_way == tlb_way_t'(w));
            tlb_wr_pte_word[w] = i_tlb_wr_pte_word;
        end
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
        .i_wr_set          (i_tlb_wr_set),
        .i_wr_vpn_en       (tlb_wr_way_en),
        .i_wr_vpn_word     (i_tlb_wr_vpn_word),
        .i_wr_pte_en       (tlb_wr_way_en),
        .i_wr_pte_word     (tlb_wr_pte_word)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // -- L1-D ----------------------------------------------------
    // The frame candidates as the MMU presents them: candidate 0 is the
    // bypass / pinned frame, which this wrapper has none of, and the rest
    // are the main TLB's ways.
    logic                 dl1_hit, dl1_busy, dl1_done, dl1_fault;
    logic [31:0]          dl1_word, dl1_access_word, dcache_access_addr;
    logic [3:0]           dcache_byte_en;
    logic                 dcache_count_read, dcache_count_write;
    logic                 dcache_access_req, dcache_access_we, dcache_access_cacheable;

    /* verilator lint_off PINCONNECTEMPTY */
    penumbra3_l1 #(
        .WAYS       (D_WAYS),
        .LINE_BYTES (UNCORE_LINE_WORDS * 4),
        .FRAMES     (TLB_WAYS + 1)
    ) u_dl1 (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_launch           (dc_en && !hold),
        .i_offset           (dc_vaddr[11:0]),
        .i_discard          (1'b0),
        .i_frame            ({tr_way_frame, 20'b0}),
        .i_frame_sel        ({tr_way_sel, 1'b0}),
        .o_way_hit          (),
        .o_way_word         (),
        .o_hit              (dl1_hit),
        .o_word             (dl1_word),
        .i_store            (o_dcache_we),
        .i_store_wdata      (o_dcache_wdata),
        .i_store_byte_en    (dcache_byte_en),
        .i_count_read       (dcache_count_read),
        .i_count_write      (dcache_count_write),
        .i_access_req       (dcache_access_req),
        .i_access_we        (dcache_access_we),
        .i_access_addr      (dcache_access_addr),
        .i_access_cacheable (dcache_access_cacheable),
        .i_access_byte_en   (dcache_byte_en),
        .i_access_wdata     (o_dcache_wdata),
        .o_busy             (dl1_busy),
        .o_access_done      (dl1_done),
        .o_access_word      (dl1_access_word),
        .o_access_fault     (dl1_fault),
        .o_store_fault      (),
        .o_store_fault_addr (),
        .o_req_valid        (o_dreq_valid),
        .o_req_cacheable    (o_dreq_cacheable),
        .o_req_addr         (o_dreq_addr),
        .o_req_we           (o_dreq_we),
        .o_req_byte_en      (o_dreq_byte_en),
        .o_req_wdata        (o_dreq_wdata),
        .i_rsp_done         (i_drsp_done),
        .i_rsp_fault        (i_rsp_fault),
        .i_rsp_line         (i_rsp_line),
        .i_rsp_store_fault  (1'b0),
        .i_rsp_store_fault_addr ('0),
        .i_sys_reg          (i_l1_sys_reg),
        .i_sys_wdata        (i_l1_sys_wdata),
        .i_sys_we           (i_l1_sys_we),
        .i_sys_re           (i_l1_sys_re),
        .o_sys_rdata        (o_l1_sys_rdata)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // -- MEM2 resolve ---------------------------------------------
    /* verilator lint_off UNUSEDSIGNAL */
    ctrl_bundle_t   wb_bundle;       // only op_class fanned out
    dpath_payload_t wb_payload;      // value / dst / fault fanned out
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
        .i_dcache_hit           (dl1_hit),
        .i_dcache_rdata         (dl1_word),
        .o_dcache_we            (o_dcache_we),
        .o_dcache_wdata         (o_dcache_wdata),
        .o_dcache_byte_en       (dcache_byte_en),
        .o_dcache_count_read    (dcache_count_read),
        .o_dcache_count_write   (dcache_count_write),
        .o_dcache_access_req    (dcache_access_req),
        .o_dcache_access_we     (dcache_access_we),
        .o_dcache_access_addr   (dcache_access_addr),
        .o_dcache_access_cacheable (dcache_access_cacheable),
        .i_dcache_busy          (dl1_busy),
        .i_dcache_access_done   (dl1_done),
        .i_dcache_access_word   (dl1_access_word),
        .i_dcache_access_fault  (dl1_fault),
        .o_store_enq            (o_store_enq),
        .o_store_addr           (o_store_addr),
        .i_store_full           (i_store_full),
        .i_sys_rdata            (32'b0),
        .o_mem_pending          (o_mem_pending),
        .o_complete             (o_complete),
        .o_complete_we          (o_complete_we),
        .o_complete_dest        (o_complete_dest),
        .o_complete_value       (o_complete_value),
        .o_complete_fault       (o_complete_fault),
        .o_complete_pc          (o_complete_pc),
        .o_complete_vaddr       (o_complete_vaddr),
        .o_complete_status      (o_complete_status),
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
        .o_wb_bcause            (o_wb_bcause)
    );

    assign o_dcache_byte_en   = dcache_byte_en;
    assign o_wb_op_class      = wb_bundle.op_class;
    assign o_wb_value         = wb_payload.value;
    assign o_wb_phys_dst      = wb_payload.phys_dst;
    assign o_wb_fault_pending = wb_payload.fault_pending;
    assign o_wb_fault_vec     = wb_payload.fault_vec;

endmodule
