// machine_penumbra4 — the board-independent gen4 computer.
//
// The machine layer of the build (doc/internals/build-system.md) for
// Penumbra/4: the core, the MMU, the two L1s, the uncore and the sysreg
// devices, behind the gen3 machine's bus, interrupt and bus-control
// ports, so a board top or a sim wrapper attaches its devices to it as it
// does to gen3's (doc/internals/penumbra4/structure.md, "The machine").
//
// The observability ports are gen4's own: the register writes a commit
// makes, up to two a cycle, the instructions it retires, and the
// program-end pulse as a BREAK's trap commits.
//
// The machine has the fetch side alone: the L1-I, the MMU's fetch
// translation and the uncore's fetch side. Nothing drives the bus
// controller's outputs or takes an interrupt.

module machine_penumbra4
    import penumbra_pkg::*;
    import penumbra4_pkg::*;
#(
    parameter vaddr_t RESET_PC = 32'hFFFF_0000
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
    vaddr_t      fetch_vaddr;
    logic        icache_hit;
    line_t       icache_line;
    logic        icache_miss_req;
    paddr_t      icache_miss_addr;
    logic        icache_miss_cacheable;
    logic        icache_vector_req;
    paddr_t      icache_vector_addr;
    logic        icache_busy;
    logic        icache_done;
    line_t       icache_access_line;
    word_t       icache_access_word;
    logic        icache_access_fault;
    paddr_t      fetch_paddr;
    logic        fetch_cacheable;
    fault_type_t fetch_fault;

    logic        uc_fetch_valid;
    uncore_req_t uc_fetch_req;
    logic        uc_fetch_done;
    logic        uc_fault;
    line_t       uc_line;

    reg_write_t [1:0] reg_write;

    penumbra4_core #(
        .RESET_PC (RESET_PC)
    ) u_core (
        .i_clk                   (i_clk),
        .i_rst                   (i_rst),
        .o_fetch_launch          (fetch_launch),
        .o_fetch_vaddr           (fetch_vaddr),
        .i_icache_hit            (icache_hit),
        .i_icache_line           (icache_line),
        .o_icache_miss_req       (icache_miss_req),
        .o_icache_miss_addr      (icache_miss_addr),
        .o_icache_miss_cacheable (icache_miss_cacheable),
        .o_icache_vector_req     (icache_vector_req),
        .o_icache_vector_addr    (icache_vector_addr),
        .i_icache_busy           (icache_busy),
        .i_icache_done           (icache_done),
        .i_icache_access_line    (icache_access_line),
        .i_icache_access_word    (icache_access_word),
        .i_icache_access_fault   (icache_access_fault),
        .i_fetch_paddr           (fetch_paddr),
        .i_fetch_cacheable       (fetch_cacheable),
        .i_fetch_fault           (fetch_fault),
        .o_reg_write             (reg_write),
        .o_retire_count          (o_retire_count),
        .o_prog_end              (o_prog_end)
    );

    penumbra4_mmu u_mmu (
        .i_clk             (i_clk),
        .i_fetch_launch    (fetch_launch),
        .i_fetch_vaddr     (fetch_vaddr),
        .o_fetch_paddr     (fetch_paddr),
        .o_fetch_cacheable (fetch_cacheable),
        .o_fetch_fault     (fetch_fault)
    );

    penumbra4_icache u_icache (
        .i_clk            (i_clk),
        .i_rst            (i_rst),
        .i_launch         (fetch_launch),
        .i_launch_vaddr   (fetch_vaddr),
        .o_hit            (icache_hit),
        .o_line           (icache_line),
        .i_miss_req       (icache_miss_req),
        .i_miss_addr      (icache_miss_addr),
        .i_miss_cacheable (icache_miss_cacheable),
        .i_vector_req     (icache_vector_req),
        .i_vector_addr    (icache_vector_addr),
        .o_busy           (icache_busy),
        .o_done           (icache_done),
        .o_access_line    (icache_access_line),
        .o_access_word    (icache_access_word),
        .o_access_fault   (icache_access_fault),
        .o_uc_valid       (uc_fetch_valid),
        .o_uc_req         (uc_fetch_req),
        .i_uc_done        (uc_fetch_done),
        .i_uc_fault       (uc_fault),
        .i_uc_line        (uc_line)
    );

    penumbra4_uncore u_uncore (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_fetch_valid (uc_fetch_valid),
        .i_fetch_req   (uc_fetch_req),
        .o_fetch_done  (uc_fetch_done),
        .o_fault       (uc_fault),
        .o_line        (uc_line),
        .o_bus_addr    (o_bus_addr),
        .o_bus_wdata   (o_bus_wdata),
        .o_bus_byte_en (o_bus_byte_en),
        .o_bus_re      (o_bus_re),
        .o_bus_we      (o_bus_we),
        .i_bus_rdata   (i_bus_rdata),
        .i_bus_ack     (i_bus_ack),
        .i_bus_fault   (i_bus_fault)
    );

    always_comb begin
        for (int p = 0; p < 2; p++) begin
            o_commit_we[p]    = reg_write[p].en;
            o_commit_phys[p]  = reg_write[p].phys;
            o_commit_data[p]  = reg_write[p].value;
        end
    end

    assign o_bus_rst    = 1'b0;
    assign o_bus_cfg_en = 1'b0;

    /* verilator lint_off UNUSEDSIGNAL */
    logic unused_inputs;
    assign unused_inputs = ^{i_irq, i_timer_irq};
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
