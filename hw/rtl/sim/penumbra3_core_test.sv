// Unit-test wrapper for penumbra3_core.
//
// The core with a real L1-I on its fetch side and an identity translation:
// every frame is the fetch's own, and i_cacheable is the page verdict for the
// whole run. The L1-I's side of the core-to-uncore port is passed through
// flat for the testbench to answer. The D-side verdicts are tied benign, so
// the programs are ALU + branch only; the L1-D's store fault is the
// testbench's to drive.
//
// i_rst resets the L1 and the core, i_core_rst the core alone: holding the
// core in reset a while longer lets the testbench enable the L1 ahead of the
// first fetch.
module penumbra3_core_test
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic                i_clk,
    input  logic                i_rst,
    input  logic                i_core_rst,
    input  logic                i_irq,
    input  logic                i_cacheable,

    // The L1-I's cache device
    input  logic [3:0]          i_l1_sys_reg,
    input  logic [31:0]         i_l1_sys_wdata,
    input  logic                i_l1_sys_we,
    input  logic                i_l1_sys_re,
    output logic [31:0]         o_l1_sys_rdata,

    // Core-to-uncore port, fetch side: the L1-I's
    output logic                o_ireq_valid,
    output logic                o_ireq_cacheable,
    output logic [31:0]         o_ireq_addr,
    input  logic                i_irsp_done,
    input  logic                i_rsp_fault,
    input  logic [UNCORE_LINE_WORDS-1:0][31:0] i_rsp_line,

    // The L1-D's store fault: the bus faulted an earlier cacheable write
    input  logic                i_store_fault,
    input  logic [31:0]         i_store_fault_addr,

    // Observability
    output logic [SB_IDX_W-1:0] o_commit_idx,
    output logic [31:0]         o_commit_data,
    output logic                o_commit_we,
    output logic                o_fault_commit,
    output logic [3:0]          o_fault_vec,
    output logic                o_mmu_fault_commit,
    output logic [31:0]         o_mmu_fault_vaddr,
    output logic [31:0]         o_mmu_fault_status,

    // Trace: the retiring instruction, drain-commits, taken branches, and
    // the slots in EX and MEM2
    output logic                o_retire_valid,
    output logic [31:0]         o_retire_pc,
    output logic [3:0]          o_retire_op_class,
    output logic [31:0]         o_retire_sr,
    output logic                o_dc_commit,
    output logic [31:0]         o_dc_commit_pc,
    output logic [3:0]          o_dc_commit_op_class,
    output logic                o_branch_taken,
    output logic [31:0]         o_branch_target,
    output logic [31:0]         o_branch_pc,
    // Inside the core, for the predictor's cases: EX's redirect (a
    // mispredict, of the transfer at o_branch_pc) and IF1's steer by a
    // prediction (of the slot at o_predict_pc), each a cycle's pulse.
    output logic                o_mispredict,
    output logic                o_predict,
    output logic [31:0]         o_predict_pc,
    output logic [31:0]         o_ex_pc,
    output logic                o_ex_valid,
    output logic [31:0]         o_mem_pc,
    output logic                o_mem_valid
);

    op_class_e retire_op_class, dc_commit_op_class;
    assign o_retire_op_class    = retire_op_class;
    assign o_dc_commit_op_class = dc_commit_op_class;

    localparam int WAYS   = 2;
    localparam int FRAMES = 3;

    logic [31:0]           fetch_addr, vaddr_q, access_addr, access_word;
    logic                  fetch_en, count_read, discard, hit;
    logic                  access_req, access_cacheable, busy, access_done, access_fault;
    logic [WAYS-1:0]       way_hit;
    logic [WAYS-1:0][31:0] way_word;

    always_ff @(posedge i_clk)
        if (fetch_en) vaddr_q <= fetch_addr;

    /* verilator lint_off PINCONNECTEMPTY */
    penumbra3_l1 #(
        .WAYS   (WAYS),
        .FRAMES (FRAMES)
    ) u_il1 (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_launch           (fetch_en),
        .i_offset           (fetch_addr[11:0]),
        .i_discard          (discard),
        .i_frame            ({FRAMES{vaddr_q[31:12]}}),
        .i_frame_sel        (3'b001),
        .o_way_hit          (way_hit),
        .o_way_word         (way_word),
        .o_hit              (hit),
        .o_word             (),
        .i_store            (1'b0),
        .i_store_wdata      ('0),
        .i_store_byte_en    ('0),
        .i_count_read       (count_read),
        .i_count_write      (1'b0),
        .i_access_req       (access_req),
        .i_access_we        (1'b0),
        .i_access_addr      (access_addr),
        .i_access_cacheable (access_cacheable),
        .i_access_byte_en   (4'hF),
        .i_access_wdata     ('0),
        .o_busy             (busy),
        .o_access_done      (access_done),
        .o_access_word      (access_word),
        .o_access_fault     (access_fault),
        .o_store_fault      (),
        .o_store_fault_addr (),
        .o_req_valid        (o_ireq_valid),
        .o_req_cacheable    (o_ireq_cacheable),
        .o_req_addr         (o_ireq_addr),
        .o_req_we           (),
        .o_req_byte_en      (),
        .o_req_wdata        (),
        .i_rsp_done         (i_irsp_done),
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

    penumbra3_core #(
        .IL1_WAYS (WAYS)
    ) u_core (
        .i_clk                    (i_clk),
        .i_rst                    (i_rst | i_core_rst),
        .i_irq                    (i_irq),
        .i_timer_irq              (1'b0),
        .o_fetch_addr             (fetch_addr),
        .o_fetch_en               (fetch_en),
        .o_fetch_user             (),
        .i_fetch_paddr            (vaddr_q),
        .i_fetch_cacheable        (i_cacheable),
        .i_fetch_mmu_fault        (1'b0),
        .i_fetch_mmu_fault_status ('0),
        .i_icache_way_hit         (way_hit),
        .i_icache_way_word        (way_word),
        .i_icache_hit             (hit),
        .o_icache_count_read      (count_read),
        .o_icache_discard         (discard),
        .o_icache_access_req      (access_req),
        .o_icache_access_addr     (access_addr),
        .o_icache_access_cacheable (access_cacheable),
        .i_icache_busy            (busy),
        .i_icache_access_done     (access_done),
        .i_icache_access_word     (access_word),
        .i_icache_access_fault    (access_fault),
        .o_translate_en           (),
        .o_translate_vaddr        (),
        .o_translate_acc_type     (),
        .o_mem_launch_hold        (),
        .o_user_mode              (),
        .i_translate_paddr        ('0),
        .i_translate_cacheable    (1'b1),
        .i_translate_hit          (1'b1),
        .i_translate_miss_fault   (1'b0),
        .i_translate_prot_fault   (1'b0),
        .o_dcache_en              (),
        .o_dcache_vaddr           (),
        .i_dcache_hit             (1'b1),
        .i_dcache_rdata           ('0),
        .o_dcache_we              (),
        .o_dcache_wdata           (),
        .o_dcache_byte_en         (),
        .o_dcache_count_read      (),
        .o_dcache_count_write     (),
        .o_dcache_access_req      (),
        .o_dcache_access_we       (),
        .o_dcache_access_addr     (),
        .o_dcache_access_cacheable (),
        .i_dcache_busy            (1'b0),
        .i_dcache_access_done     (1'b0),
        .i_dcache_access_word     ('0),
        .i_dcache_access_fault    (1'b0),
        .i_dcache_store_fault     (i_store_fault),
        .i_dcache_store_fault_addr (i_store_fault_addr),
        .o_sys_re                 (),
        .o_sys_dev                (),
        .o_sys_reg                (),
        .i_sys_rdata              ('0),
        .o_sys_we                 (),
        .o_sys_wr_dev             (),
        .o_sys_wr_reg             (),
        .o_sys_wdata              (),
        .i_sys_busy               (1'b0),
        .o_mmu_fault_commit       (o_mmu_fault_commit),
        .o_mmu_fault_vaddr        (o_mmu_fault_vaddr),
        .o_mmu_fault_status       (o_mmu_fault_status),
        .o_commit_idx             (o_commit_idx),
        .o_commit_data            (o_commit_data),
        .o_commit_we              (o_commit_we),
        .o_insn_committed         (),
        .o_insn_retired           (),
        .o_retire_valid           (o_retire_valid),
        .o_retire_pc              (o_retire_pc),
        .o_retire_op_class        (retire_op_class),
        .o_retire_sr              (o_retire_sr),
        .o_bcause                 (),
        .o_fault_commit           (o_fault_commit),
        .o_fault_vec              (o_fault_vec),
        .o_eret_commit            (),
        .o_dc_commit              (o_dc_commit),
        .o_dc_commit_pc           (o_dc_commit_pc),
        .o_dc_commit_op_class     (dc_commit_op_class),
        .o_branch_taken           (o_branch_taken),
        .o_branch_target          (o_branch_target),
        .o_branch_pc              (o_branch_pc),
        .o_ex_pc                  (o_ex_pc),
        .o_ex_valid               (o_ex_valid),
        .o_mem_pc                 (o_mem_pc),
        .o_mem_valid              (o_mem_valid)
    );
    /* verilator lint_on PINCONNECTEMPTY */

    assign o_mispredict = u_core.ex_redirect;
    assign o_predict    = u_core.u_if1.predict && !u_core.if1_redirect;
    assign o_predict_pc = u_core.if1_pc;

endmodule
