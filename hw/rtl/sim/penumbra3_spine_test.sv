// Unit-test wrapper for penumbra3_spine.
//
// Drives the spine as a pipeline: a raw instruction word is decoded into the
// ctrl_bundle_t the fetch FIFO would present (the IF2 enqueue path), and the
// external verdicts the spine consumes -- translation, sysreg read, the
// uncore's response -- are passed through flat so the testbench supplies
// them on the right cycle. Commit/retire/branch/fault observability is
// fanned out flat.
//
// The L1-D is a real penumbra3_l1, left disabled: every lookup misses, and
// every load and uncached store goes to the uncore, which the testbench
// plays at the L1's port. A cacheable store goes to the store buffer, which
// the testbench plays at the spine's store port. The translation's frame is
// the L1's one candidate.
//
// This exercises the spine as a whole: issue, forwarding, the scoreboard
// interlock, the freeze distribution, and commit -- without the real MMU
// (it attaches at the machine layer).
module penumbra3_spine_test
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // Fetch-FIFO head (instruction word -> decoded bundle)
    input  logic [31:0] i_ir,
    input  logic [31:0] i_pc,
    input  logic [31:0] i_next_pc,
    input  logic        i_valid,
    input  logic        i_if_fault_pending,
    input  logic [3:0]  i_if_fault_vec,
    input  logic [31:0] i_if_fault_status,
    input  logic        i_fetch_busy,
    output logic        o_deq_ready,

    // Translation verdict (testbench-driven)
    output logic        o_translate_en,
    output logic [31:0] o_translate_vaddr,
    output logic [2:0]  o_translate_acc_type,
    output logic        o_mem_launch_hold,
    output logic        o_user_mode,
    input  logic [31:0] i_translate_paddr,
    input  logic        i_translate_cacheable,
    input  logic        i_translate_hit,
    input  logic        i_translate_miss_fault,
    input  logic        i_translate_prot_fault,

    // L1-D launch and store strobe, as the spine drives them
    output logic        o_dcache_en,
    output logic [31:0] o_dcache_vaddr,
    output logic        o_dcache_we,
    output logic [31:0] o_dcache_wdata,
    output logic [3:0]  o_dcache_byte_en,
    output logic        o_dcache_count_read,
    output logic        o_dcache_count_write,

    // Sysreg
    output logic        o_sys_re,
    output logic [3:0]  o_sys_dev,
    output logic [3:0]  o_sys_reg,
    input  logic [31:0] i_sys_rdata,
    output logic        o_sys_we,
    output logic [3:0]  o_sys_wr_dev,
    output logic [3:0]  o_sys_wr_reg,
    output logic [31:0] o_sys_wdata,

    // Core-to-uncore port, data side: the L1-D's
    output logic        o_dreq_valid,
    output logic        o_dreq_cacheable,
    output logic [31:0] o_dreq_addr,
    output logic        o_dreq_we,
    output logic [3:0]  o_dreq_byte_en,
    output logic [31:0] o_dreq_wdata,
    input  logic        i_drsp_done,
    input  logic        i_rsp_fault,
    input  logic [UNCORE_LINE_WORDS-1:0][31:0] i_rsp_line,

    // Store buffer: a cacheable store retiring (data and lanes on the L1-D
    // store strobe)
    output logic        o_store_enq,
    output logic [31:0] o_store_addr,
    input  logic        i_store_full,

    // Branch / exception / interrupt
    output logic        o_redirect,
    output logic [31:0] o_redirect_pc,
    output logic        o_branch_taken,
    output logic [31:0] o_branch_target,
    output logic        o_train,
    output logic [31:0] o_train_pc,
    output dir_counter_t o_train_counter,
    output logic        o_fault_commit,
    output logic [3:0]  o_fault_vec,
    output logic [31:0] o_epc,
    output logic        o_eret_commit,
    output logic        o_wrsys_resync,
    output logic [31:0] o_wrsys_resync_pc,
    output logic        o_mmu_fault_commit,
    output logic [31:0] o_mmu_fault_vaddr,
    output logic [31:0] o_mmu_fault_status,
    output logic        o_sr_s,
    output logic        o_sr_i,
    output logic        o_ei_commit,
    output logic        o_dc_commit,
    output logic        o_ex_valid,
    output logic        o_ex_stall,
    input  logic        i_irq_inject,
    input  logic [3:0]  i_irq_vec,
    input  logic        i_deferred_inject,
    input  logic [31:0] i_deferred_addr,

    // Observability
    output logic        o_insn_committed,
    output logic [2:0]  o_bcause,
    output logic        o_retire_valid,
    output logic [31:0] o_retire_pc,
    output logic [3:0]  o_retire_op_class,
    output logic [31:0] o_retire_sr,
    output logic [SB_IDX_W-1:0] o_commit_idx,
    output logic [31:0] o_commit_data,
    output logic        o_commit_we,
    output logic [31:0] o_dc_commit_pc,
    output logic [3:0]  o_dc_commit_op_class,
    output logic [31:0] o_branch_pc,
    output logic [31:0] o_ex_pc,
    output logic [31:0] o_mem_pc,
    output logic        o_mem_valid
);

    ctrl_bundle_t bundle;
    penumbra3_decode u_decode (.i_ir (i_ir), .o_bundle (bundle));

    bcause_e   bcause_out;
    op_class_e retire_op_class, dc_commit_op_class;
    assign o_bcause             = bcause_out;
    assign o_retire_op_class    = retire_op_class;
    assign o_dc_commit_op_class = dc_commit_op_class;

    // -- L1-D -----------------------------------------------------
    logic        dl1_hit, dl1_busy, dl1_done, dl1_fault;
    logic [31:0] dl1_word, dl1_access_word, dcache_access_addr;
    logic        dcache_access_req, dcache_access_we, dcache_access_cacheable;

    /* verilator lint_off PINCONNECTEMPTY */
    penumbra3_l1 #(
        .WAYS       (2),
        .LINE_BYTES (UNCORE_LINE_WORDS * 4),
        .FRAMES     (1)
    ) u_dl1 (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_launch           (o_dcache_en && !o_mem_launch_hold),
        .i_offset           (o_dcache_vaddr[11:0]),
        .i_discard          (1'b0),
        .i_frame            (i_translate_paddr[31:12]),
        .i_frame_sel        (i_translate_hit),
        .o_way_hit          (),
        .o_way_word         (),
        .o_hit              (dl1_hit),
        .o_word             (dl1_word),
        .i_store            (o_dcache_we),
        .i_store_wdata      (o_dcache_wdata),
        .i_store_byte_en    (o_dcache_byte_en),
        .i_count_read       (o_dcache_count_read),
        .i_count_write      (o_dcache_count_write),
        .i_access_req       (dcache_access_req),
        .i_access_we        (dcache_access_we),
        .i_access_addr      (dcache_access_addr),
        .i_access_cacheable (dcache_access_cacheable),
        .i_access_byte_en   (o_dcache_byte_en),
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
        .i_sys_reg          (4'b0),
        .i_sys_wdata        (32'b0),
        .i_sys_we           (1'b0),
        .i_sys_re           (1'b0),
        .o_sys_rdata        ()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    // Every slot arrives predicted not taken, as from a front end that
    // predicts nothing: each taken transfer is EX's redirect.
    prediction_t prediction;
    assign prediction = '{taken: 1'b0, target: 32'b0, counter: '0};

    penumbra3_spine u_spine (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bundle(bundle), .i_pc(i_pc), .i_next_pc(i_next_pc),
        .i_prediction(prediction), .i_valid(i_valid),
        .i_if_fault_pending(i_if_fault_pending), .i_if_fault_vec(i_if_fault_vec),
        .i_if_fault_status(i_if_fault_status), .i_fetch_busy(i_fetch_busy),
        .o_deq_ready(o_deq_ready),
        .o_translate_en(o_translate_en), .o_translate_vaddr(o_translate_vaddr),
        .o_translate_acc_type(o_translate_acc_type), .o_mem_launch_hold(o_mem_launch_hold),
        .o_user_mode(o_user_mode),
        .i_translate_paddr(i_translate_paddr), .i_translate_cacheable(i_translate_cacheable),
        .i_translate_hit(i_translate_hit), .i_translate_miss_fault(i_translate_miss_fault),
        .i_translate_prot_fault(i_translate_prot_fault),
        .o_dcache_en(o_dcache_en), .o_dcache_vaddr(o_dcache_vaddr),
        .i_dcache_hit(dl1_hit), .i_dcache_rdata(dl1_word),
        .o_dcache_we(o_dcache_we),
        .o_dcache_wdata(o_dcache_wdata), .o_dcache_byte_en(o_dcache_byte_en),
        .o_dcache_count_read(o_dcache_count_read),
        .o_dcache_count_write(o_dcache_count_write),
        .o_dcache_access_req(dcache_access_req), .o_dcache_access_we(dcache_access_we),
        .o_dcache_access_addr(dcache_access_addr),
        .o_dcache_access_cacheable(dcache_access_cacheable),
        .i_dcache_busy(dl1_busy), .i_dcache_access_done(dl1_done),
        .i_dcache_access_word(dl1_access_word), .i_dcache_access_fault(dl1_fault),
        .o_store_enq(o_store_enq), .o_store_addr(o_store_addr),
        .i_store_full(i_store_full),
        .o_sys_re(o_sys_re), .o_sys_dev(o_sys_dev), .o_sys_reg(o_sys_reg),
        .i_sys_rdata(i_sys_rdata),
        .o_sys_we(o_sys_we), .o_sys_wr_dev(o_sys_wr_dev), .o_sys_wr_reg(o_sys_wr_reg),
        .o_sys_wdata(o_sys_wdata),
        .o_redirect(o_redirect), .o_redirect_pc(o_redirect_pc),
        .o_branch_taken(o_branch_taken), .o_branch_target(o_branch_target),
        .o_train(o_train), .o_train_pc(o_train_pc), .o_train_counter(o_train_counter),
        .o_fault_commit(o_fault_commit), .o_fault_vec(o_fault_vec), .o_epc(o_epc),
        .o_eret_commit(o_eret_commit),
        .o_wrsys_resync(o_wrsys_resync), .o_wrsys_resync_pc(o_wrsys_resync_pc),
        .o_mmu_fault_commit(o_mmu_fault_commit), .o_mmu_fault_vaddr(o_mmu_fault_vaddr),
        .o_mmu_fault_status(o_mmu_fault_status),
        .o_sr_s(o_sr_s), .o_sr_i(o_sr_i), .o_ei_commit(o_ei_commit), .o_dc_commit(o_dc_commit),
        .o_ex_valid(o_ex_valid), .o_ex_stall(o_ex_stall),
        .i_irq_inject(i_irq_inject), .i_irq_vec(i_irq_vec),
        .i_deferred_inject(i_deferred_inject), .i_deferred_addr(i_deferred_addr),
        .o_insn_committed(o_insn_committed), .o_bcause(bcause_out),
        .o_retire_valid(o_retire_valid), .o_retire_pc(o_retire_pc),
        .o_retire_op_class(retire_op_class), .o_retire_sr(o_retire_sr),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data), .o_commit_we(o_commit_we),
        .o_dc_commit_pc(o_dc_commit_pc), .o_dc_commit_op_class(dc_commit_op_class),
        .o_branch_pc(o_branch_pc),
        .o_ex_pc(o_ex_pc), .o_mem_pc(o_mem_pc), .o_mem_valid(o_mem_valid)
    );

endmodule
