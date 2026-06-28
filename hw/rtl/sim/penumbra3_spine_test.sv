// Unit-test wrapper for penumbra3_spine.
//
// Drives the spine as a pipeline: a raw instruction word is decoded into the
// ctrl_bundle_t the fetch FIFO would present (the IF2 enqueue path), and the
// external verdicts the spine consumes -- translation, D-cache, sysreg read,
// bus fill -- are passed through flat so the testbench supplies them on the
// right cycle. Commit/retire/branch/fault observability is fanned out flat.
//
// This exercises the spine as a whole: issue, forwarding, the scoreboard
// interlock, the freeze distribution, and commit -- without the real MMU/cache
// (those attach at the machine layer).
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

    // D-cache (testbench-driven resolve)
    output logic        o_dcache_en,
    output logic [31:0] o_dcache_vaddr,
    input  logic        i_dcache_hit,
    input  logic [31:0] i_dcache_rdata,
    output logic        o_dcache_we,
    output logic [31:0] o_dcache_paddr,
    output logic [31:0] o_dcache_wdata,
    output logic [3:0]  o_dcache_byte_en,

    // Sysreg
    output logic        o_sys_re,
    output logic [3:0]  o_sys_dev,
    output logic [3:0]  o_sys_reg,
    input  logic [31:0] i_sys_rdata,
    output logic        o_sys_we,
    output logic [3:0]  o_sys_wr_dev,
    output logic [3:0]  o_sys_wr_reg,
    output logic [31:0] o_sys_wdata,

    // Bus fill
    output logic        o_launch_fill,
    output logic [31:0] o_fill_paddr,
    output logic        o_fill_cacheable,
    input  logic        i_fill_done,
    input  logic [31:0] i_fill_data,
    input  logic        i_fill_fault,

    // Branch / exception / interrupt
    output logic        o_branch_taken,
    output logic [31:0] o_branch_target,
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
    input  logic        i_irq_inject,
    input  logic [3:0]  i_irq_vec,

    // Observability
    output logic        o_insn_committed,
    output logic [2:0]  o_bcause,
    output logic        o_retire_valid,
    output logic [31:0] o_retire_pc,
    output logic [SB_IDX_W-1:0] o_commit_idx,
    output logic [31:0] o_commit_data,
    output logic        o_commit_we
);

    ctrl_bundle_t bundle;
    penumbra3_decode u_decode (.i_ir (i_ir), .o_bundle (bundle));

    bcause_e bcause_out;
    assign o_bcause = bcause_out;

    penumbra3_spine u_spine (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_bundle(bundle), .i_pc(i_pc), .i_next_pc(i_next_pc), .i_valid(i_valid),
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
        .i_dcache_hit(i_dcache_hit), .i_dcache_rdata(i_dcache_rdata),
        .o_dcache_paddr(o_dcache_paddr), .o_dcache_we(o_dcache_we),
        .o_dcache_wdata(o_dcache_wdata), .o_dcache_byte_en(o_dcache_byte_en),
        .o_sys_re(o_sys_re), .o_sys_dev(o_sys_dev), .o_sys_reg(o_sys_reg),
        .i_sys_rdata(i_sys_rdata),
        .o_sys_we(o_sys_we), .o_sys_wr_dev(o_sys_wr_dev), .o_sys_wr_reg(o_sys_wr_reg),
        .o_sys_wdata(o_sys_wdata),
        .o_launch_fill(o_launch_fill), .o_fill_paddr(o_fill_paddr),
        .o_fill_cacheable(o_fill_cacheable),
        .i_fill_done(i_fill_done), .i_fill_data(i_fill_data), .i_fill_fault(i_fill_fault),
        .o_branch_taken(o_branch_taken), .o_branch_target(o_branch_target),
        .o_fault_commit(o_fault_commit), .o_fault_vec(o_fault_vec), .o_epc(o_epc),
        .o_eret_commit(o_eret_commit),
        .o_wrsys_resync(o_wrsys_resync), .o_wrsys_resync_pc(o_wrsys_resync_pc),
        .o_mmu_fault_commit(o_mmu_fault_commit), .o_mmu_fault_vaddr(o_mmu_fault_vaddr),
        .o_mmu_fault_status(o_mmu_fault_status),
        .o_sr_s(o_sr_s), .o_sr_i(o_sr_i), .o_ei_commit(o_ei_commit), .o_dc_commit(o_dc_commit),
        .i_irq_inject(i_irq_inject), .i_irq_vec(i_irq_vec),
        .o_insn_committed(o_insn_committed), .o_bcause(bcause_out),
        .o_retire_valid(o_retire_valid), .o_retire_pc(o_retire_pc),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data), .o_commit_we(o_commit_we)
    );

endmodule
