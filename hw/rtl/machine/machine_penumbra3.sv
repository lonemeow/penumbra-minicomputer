// machine_penumbra3 -- the Penumbra/3 machine: the core, the MMU, the two
// L1s, the uncore and the sysreg devices, behind the gen2 machine's port
// list. doc/internals/penumbra3/machine.md specifies how the parts compose.

module machine_penumbra3
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
#(
    parameter logic [31:0] RESET_PC        = 32'hFFFF_0000,

    // Cache geometry: the board's, sized to its block RAM; software reads
    // it from each cache's INFO. An L1 way is one page.
    parameter int          L1_WAYS         = 2,
    parameter int          L2_SETS         = 1024,
    parameter int          L2_WAYS         = 4,

    // Main-TLB sets: the board's; software reads them from MMU_CONFIG.
    // A copy of the TLB is a block RAM per way and word whatever the set
    // count up to its depth, so the count costs no block RAM there.
    parameter int          TLB_SETS        = 256,

    // Machine identity (SYSDEV_MACH): the board's, which the wrapper or
    // board top supplies. The defaults describe an unnamed machine at an
    // unknown clock.
    parameter logic [31:0] MACH_FEAT_VALUE = 32'd0,
    parameter logic [31:0] MACH_NAME0      = 32'h00000000,
    parameter logic [31:0] MACH_NAME1      = 32'h00000000,
    parameter logic [31:0] MACH_NAME2      = 32'h00000000,
    parameter logic [31:0] MACH_NAME3      = 32'h00000000,
    parameter logic [31:0] CPU_FREQ        = 32'd0
)(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // ── Interrupt request inputs ─────────────────────────────────
    input  logic                  i_irq,
    input  logic                  i_timer_irq,

    // ── External Penumbra Bus (master side; devices attach here) ─
    output logic [31:0]           o_bus_addr,
    output logic [31:0]           o_bus_wdata,
    output logic [3:0]            o_bus_byte_en,
    output logic                  o_bus_re,
    output logic                  o_bus_we,
    input  logic [31:0]           i_bus_rdata,
    input  logic                  i_bus_ack,
    input  logic                  i_bus_fault,   // valid with i_bus_ack

    // ── Bus autoconfig control (to the wrapper's autoconfig chain) ─
    output logic                  o_bus_rst,     // BUSCTL.RST
    output logic                  o_bus_cfg_en,  // BUSCTL.CFG_EN

    // ── Commit / retire observability ────────────────────────────
    output logic [SB_IDX_W-1:0]   o_commit_idx,
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we,
    output logic                  o_retire_valid,
    output op_class_e             o_retire_op_class,
    output logic [31:0]           o_retire_pc,
    output logic [31:0]           o_retire_sr,
    output logic                  o_fault_commit,    // a fault is taken
    output logic [3:0]            o_fault_vec,       // its vector number
    output logic                  o_eret_commit,
    output logic                  o_dc_commit,       // a drain-commit retires
    output logic [31:0]           o_dc_commit_pc,
    output op_class_e             o_dc_commit_op_class,
    output logic                  o_branch_taken,    // a branch resolves taken
    output logic [31:0]           o_branch_target,
    output logic [31:0]           o_branch_pc,
    output logic [31:0]           o_ex_pc,           // the slots in EX and MEM
    output logic                  o_ex_valid,
    output logic [31:0]           o_mem_pc,
    output logic                  o_mem_valid,

    // ── Program end: a BREAK's trap commits ──────────────────────
    output logic                  o_prog_end
);

    localparam int TLB_WAYS   = 2;
    localparam int FRAMES     = TLB_WAYS + 1;          // the MMU's candidates
    localparam int LINE_BYTES = UNCORE_LINE_WORDS * 4;

    // Outputs no part here consumes: the MMU's D hit, the L1-D's per-way
    // verdict, the L1-I's selected word and its write side.
    /* verilator lint_off PINCONNECTEMPTY */

    // ── Fetch side ───────────────────────────────────────────────
    logic [31:0]                     fetch_addr, fetch_paddr, fetch_fault_status;
    logic                            fetch_en, fetch_user, fetch_cacheable, fetch_fault;
    logic [FRAMES-1:0][19:0]         i_frame;
    logic [FRAMES-1:0]               i_frame_sel;
    logic [L1_WAYS-1:0]              il1_way_hit;
    logic [L1_WAYS-1:0][31:0]        il1_way_word;
    logic                            il1_hit, il1_count_read, il1_discard;
    logic                            il1_access_req, il1_access_cacheable;
    logic [31:0]                     il1_access_addr, il1_access_word;
    logic                            il1_busy, il1_access_done, il1_access_fault;

    // ── Data side ────────────────────────────────────────────────
    logic                            translate_en, mem_launch_hold, user_mode;
    logic [31:0]                     translate_vaddr, translate_paddr;
    logic [2:0]                      translate_acc_type;
    logic                            translate_cacheable, translate_hit;
    logic                            translate_miss_fault, translate_prot_fault;
    logic [FRAMES-1:0][19:0]         d_frame;
    logic [FRAMES-1:0]               d_frame_sel;
    logic                            dl1_en, dl1_hit, dl1_we;
    logic [31:0]                     dl1_rdata, dl1_wdata;
    // The L1-D indexes by the page offset; its frames come from the MMU.
    /* verilator lint_off UNUSEDSIGNAL */
    logic [31:0]                     dl1_vaddr;
    /* verilator lint_on UNUSEDSIGNAL */
    logic [3:0]                      dl1_byte_en;
    logic                            dl1_count_read, dl1_count_write;
    logic                            dl1_access_req, dl1_access_we, dl1_access_cacheable;
    logic [31:0]                     dl1_access_addr, dl1_access_word;
    logic                            dl1_busy, dl1_access_done, dl1_access_fault;
    logic                            store_fault;
    logic [31:0]                     store_fault_addr;

    // ── MMU fault-register commit ────────────────────────────────
    logic                            mmu_fault_commit;
    logic [31:0]                     mmu_fault_vaddr, mmu_fault_status;

    // ── Sysreg side ──────────────────────────────────────────────
    logic                            sys_re, sys_we;
    logic [3:0]                      sys_dev, sys_reg, sys_wr_dev, sys_wr_reg;
    logic [31:0]                     sys_wdata, sys_rdata;
    logic                            mmu_sys_busy;      // an invalidation holds fetch
    logic [3:0]                      sys_reg_sel;

    // ── Perfctr and timer feeds, program end ─────────────────────
    logic                            insn_retired;
    bcause_e                         bcause;
    logic                            core_timer_irq;
    logic                            fault_commit;
    logic [3:0]                      fault_vec;

    // ══════════════════════════════════════════════════════════
    // The core
    // ══════════════════════════════════════════════════════════
    penumbra3_core #(
        .RESET_PC (RESET_PC),
        .IL1_WAYS (L1_WAYS)
    ) u_core (
        .i_clk                     (i_clk),
        .i_rst                     (i_rst),
        .i_irq                     (i_irq),
        .i_timer_irq               (core_timer_irq),
        .o_fetch_addr              (fetch_addr),
        .o_fetch_en                (fetch_en),
        .o_fetch_user              (fetch_user),
        .i_fetch_paddr             (fetch_paddr),
        .i_fetch_cacheable         (fetch_cacheable),
        .i_fetch_mmu_fault         (fetch_fault),
        .i_fetch_mmu_fault_status  (fetch_fault_status),
        .i_icache_way_hit          (il1_way_hit),
        .i_icache_way_word         (il1_way_word),
        .i_icache_hit              (il1_hit),
        .o_icache_count_read       (il1_count_read),
        .o_icache_discard          (il1_discard),
        .o_icache_access_req       (il1_access_req),
        .o_icache_access_addr      (il1_access_addr),
        .o_icache_access_cacheable (il1_access_cacheable),
        .i_icache_busy             (il1_busy),
        .i_icache_access_done      (il1_access_done),
        .i_icache_access_word      (il1_access_word),
        .i_icache_access_fault     (il1_access_fault),
        .o_translate_en            (translate_en),
        .o_translate_vaddr         (translate_vaddr),
        .o_translate_acc_type      (translate_acc_type),
        .o_mem_launch_hold         (mem_launch_hold),
        .o_user_mode               (user_mode),
        .i_translate_paddr         (translate_paddr),
        .i_translate_cacheable     (translate_cacheable),
        .i_translate_hit           (translate_hit),
        .i_translate_miss_fault    (translate_miss_fault),
        .i_translate_prot_fault    (translate_prot_fault),
        .o_dcache_en               (dl1_en),
        .o_dcache_vaddr            (dl1_vaddr),
        .i_dcache_hit              (dl1_hit),
        .i_dcache_rdata            (dl1_rdata),
        .o_dcache_we               (dl1_we),
        .o_dcache_wdata            (dl1_wdata),
        .o_dcache_byte_en          (dl1_byte_en),
        .o_dcache_count_read       (dl1_count_read),
        .o_dcache_count_write      (dl1_count_write),
        .o_dcache_access_req       (dl1_access_req),
        .o_dcache_access_we        (dl1_access_we),
        .o_dcache_access_addr      (dl1_access_addr),
        .o_dcache_access_cacheable (dl1_access_cacheable),
        .i_dcache_busy             (dl1_busy),
        .i_dcache_access_done      (dl1_access_done),
        .i_dcache_access_word      (dl1_access_word),
        .i_dcache_access_fault     (dl1_access_fault),
        .i_dcache_store_fault      (store_fault),
        .i_dcache_store_fault_addr (store_fault_addr),
        .o_sys_re                  (sys_re),
        .o_sys_dev                 (sys_dev),
        .o_sys_reg                 (sys_reg),
        .i_sys_rdata               (sys_rdata),
        .o_sys_we                  (sys_we),
        .o_sys_wr_dev              (sys_wr_dev),
        .o_sys_wr_reg              (sys_wr_reg),
        .o_sys_wdata               (sys_wdata),
        .i_sys_busy                (mmu_sys_busy),
        .o_mmu_fault_commit        (mmu_fault_commit),
        .o_mmu_fault_vaddr         (mmu_fault_vaddr),
        .o_mmu_fault_status        (mmu_fault_status),
        .o_commit_idx              (o_commit_idx),
        .o_commit_data             (o_commit_data),
        .o_commit_we               (o_commit_we),
        .o_insn_committed          (),
        .o_insn_retired            (insn_retired),
        .o_retire_valid            (o_retire_valid),
        .o_retire_pc               (o_retire_pc),
        .o_retire_op_class         (o_retire_op_class),
        .o_retire_sr               (o_retire_sr),
        .o_bcause                  (bcause),
        .o_fault_commit            (fault_commit),
        .o_fault_vec               (fault_vec),
        .o_eret_commit             (o_eret_commit),
        .o_dc_commit               (o_dc_commit),
        .o_dc_commit_pc            (o_dc_commit_pc),
        .o_dc_commit_op_class      (o_dc_commit_op_class),
        .o_branch_taken            (o_branch_taken),
        .o_branch_target           (o_branch_target),
        .o_branch_pc               (o_branch_pc),
        .o_ex_pc                   (o_ex_pc),
        .o_ex_valid                (o_ex_valid),
        .o_mem_pc                  (o_mem_pc),
        .o_mem_valid               (o_mem_valid)
    );

    assign o_fault_commit = fault_commit;
    assign o_fault_vec    = fault_vec;

    // BREAK is a trap like any other, and its handler runs; the pulse only
    // reports its commit, for a testbench to end a run on.
    assign o_prog_end = fault_commit && (fault_vec == VEC_BREAK);

    // ══════════════════════════════════════════════════════════
    // MMU: both translate ports and the fault registers
    // ══════════════════════════════════════════════════════════
    logic        mmu_sys_we, mmu_sys_re;
    logic [31:0] mmu_sys_rdata;

    penumbra3_mmu #(
        .SETS (TLB_SETS),
        .WAYS (TLB_WAYS)
    ) u_mmu (
        .i_clk            (i_clk),
        .i_rst            (i_rst),
        .i_i_lookup_en    (fetch_en),
        .i_i_vaddr        (fetch_addr),
        .i_i_user_mode    (fetch_user),
        .i_i_hold         (~fetch_en),
        .o_i_paddr        (fetch_paddr),
        .o_i_cacheable    (fetch_cacheable),
        .o_i_fault        (fetch_fault),
        .o_i_fault_status (fetch_fault_status),
        .o_i_frame        (i_frame),
        .o_i_frame_sel    (i_frame_sel),
        .i_d_lookup_en    (translate_en),
        .i_d_vaddr        (translate_vaddr),
        .i_d_access_type  (translate_acc_type),
        .i_d_user_mode    (user_mode),
        .i_d_hold         (mem_launch_hold),
        .o_d_paddr        (translate_paddr),
        .o_d_cacheable    (translate_cacheable),
        .o_d_hit          (translate_hit),
        .o_d_miss_fault   (translate_miss_fault),
        .o_d_prot_fault   (translate_prot_fault),
        .o_d_frame        (d_frame),
        .o_d_frame_sel    (d_frame_sel),
        .i_fault_commit   (mmu_fault_commit),
        .i_fault_vaddr    (mmu_fault_vaddr),
        .i_fault_status   (mmu_fault_status),
        .i_sys_reg        (sys_reg_sel),
        .i_sys_wdata      (sys_wdata),
        .i_sys_we         (mmu_sys_we),
        .i_sys_re         (mmu_sys_re),
        .o_sys_rdata      (mmu_sys_rdata),
        .o_sys_busy       (mmu_sys_busy)
    );

    // ══════════════════════════════════════════════════════════
    // The L1s and the uncore
    // ══════════════════════════════════════════════════════════
    logic                                ireq_valid, ireq_cacheable, irsp_done;
    logic [31:0]                         ireq_addr;
    logic                                dreq_valid, dreq_cacheable, dreq_we, drsp_done;
    logic [31:0]                         dreq_addr, dreq_wdata;
    logic [3:0]                          dreq_byte_en;
    logic                                rsp_fault, rsp_store_fault;
    logic [UNCORE_LINE_WORDS-1:0][31:0]  rsp_line;
    logic [31:0]                         rsp_store_fault_addr;

    logic        il1_sys_we, il1_sys_re, dl1_sys_we, dl1_sys_re;
    logic [31:0] il1_sys_rdata, dl1_sys_rdata;

    // The fetch side reads whole words and never writes.
    penumbra3_l1 #(
        .WAYS       (L1_WAYS),
        .LINE_BYTES (LINE_BYTES),
        .FRAMES     (FRAMES)
    ) u_il1 (
        .i_clk                  (i_clk),
        .i_rst                  (i_rst),
        .i_launch               (fetch_en),
        .i_offset               (fetch_addr[11:0]),
        .i_discard              (il1_discard),
        .i_frame                (i_frame),
        .i_frame_sel            (i_frame_sel),
        .o_way_hit              (il1_way_hit),
        .o_way_word             (il1_way_word),
        .o_hit                  (il1_hit),
        .o_word                 (),
        .i_store                (1'b0),
        .i_store_wdata          ('0),
        .i_store_byte_en        ('0),
        .i_count_read           (il1_count_read),
        .i_count_write          (1'b0),
        .i_access_req           (il1_access_req),
        .i_access_we            (1'b0),
        .i_access_addr          (il1_access_addr),
        .i_access_cacheable     (il1_access_cacheable),
        .i_access_byte_en       (4'hF),
        .i_access_wdata         ('0),
        .o_busy                 (il1_busy),
        .o_access_done          (il1_access_done),
        .o_access_word          (il1_access_word),
        .o_access_fault         (il1_access_fault),
        .o_store_fault          (),
        .o_store_fault_addr     (),
        .o_req_valid            (ireq_valid),
        .o_req_cacheable        (ireq_cacheable),
        .o_req_addr             (ireq_addr),
        .o_req_we               (),
        .o_req_byte_en          (),
        .o_req_wdata            (),
        .i_rsp_done             (irsp_done),
        .i_rsp_fault            (rsp_fault),
        .i_rsp_line             (rsp_line),
        .i_rsp_store_fault      (1'b0),
        .i_rsp_store_fault_addr ('0),
        .i_sys_reg              (sys_reg_sel),
        .i_sys_wdata            (sys_wdata),
        .i_sys_we               (il1_sys_we),
        .i_sys_re               (il1_sys_re),
        .o_sys_rdata            (il1_sys_rdata)
    );

    // The data side's launch hold freezes the L1-D's lookup with the
    // translation's, so a held slot keeps both verdicts. A store's data and
    // lanes serve its hit write and its access both.
    penumbra3_l1 #(
        .WAYS       (L1_WAYS),
        .LINE_BYTES (LINE_BYTES),
        .FRAMES     (FRAMES)
    ) u_dl1 (
        .i_clk                  (i_clk),
        .i_rst                  (i_rst),
        .i_launch               (dl1_en && !mem_launch_hold),
        .i_offset               (dl1_vaddr[11:0]),
        .i_discard              (1'b0),
        .i_frame                (d_frame),
        .i_frame_sel            (d_frame_sel),
        .o_way_hit              (),
        .o_way_word             (),
        .o_hit                  (dl1_hit),
        .o_word                 (dl1_rdata),
        .i_store                (dl1_we),
        .i_store_wdata          (dl1_wdata),
        .i_store_byte_en        (dl1_byte_en),
        .i_count_read           (dl1_count_read),
        .i_count_write          (dl1_count_write),
        .i_access_req           (dl1_access_req),
        .i_access_we            (dl1_access_we),
        .i_access_addr          (dl1_access_addr),
        .i_access_cacheable     (dl1_access_cacheable),
        .i_access_byte_en       (dl1_byte_en),
        .i_access_wdata         (dl1_wdata),
        .o_busy                 (dl1_busy),
        .o_access_done          (dl1_access_done),
        .o_access_word          (dl1_access_word),
        .o_access_fault         (dl1_access_fault),
        .o_store_fault          (store_fault),
        .o_store_fault_addr     (store_fault_addr),
        .o_req_valid            (dreq_valid),
        .o_req_cacheable        (dreq_cacheable),
        .o_req_addr             (dreq_addr),
        .o_req_we               (dreq_we),
        .o_req_byte_en          (dreq_byte_en),
        .o_req_wdata            (dreq_wdata),
        .i_rsp_done             (drsp_done),
        .i_rsp_fault            (rsp_fault),
        .i_rsp_line             (rsp_line),
        .i_rsp_store_fault      (rsp_store_fault),
        .i_rsp_store_fault_addr (rsp_store_fault_addr),
        .i_sys_reg              (sys_reg_sel),
        .i_sys_wdata            (sys_wdata),
        .i_sys_we               (dl1_sys_we),
        .i_sys_re               (dl1_sys_re),
        .o_sys_rdata            (dl1_sys_rdata)
    );

    logic        l2_sys_we, l2_sys_re;
    logic [31:0] l2_sys_rdata;

    penumbra3_uncore #(
        .SETS (L2_SETS),
        .WAYS (L2_WAYS)
    ) u_uncore (
        .i_clk              (i_clk),
        .i_rst              (i_rst),
        .i_ireq_valid       (ireq_valid),
        .i_ireq_cacheable   (ireq_cacheable),
        .i_ireq_addr        (ireq_addr),
        .o_irsp_done        (irsp_done),
        .i_dreq_valid       (dreq_valid),
        .i_dreq_cacheable   (dreq_cacheable),
        .i_dreq_addr        (dreq_addr),
        .i_dreq_we          (dreq_we),
        .i_dreq_byte_en     (dreq_byte_en),
        .i_dreq_wdata       (dreq_wdata),
        .o_drsp_done        (drsp_done),
        .o_rsp_fault        (rsp_fault),
        .o_rsp_line         (rsp_line),
        .o_store_fault      (rsp_store_fault),
        .o_store_fault_addr (rsp_store_fault_addr),
        .i_sys_reg          (sys_reg_sel),
        .i_sys_wdata        (sys_wdata),
        .i_sys_we           (l2_sys_we),
        .i_sys_re           (l2_sys_re),
        .o_sys_rdata        (l2_sys_rdata),
        .o_bus_addr         (o_bus_addr),
        .o_bus_wdata        (o_bus_wdata),
        .o_bus_byte_en      (o_bus_byte_en),
        .o_bus_re           (o_bus_re),
        .o_bus_we           (o_bus_we),
        .i_bus_rdata        (i_bus_rdata),
        .i_bus_ack          (i_bus_ack),
        .i_bus_fault        (i_bus_fault)
    );

    // ══════════════════════════════════════════════════════════
    // Sysreg devices
    // ══════════════════════════════════════════════════════════
    logic timer_sys_we, busctl_sys_we;

    assign mmu_sys_we    = sys_we && (sys_wr_dev == SYSDEV_MMU);
    assign dl1_sys_we    = sys_we && (sys_wr_dev == SYSDEV_L1_DCACHE);
    assign il1_sys_we    = sys_we && (sys_wr_dev == SYSDEV_L1_ICACHE);
    assign l2_sys_we     = sys_we && (sys_wr_dev == SYSDEV_L2_CACHE);
    assign timer_sys_we  = sys_we && (sys_wr_dev == SYSDEV_TIMER);
    assign busctl_sys_we = sys_we && (sys_wr_dev == SYSDEV_BUS);
    assign mmu_sys_re    = sys_re && (sys_dev == SYSDEV_MMU);
    assign dl1_sys_re    = sys_re && (sys_dev == SYSDEV_L1_DCACHE);
    assign il1_sys_re    = sys_re && (sys_dev == SYSDEV_L1_ICACHE);
    assign l2_sys_re     = sys_re && (sys_dev == SYSDEV_L2_CACHE);

    // A WRSYS commits into an empty pipe, so no read is in flight with it
    // (asserted below), and one selector serves both.
    always_comb begin
        if (sys_we) sys_reg_sel = sys_wr_reg;
        else        sys_reg_sel = sys_reg;
    end

    // ── CPU: identity (regs 0-4) and performance counters (5 and up) ──
    logic [31:0] cpuid_rdata, perfctr_rdata;

    cpuid #(
        .CPU_NAME2 (32'h0000332F)    // "/3\0\0"
    ) u_cpuid (
        .i_sys_reg   (sys_reg),
        .o_sys_rdata (cpuid_rdata)
    );

    penumbra3_perfctr u_perfctr (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_insn_retired (insn_retired),
        .i_bcause       (bcause),
        .i_sys_reg      (sys_reg),
        .o_sys_rdata    (perfctr_rdata)
    );

    // ── MACH: the board's identity ───────────────────────────────
    logic [31:0] machid_rdata;

    machid #(
        .MACH_FEAT_VALUE (MACH_FEAT_VALUE),
        .MACH_NAME0      (MACH_NAME0),
        .MACH_NAME1      (MACH_NAME1),
        .MACH_NAME2      (MACH_NAME2),
        .MACH_NAME3      (MACH_NAME3),
        .CPU_FREQ        (CPU_FREQ)
    ) u_machid (
        .i_sys_reg   (sys_reg),
        .o_sys_rdata (machid_rdata)
    );

    // ── TIMER, on a tick near 1 MHz ──────────────────────────────
    // The prescaler toggles its tick, since the timer edge-detects it. With
    // CPU_FREQ 0, a bare probe's, the prescaler stays legal and nothing
    // programs the timer.
    localparam int TICK_TARGET_HZ = 1_000_000;
    localparam int PRESCALE_RAW   = (CPU_FREQ + TICK_TARGET_HZ) / (2 * TICK_TARGET_HZ);
    localparam int PRESCALE_DIV   = (PRESCALE_RAW >= 2) ? PRESCALE_RAW : 2;
    localparam int TIMER_TICK_HZ  = CPU_FREQ / (2 * PRESCALE_DIV);

    typedef logic [$clog2(PRESCALE_DIV)-1:0] prescale_t;
    localparam prescale_t PRESCALE_LAST = prescale_t'(PRESCALE_DIV - 1);

    prescale_t prescale_q;
    logic      timer_tick_q;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            prescale_q   <= '0;
            timer_tick_q <= 1'b0;
        end else if (prescale_q == PRESCALE_LAST) begin
            prescale_q   <= '0;
            timer_tick_q <= ~timer_tick_q;
        end else begin
            prescale_q   <= prescale_q + 1'b1;
        end
    end

    logic [31:0] timer_rdata;
    logic        timer_irq;

    timer #(
        .TICK_FREQ_HZ (TIMER_TICK_HZ)
    ) u_timer (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_tick      (timer_tick_q),
        .i_sys_reg   (sys_reg_sel),
        .i_sys_wdata (sys_wdata),
        .i_sys_we    (timer_sys_we),
        .o_sys_rdata (timer_rdata),
        .o_irq       (timer_irq)
    );

    assign core_timer_irq = timer_irq | i_timer_irq;

    // ── BUS: bus reset and autoconfig enable, for the wrapper's chain ──
    logic [31:0] busctl_rdata;

    busctl u_busctl (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_sys_reg   (sys_reg_sel),
        .i_sys_wdata (sys_wdata),
        .i_sys_we    (busctl_sys_we),
        .o_sys_rdata (busctl_rdata),
        .o_bus_rst   (o_bus_rst),
        .o_cfg_en    (o_bus_cfg_en)
    );

    // ── Read response ────────────────────────────────────────────
    // The MMU, the L1s and the L2 answer a cycle after the read, from the
    // register they latch with it. The other devices answer in the read's
    // cycle, so their answer is registered then, and the device read picks
    // between the two. Every unassigned device reads 0.
    logic [31:0] now_rdata, now_rdata_q;
    logic [3:0]  rd_dev_q;

    always_comb begin
        case (sys_dev)
            SYSDEV_CPU: begin
                if (sys_reg <= SYSREG_CPU_NAME3) now_rdata = cpuid_rdata;
                else                             now_rdata = perfctr_rdata;
            end
            SYSDEV_BUS:   now_rdata = busctl_rdata;
            SYSDEV_TIMER: now_rdata = timer_rdata;
            SYSDEV_MACH:  now_rdata = machid_rdata;
            default:      now_rdata = 32'b0;
        endcase
    end

    always_ff @(posedge i_clk)
        if (sys_re) begin
            now_rdata_q <= now_rdata;
            rd_dev_q    <= sys_dev;
        end

    always_comb begin
        case (rd_dev_q)
            SYSDEV_MMU:       sys_rdata = mmu_sys_rdata;
            SYSDEV_L1_DCACHE: sys_rdata = dl1_sys_rdata;
            SYSDEV_L1_ICACHE: sys_rdata = il1_sys_rdata;
            SYSDEV_L2_CACHE:  sys_rdata = l2_sys_rdata;
            default:          sys_rdata = now_rdata_q;
        endcase
    end

    /* verilator lint_on PINCONNECTEMPTY */

    // ── Assertions (sim-only; Verilator --assert) ─────────────────
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(sys_we && sys_re))
        else $error("machine_penumbra3: a sysreg write and read collide");

endmodule
