// machine_penumbra4 — the board-independent gen4 computer.
//
// The machine layer of the build (doc/internals/build-system.md) for
// Penumbra/4: the core, the MMU, the two L1s, the uncore and the sysreg
// devices, behind the gen3 machine's bus, interrupt and bus-control
// ports, so a board top or a sim wrapper attaches its devices to it as it
// does to gen3's (doc/internals/penumbra4/structure.md, "The machine").
//
// The observability ports are gen4's own: the ops that commit, retiring
// or trapping, the register writes they make, up to two a cycle, the SR,
// the count retired, and the program-end pulse as a BREAK's trap
// commits.
//
// The sysreg devices are the CPU's identity and counters, the board's
// identity and the bus controller. The caches have no arrays and read as
// absent. The MMU's and the timer's devices are not built yet: an access
// to one stops the simulation, and on a board takes the illegal-instruction
// trap. Nothing takes an interrupt.

module machine_penumbra4
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
#(
    parameter vaddr_t      RESET_PC        = 32'hFFFF_0000,

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
    input  logic                 i_clk,
    input  logic                 i_rst,

    // ── Interrupt request inputs ─────────────────────────────────
    input  logic                 i_irq,
    input  logic                 i_timer_irq,

    // ── External bus ─────────────────────────────────────────────
    output paddr_t               o_bus_addr,
    output word_t                o_bus_wdata,
    output logic [3:0]           o_bus_byte_en,
    output logic                 o_bus_re,
    output logic                 o_bus_we,
    input  word_t                i_bus_rdata,
    input  logic                 i_bus_ack,
    input  logic                 i_bus_fault,     // valid with i_bus_ack

    // ── Bus controller, toward the wrapper's autoconfig chain ────
    output logic                 o_bus_rst,       // BUSCTL.RST
    output logic                 o_bus_cfg_en,    // BUSCTL.CFG_EN

    // ── Observability ────────────────────────────────────────────
    // The ops that commit this cycle, each at its slot's index (slot_e):
    // one that retires, or one whose trap is taken. The older names the
    // first in program order when both commit. The SR is the one they
    // commit on, before their own writes to it.
    output logic       [1:0]     o_commit_valid,
    output vaddr_t     [1:0]     o_commit_pc,
    output op_class_e  [1:0]     o_commit_op_class,
    output logic       [1:0]     o_commit_trap,
    output vector_t    [1:0]     o_commit_vector,
    output slot_e                o_commit_older,
    output sr_t                  o_sr,
    // A commit's register writes never name one register twice, so they
    // apply in either order.
    output logic       [1:0]     o_commit_we,
    output phys_reg_t  [1:0]     o_commit_phys,
    output word_t      [1:0]     o_commit_data,
    output logic       [1:0]     o_retire_count,  // up to three: a fused op and its partner
    output logic                 o_prog_end
);

    // ── Between the core and its memory system ──────────────────
    logic        fetch_launch;
    vaddr_t      fetch_vaddr_q;
    logic        icache_hit;
    line_t       icache_line;
    logic        icache_miss_req;
    paddr_t      icache_miss_addr;
    logic        icache_miss_cacheable;
    logic        icache_vector_req;
    paddr_t      icache_vector_addr_q;
    logic        icache_busy_q;
    logic        icache_done_q;
    line_t       icache_access_line_q;
    word_t       icache_access_word_q;
    logic        icache_access_fault_q;
    paddr_t      fetch_paddr_q;
    logic        fetch_cacheable;
    fault_type_t fetch_fault;

    logic        data_launch;
    vaddr_t      data_vaddr_q;
    logic        dcache_hit;
    word_t       dcache_word;
    logic        dcache_req;
    uncore_req_t dcache_access;
    logic        dcache_done_q;
    word_t       dcache_access_word_q;
    logic        dcache_access_fault_q;
    paddr_t      data_paddr_q;
    logic        data_cacheable;
    fault_type_t data_fault;

    logic        uc_fetch_valid_q;
    uncore_req_t uc_fetch_req_q;
    logic        uc_fetch_done;
    logic        uc_data_valid_q;
    uncore_req_t uc_data_req_q;
    logic        uc_data_done;
    logic        uc_fault_q;
    line_t       uc_line_q;

    sysreg_req_t sysreg_req;
    sysreg_rsp_t sysreg_rsp_q;

    pair_t            committed_q;
    reg_write_t [1:0] reg_write_q;

    penumbra4_core #(
        .RESET_PC (RESET_PC)
    ) u_core (
        .i_clk                   (i_clk),
        .i_rst                   (i_rst),
        .o_fetch_launch          (fetch_launch),
        .o_fetch_vaddr           (fetch_vaddr_q),
        .i_icache_hit            (icache_hit),
        .i_icache_line           (icache_line),
        .o_icache_miss_req       (icache_miss_req),
        .o_icache_miss_addr      (icache_miss_addr),
        .o_icache_miss_cacheable (icache_miss_cacheable),
        .o_icache_vector_req     (icache_vector_req),
        .o_icache_vector_addr    (icache_vector_addr_q),
        .i_icache_busy           (icache_busy_q),
        .i_icache_done           (icache_done_q),
        .i_icache_access_line    (icache_access_line_q),
        .i_icache_access_word    (icache_access_word_q),
        .i_icache_access_fault   (icache_access_fault_q),
        .i_fetch_paddr           (fetch_paddr_q),
        .i_fetch_cacheable       (fetch_cacheable),
        .i_fetch_fault           (fetch_fault),
        .o_data_launch           (data_launch),
        .o_data_vaddr            (data_vaddr_q),
        .i_dcache_hit            (dcache_hit),
        .i_dcache_word           (dcache_word),
        .o_dcache_req            (dcache_req),
        .o_dcache_access         (dcache_access),
        .i_dcache_done           (dcache_done_q),
        .i_dcache_access_word    (dcache_access_word_q),
        .i_dcache_access_fault   (dcache_access_fault_q),
        .i_data_paddr            (data_paddr_q),
        .i_data_cacheable        (data_cacheable),
        .i_data_fault            (data_fault),
        .o_sysreg                (sysreg_req),
        .i_sysreg                (sysreg_rsp_q),
        .o_committed             (committed_q),
        .o_sr                    (o_sr),
        .o_reg_write             (reg_write_q),
        .o_retire_count          (o_retire_count),
        .o_prog_end              (o_prog_end)
    );

    penumbra4_mmu u_mmu (
        .i_clk             (i_clk),
        .i_fetch_launch    (fetch_launch),
        .i_fetch_vaddr     (fetch_vaddr_q),
        .o_fetch_paddr     (fetch_paddr_q),
        .o_fetch_cacheable (fetch_cacheable),
        .o_fetch_fault     (fetch_fault),
        .i_data_launch     (data_launch),
        .i_data_vaddr      (data_vaddr_q),
        .o_data_paddr      (data_paddr_q),
        .o_data_cacheable  (data_cacheable),
        .o_data_fault      (data_fault)
    );

    penumbra4_icache u_icache (
        .i_clk            (i_clk),
        .i_rst            (i_rst),
        .i_launch         (fetch_launch),
        .i_launch_vaddr   (fetch_vaddr_q),
        .o_hit            (icache_hit),
        .o_line           (icache_line),
        .i_miss_req       (icache_miss_req),
        .i_miss_addr      (icache_miss_addr),
        .i_miss_cacheable (icache_miss_cacheable),
        .i_vector_req     (icache_vector_req),
        .i_vector_addr    (icache_vector_addr_q),
        .o_busy           (icache_busy_q),
        .o_done           (icache_done_q),
        .o_access_line    (icache_access_line_q),
        .o_access_word    (icache_access_word_q),
        .o_access_fault   (icache_access_fault_q),
        .o_uc_valid       (uc_fetch_valid_q),
        .o_uc_req         (uc_fetch_req_q),
        .i_uc_done        (uc_fetch_done),
        .i_uc_fault       (uc_fault_q),
        .i_uc_line        (uc_line_q)
    );

    penumbra4_dcache u_dcache (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_launch       (data_launch),
        .i_launch_vaddr (data_vaddr_q),
        .o_hit          (dcache_hit),
        .o_word         (dcache_word),
        .i_req          (dcache_req),
        .i_access       (dcache_access),
        .o_done         (dcache_done_q),
        .o_access_word  (dcache_access_word_q),
        .o_access_fault (dcache_access_fault_q),
        .o_uc_valid     (uc_data_valid_q),
        .o_uc_req       (uc_data_req_q),
        .i_uc_done      (uc_data_done),
        .i_uc_fault     (uc_fault_q),
        .i_uc_line      (uc_line_q)
    );

    penumbra4_uncore u_uncore (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_fetch_valid (uc_fetch_valid_q),
        .i_fetch_req   (uc_fetch_req_q),
        .o_fetch_done  (uc_fetch_done),
        .i_data_valid  (uc_data_valid_q),
        .i_data_req    (uc_data_req_q),
        .o_data_done   (uc_data_done),
        .o_fault       (uc_fault_q),
        .o_line        (uc_line_q),
        .o_bus_addr    (o_bus_addr),
        .o_bus_wdata   (o_bus_wdata),
        .o_bus_byte_en (o_bus_byte_en),
        .o_bus_re      (o_bus_re),
        .o_bus_we      (o_bus_we),
        .i_bus_rdata   (i_bus_rdata),
        .i_bus_ack     (i_bus_ack),
        .i_bus_fault   (i_bus_fault)
    );

    slot_t [1:0] committed_slots;

    assign committed_slots[SLOT_X] = committed_q.x;
    assign committed_slots[SLOT_Y] = committed_q.y;
    assign o_commit_older          = committed_q.older;

    always_comb begin
        for (int s = 0; s < 2; s++) begin
            o_commit_valid[s]    = committed_slots[s].valid;
            o_commit_pc[s]       = committed_slots[s].op.pc;
            o_commit_op_class[s] = committed_slots[s].op.op_class;
            o_commit_trap[s]     = committed_slots[s].op.fault.valid;
            o_commit_vector[s]   = committed_slots[s].op.fault.vector;
        end
    end

    always_comb begin
        for (int p = 0; p < 2; p++) begin
            o_commit_we[p]    = reg_write_q[p].en;
            o_commit_phys[p]  = reg_write_q[p].phys;
            o_commit_data[p]  = reg_write_q[p].value;
        end
    end

    // ══════════════════════════════════════════════════════════
    // Sysreg devices
    // ══════════════════════════════════════════════════════════
    // Each answers in the request's cycle, and the answer is registered for
    // MEM2, which takes it a cycle later. Every unassigned device reads 0,
    // which is how the caches, with no arrays, read as absent.
    word_t cpuid_rdata, perfctr_rdata, machid_rdata, busctl_rdata;
    logic  busctl_we;

    assign busctl_we = sysreg_req.valid && sysreg_req.write && (sysreg_req.dev == SYSDEV_BUS);

    cpuid #(
        .CPU_NAME2 (32'h0000342F)    // "/4\0\0"
    ) u_cpuid (
        .i_sys_reg   (sysreg_req.sysreg),
        .o_sys_rdata (cpuid_rdata)
    );

    penumbra4_perfctr u_perfctr (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_retire_count (o_retire_count),
        .i_sysreg       (sysreg_req.sysreg),
        .o_rdata        (perfctr_rdata)
    );

    machid #(
        .MACH_FEAT_VALUE (MACH_FEAT_VALUE),
        .MACH_NAME0      (MACH_NAME0),
        .MACH_NAME1      (MACH_NAME1),
        .MACH_NAME2      (MACH_NAME2),
        .MACH_NAME3      (MACH_NAME3),
        .CPU_FREQ        (CPU_FREQ)
    ) u_machid (
        .i_sys_reg   (sysreg_req.sysreg),
        .o_sys_rdata (machid_rdata)
    );

    busctl u_busctl (
        .i_clk       (i_clk),
        .i_rst       (i_rst),
        .i_sys_reg   (sysreg_req.sysreg),
        .i_sys_wdata (sysreg_req.wdata),
        .i_sys_we    (busctl_we),
        .o_sys_rdata (busctl_rdata),
        .o_bus_rst   (o_bus_rst),
        .o_cfg_en    (o_bus_cfg_en)
    );

    function automatic logic unbuilt_device(sysdev_t dev);
        return dev inside {SYSDEV_MMU, SYSDEV_TIMER};
    endfunction

    sysreg_rsp_t sysreg_rsp_d;

    always_comb begin
        sysreg_rsp_d.unbuilt = unbuilt_device(sysreg_req.dev);
        case (sysreg_req.dev)
            SYSDEV_CPU: begin
                if (sysreg_req.sysreg <= SYSREG_CPU_NAME3) begin
                    sysreg_rsp_d.rdata = cpuid_rdata;
                end else begin
                    sysreg_rsp_d.rdata = perfctr_rdata;
                end
            end
            SYSDEV_BUS:  sysreg_rsp_d.rdata = busctl_rdata;
            SYSDEV_MACH: sysreg_rsp_d.rdata = machid_rdata;
            default:     sysreg_rsp_d.rdata = '0;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            sysreg_rsp_q <= '0;
        end else if (sysreg_req.valid) begin
            sysreg_rsp_q <= sysreg_rsp_d;
        end
    end

    logic unused_inputs;
    assign unused_inputs = ^{i_irq, i_timer_irq};

    // ── Contract ─────────────────────────────────────────────────
    always_ff @(posedge i_clk) begin
        if (!i_rst && sysreg_req.valid) begin
            assert (!unbuilt_device(sysreg_req.dev))
                else $fatal(1, "machine_penumbra4: a sysreg access to device %0d, which the machine does not build yet",
                            sysreg_req.dev);
        end
    end

endmodule
