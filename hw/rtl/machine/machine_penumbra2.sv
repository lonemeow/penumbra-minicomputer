// machine_penumbra2 — the Penumbra/2 machine: core + memory system + bus.
//
// The board-independent gen2 computer (the machine layer of
// doc/internals/build-system.md). It binds the bare pipelined core to its
// generation-bound memory system and exposes the external Penumbra Bus:
//
//   core fetch port ──> L1 I-cache ──┐
//                        (VIPT)      ├──> I/D txn arbiter ─> fill sequencer
//   core dmem port ───> L1 D-cache ──┘                            │
//                        (VIPT)                                   v
//   core MMU ports ───> mmu_bram (port A fetch / port B data)   L2 cache
//   core sysreg side ─> device complex (MMU, caches, L2, CPU id) │
//                                                                v
//                                                       external bus (o_bus_*)
//
// Memory devices and peripherals live outside, on the bus a wrapper attaches
// them to (machine_penumbra2_sim for Verilator, a board top for FPGA) — what
// simulates is what synthesizes. The bus controller (SYSDEV_BUS) is the one
// device that straddles that boundary: a sysreg device inside the machine,
// but its autoconfig outputs (o_bus_rst / o_bus_cfg_en) drive the daisy chain
// those external devices sit on. The L1s resolve against the MMU verdicts
// (VIPT: vaddr indexes, paddr tags), so non-identity translations are fully
// supported; vector-table reads are bypass-translated, which the MMU reports
// uncacheable, so they pass through the I-L1 uncached and always observe the
// write-through-updated table.
//
// The sysreg device complex serves RDSYS through the core's sideband (the
// selected combinational response is captured at the launch strobe; the
// MMU's registered TLB readback is muxed in at data-ready instead — it is
// not valid at the launch edge) and fans the WRSYS commit out by device.
// Each device's single register selector serves both paths; they can never
// collide because WRSYS commits into an empty pipe (asserted below).
//
// The program-end contract (build-system.md): o_prog_end pulses on a
// retiring BREAK — gen2 traps rather than halts, and in-order commit
// guarantees architectural state is final when the pulse fires.
//
// A bus fault (no device at the address, or a slave rejecting an access)
// returns inward as a completion companion: i_bus_fault rides the external
// busy-drop, L2/sequencer/arbiter/L1 each carry it alongside the completion
// they already forward (a faulting line beat aborts the fill), and it enters
// the core as a fetch- or data-side fault, vectoring to VEC_BUS_FAULT with
// the faulting vaddr in FAULT_ADDR. Deferred: a fault on L2's own line fill
// while L2 is enabled (asserted in l2_cache).

module machine_penumbra2
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
#(
    parameter logic [31:0] RESET_PC     = 32'hFFFF_0000,
    parameter int          ICACHE_BYTES = 4096,
    parameter int          DCACHE_BYTES = 4096,
    parameter int          LINE_BYTES   = 16,
    parameter int          NUM_WAYS     = 4,

    // Machine identity (SYSDEV_MACH) — board-supplied. The wrapper / board top
    // sets these; the defaults describe an unnamed machine at unknown clock.
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
    input  logic                  i_bus_busy,
    input  logic                  i_bus_fault,   // no-device / slave access fault, at the busy-drop

    // ── Bus autoconfig control (to the wrapper's autoconfig chain) ─
    output logic                  o_bus_rst,     // software-timed bus reset (BUSCTL.RST)
    output logic                  o_bus_cfg_en,  // config window + daisy-chain enable (BUSCTL.CFG_EN)

    // ── Commit / retire observability ────────────────────────────
    output logic [SB_IDX_W-1:0]   o_commit_idx,
    output logic [31:0]           o_commit_data,
    output logic                  o_commit_we,
    output logic                  o_retire_valid,
    output logic [OPC_W-1:0]      o_retire_op_class,
    output logic [31:0]           o_retire_pc,       // retiring instruction's PC (trace)
    output logic [31:0]           o_retire_sr,       // its architectural SR (trace)
    output logic                  o_fault_commit,    // a fault is taken this cycle (trace marker)
    output logic [3:0]            o_fault_vec,       // its vector number
    output logic                  o_eret_commit,     // an ERET is committing (trace marker)
    output logic                  o_dc_commit,       // a drain-commit (EI/DI/WRSYS/ERET) retires (trace)
    output logic [31:0]           o_dc_commit_pc,    // its PC
    output logic [OPC_W-1:0]      o_dc_commit_op_class, // its op_class
    output logic                  o_branch_taken,    // EX branch resolve (trace)
    output logic [31:0]           o_branch_target,   // its target
    output logic [31:0]           o_branch_pc,       // the branch's own PC
    output logic [31:0]           o_ex_pc,           // pipeline occupancy: EX slot (trace)
    output logic                  o_ex_valid,
    output logic [31:0]           o_mem_pc,          // MEM slot
    output logic                  o_mem_valid,

    // ── Program end (test-runner contract: a retiring BREAK) ─────
    output logic                  o_prog_end
);

    localparam int FILL_WORD_W = $clog2(LINE_BYTES / 4);

    // MMU hit indications are observability the L1s do not consume.
    /* verilator lint_off PINCONNECTEMPTY */

    // ── Core <-> memory-system wires ─────────────────────────────
    logic [31:0] fetch_addr;
    logic        fetch_en, fetch_re, fetch_busy, fetch_bypass, fetch_user;
    logic        fetch_fault;
    logic [31:0] fetch_rdata;
    logic [31:0] dmem_addr, dmem_wdata, dmem_rdata;
    logic [3:0]  dmem_byte_en;
    logic        dmem_re, dmem_we, dmem_en, dmem_busy, dmem_fault;

    // MMU verdicts (port A = fetch, port B = data)
    logic [31:0] mmu_a_paddr, mmu_b_paddr;
    logic        mmu_a_cacheable, mmu_b_cacheable;
    logic        mmu_a_fault, mmu_b_fault;
    logic [31:0] mmu_a_fstatus, mmu_b_fstatus;

    // MMU port-B query (from the core's MEM stage)
    logic [31:0] mmu_vaddr;
    logic [2:0]  mmu_access_type;
    logic        mmu_user, mmu_req;

    // MMU fault-register commit
    logic        mmu_fault_commit;
    logic [31:0] mmu_fault_vaddr, mmu_fault_cstatus;

    // Sysreg sideband + write port
    logic [3:0]  sys_dev, sys_reg, sys_wr_dev, sys_wr_reg;
    logic        sys_re, sys_we;
    logic [31:0] sys_wdata, sys_rdata_rsp;
    logic [3:0]  sys_reg_sel;   // shared WRSYS-write / RDSYS-read register index

    // Retire pulse for the perfctr — includes drain-commit ops (which retire
    // from EX, not WB); see the core's o_insn_retired.
    logic        insn_retired;

    // Core's timer-IRQ line: the internal programmable timer, OR the external
    // i_timer_irq input (kept for direct injection; the internal timer is the
    // normal source). Assigned with the timer device below.
    logic        core_timer_irq;

    // ══════════════════════════════════════════════════════════
    // The core
    // ══════════════════════════════════════════════════════════
    penumbra2_core #(.RESET_PC(RESET_PC)) u_core (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_irq(i_irq), .i_timer_irq(core_timer_irq),
        .o_fetch_addr(fetch_addr), .o_fetch_en(fetch_en),
        .o_fetch_re(fetch_re),
        .i_fetch_rdata(fetch_rdata), .i_fetch_busy(fetch_busy),
        .i_fetch_fault(fetch_fault),
        .o_fetch_bypass(fetch_bypass), .o_fetch_user(fetch_user),
        .i_fetch_mmu_fault(mmu_a_fault),
        .i_fetch_mmu_fault_status(mmu_a_fstatus),
        .o_dmem_addr(dmem_addr), .o_dmem_wdata(dmem_wdata),
        .o_dmem_byte_en(dmem_byte_en), .o_dmem_re(dmem_re),
        .o_dmem_we(dmem_we), .o_dmem_en(dmem_en),
        .i_dmem_rdata(dmem_rdata), .i_dmem_busy(dmem_busy),
        .i_dmem_fault(dmem_fault),
        .o_mmu_vaddr(mmu_vaddr), .o_mmu_access_type(mmu_access_type),
        .o_mmu_user(mmu_user), .o_mmu_req(mmu_req),
        .i_mmu_fault(mmu_b_fault), .i_mmu_fault_status(mmu_b_fstatus),
        .o_mmu_fault_commit(mmu_fault_commit),
        .o_mmu_fault_vaddr(mmu_fault_vaddr),
        .o_mmu_fault_status(mmu_fault_cstatus),
        .o_sys_dev(sys_dev), .o_sys_reg(sys_reg), .o_sys_re(sys_re),
        .i_sys_rdata(sys_rdata_rsp),
        .o_sys_wr_dev(sys_wr_dev), .o_sys_wr_reg(sys_wr_reg),
        .o_sys_wdata(sys_wdata), .o_sys_we(sys_we),
        .o_commit_idx(o_commit_idx), .o_commit_data(o_commit_data),
        .o_commit_we(o_commit_we),
        .o_retire_valid(o_retire_valid), .o_retire_op_class(o_retire_op_class),
        .o_insn_retired(insn_retired),
        .o_retire_pc(o_retire_pc), .o_retire_sr(o_retire_sr),
        .o_fault_commit(o_fault_commit), .o_fault_vec(o_fault_vec),
        .o_eret_commit(o_eret_commit),
        .o_dc_commit(o_dc_commit), .o_dc_commit_pc(o_dc_commit_pc),
        .o_dc_commit_op_class(o_dc_commit_op_class),
        .o_branch_taken(o_branch_taken), .o_branch_target(o_branch_target),
        .o_branch_pc(o_branch_pc),
        .o_ex_pc(o_ex_pc), .o_ex_valid(o_ex_valid),
        .o_mem_pc(o_mem_pc), .o_mem_valid(o_mem_valid)
    );

    // The program-end pulse: a retiring BREAK (see the core header — BREAK
    // traps, it does not halt; the retire port is where it is visible).
    assign o_prog_end = o_retire_valid && (o_retire_op_class == OPC_BREAK);

    // ══════════════════════════════════════════════════════════
    // MMU — both translate ports + architectural fault registers
    // ══════════════════════════════════════════════════════════
    // Port A follows the core's muxed fetch port: the launch strobe is the
    // query strobe (a held fetch keeps its held verdict), and the
    // vector-fetch FSM's ownership forces bypass — vector reads are physical
    // (boot-protocol contract) and thereby uncacheable.
    logic        mmu_sys_we, mmu_sys_re;
    logic [31:0] mmu_sys_rdata;

    mmu_bram u_mmu (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_a_vaddr(fetch_addr), .i_a_access_type(ACC_EXEC),
        .i_a_user_mode(fetch_user),
        .i_a_req(fetch_en), .i_a_force_bypass(fetch_bypass),
        .o_a_paddr(mmu_a_paddr), .o_a_cacheable(mmu_a_cacheable),
        .o_a_hit(),
        .o_a_fault(mmu_a_fault), .o_a_fault_status(mmu_a_fstatus),
        .i_b_vaddr(mmu_vaddr), .i_b_access_type(mmu_access_type),
        .i_b_user_mode(mmu_user), .i_b_req(mmu_req), .i_b_force_bypass(1'b0),
        .o_b_paddr(mmu_b_paddr), .o_b_cacheable(mmu_b_cacheable),
        .o_b_hit(),
        .o_b_fault(mmu_b_fault), .o_b_fault_status(mmu_b_fstatus),
        .i_fault_commit(mmu_fault_commit), .i_fault_vaddr(mmu_fault_vaddr),
        .i_fault_status(mmu_fault_cstatus),
        .i_sys_reg(sys_reg_sel), .i_sys_wdata(sys_wdata),
        .i_sys_we(mmu_sys_we), .i_sys_re(mmu_sys_re),
        .o_sys_rdata(mmu_sys_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Split L1 caches (VIPT) — front sides on the core's two ports
    // ══════════════════════════════════════════════════════════
    // L1 back sides toward the arbiter
    logic [31:0] l1i_mem_addr, l1i_mem_wdata, l1d_mem_addr, l1d_mem_wdata;
    logic [3:0]  l1i_mem_byte_en, l1d_mem_byte_en;
    logic        l1i_mem_re, l1i_mem_we, l1i_mem_cacheable;
    logic        l1d_mem_re, l1d_mem_we, l1d_mem_cacheable;
    logic [31:0] l1i_mem_rdata, l1d_mem_rdata;
    logic        l1i_mem_busy, l1d_mem_busy, l1i_mem_fault, l1d_mem_fault;
    logic        l1i_fill_we, l1d_fill_we, l1i_fill_done, l1d_fill_done;
    logic        l1i_fill_fault, l1d_fill_fault;
    logic [FILL_WORD_W-1:0] l1i_fill_word, l1d_fill_word;
    logic [31:0] l1i_fill_wdata, l1d_fill_wdata;

    logic        icache_sys_we, dcache_sys_we;
    logic [31:0] icache_sys_rdata, dcache_sys_rdata;

    // I-side: read-only front (fetches consume whole words; the write
    // inputs are tied off).
    cache_bram_vipt #(
        .CACHE_BYTES(ICACHE_BYTES), .LINE_BYTES(LINE_BYTES), .NUM_WAYS(NUM_WAYS)
    ) u_icache (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_vaddr(fetch_addr), .i_en(fetch_en),
        .i_paddr(mmu_a_paddr), .i_cacheable(mmu_a_cacheable),
        .i_fault(mmu_a_fault),
        .i_re(fetch_re), .i_we(1'b0),
        .i_wdata(32'b0), .i_byte_en(4'b1111),
        .o_rdata(fetch_rdata), .o_busy(fetch_busy), .o_fault(fetch_fault),
        .o_mem_addr(l1i_mem_addr), .o_mem_wdata(l1i_mem_wdata),
        .o_mem_byte_en(l1i_mem_byte_en),
        .o_mem_re(l1i_mem_re), .o_mem_we(l1i_mem_we),
        .o_mem_cacheable(l1i_mem_cacheable),
        .i_mem_rdata(l1i_mem_rdata), .i_mem_busy(l1i_mem_busy),
        .i_mem_fault(l1i_mem_fault),
        .i_fill_we(l1i_fill_we), .i_fill_word(l1i_fill_word),
        .i_fill_wdata(l1i_fill_wdata), .i_fill_done(l1i_fill_done),
        .i_fill_fault(l1i_fill_fault),
        .i_sys_reg(sys_reg_sel), .i_sys_wdata(sys_wdata),
        .i_sys_we(icache_sys_we), .o_sys_rdata(icache_sys_rdata)
    );

    // D-side: full read/write front on MEM's dmem port.
    cache_bram_vipt #(
        .CACHE_BYTES(DCACHE_BYTES), .LINE_BYTES(LINE_BYTES), .NUM_WAYS(NUM_WAYS)
    ) u_dcache (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_vaddr(dmem_addr), .i_en(dmem_en),
        .i_paddr(mmu_b_paddr), .i_cacheable(mmu_b_cacheable),
        .i_fault(mmu_b_fault),
        .i_re(dmem_re), .i_we(dmem_we),
        .i_wdata(dmem_wdata), .i_byte_en(dmem_byte_en),
        .o_rdata(dmem_rdata), .o_busy(dmem_busy), .o_fault(dmem_fault),
        .o_mem_addr(l1d_mem_addr), .o_mem_wdata(l1d_mem_wdata),
        .o_mem_byte_en(l1d_mem_byte_en),
        .o_mem_re(l1d_mem_re), .o_mem_we(l1d_mem_we),
        .o_mem_cacheable(l1d_mem_cacheable),
        .i_mem_rdata(l1d_mem_rdata), .i_mem_busy(l1d_mem_busy),
        .i_mem_fault(l1d_mem_fault),
        .i_fill_we(l1d_fill_we), .i_fill_word(l1d_fill_word),
        .i_fill_wdata(l1d_fill_wdata), .i_fill_done(l1d_fill_done),
        .i_fill_fault(l1d_fill_fault),
        .i_sys_reg(sys_reg_sel), .i_sys_wdata(sys_wdata),
        .i_sys_we(dcache_sys_we), .o_sys_rdata(dcache_sys_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // I/D arbiter -> fill sequencer -> L2 -> external bus
    // ══════════════════════════════════════════════════════════
    logic [31:0] arb_addr, arb_wdata, arb_rdata;
    logic [3:0]  arb_byte_en;
    logic        arb_re, arb_we, arb_cacheable, arb_busy, arb_fault;
    logic        seq_fill_we, seq_fill_done, seq_fill_fault;
    logic [FILL_WORD_W-1:0] seq_fill_word;
    logic [31:0] seq_fill_wdata;

    txn_arbiter #(.FILL_WORD_W(FILL_WORD_W)) u_arb (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_i_addr(l1i_mem_addr), .i_i_wdata(l1i_mem_wdata),
        .i_i_byte_en(l1i_mem_byte_en),
        .i_i_re(l1i_mem_re), .i_i_we(l1i_mem_we),
        .i_i_cacheable(l1i_mem_cacheable),
        .o_i_rdata(l1i_mem_rdata), .o_i_busy(l1i_mem_busy),
        .o_i_fault(l1i_mem_fault),
        .o_i_fill_we(l1i_fill_we), .o_i_fill_word(l1i_fill_word),
        .o_i_fill_wdata(l1i_fill_wdata), .o_i_fill_done(l1i_fill_done),
        .o_i_fill_fault(l1i_fill_fault),
        .i_d_addr(l1d_mem_addr), .i_d_wdata(l1d_mem_wdata),
        .i_d_byte_en(l1d_mem_byte_en),
        .i_d_re(l1d_mem_re), .i_d_we(l1d_mem_we),
        .i_d_cacheable(l1d_mem_cacheable),
        .o_d_rdata(l1d_mem_rdata), .o_d_busy(l1d_mem_busy),
        .o_d_fault(l1d_mem_fault),
        .o_d_fill_we(l1d_fill_we), .o_d_fill_word(l1d_fill_word),
        .o_d_fill_wdata(l1d_fill_wdata), .o_d_fill_done(l1d_fill_done),
        .o_d_fill_fault(l1d_fill_fault),
        .o_m_addr(arb_addr), .o_m_wdata(arb_wdata),
        .o_m_byte_en(arb_byte_en),
        .o_m_re(arb_re), .o_m_we(arb_we), .o_m_cacheable(arb_cacheable),
        .i_m_rdata(arb_rdata), .i_m_busy(arb_busy), .i_m_fault(arb_fault),
        .i_fill_we(seq_fill_we), .i_fill_word(seq_fill_word),
        .i_fill_wdata(seq_fill_wdata), .i_fill_done(seq_fill_done),
        .i_fill_fault(seq_fill_fault)
    );

    logic [31:0] l2_addr, l2_wdata, l2_rdata;
    logic [3:0]  l2_byte_en;
    logic        l2_re, l2_we, l2_cacheable, l2_busy, l2_fault;

    fill_sequencer #(.LINE_BYTES(LINE_BYTES)) u_fillseq (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(arb_addr), .i_wdata(arb_wdata), .i_byte_en(arb_byte_en),
        .i_re(arb_re), .i_we(arb_we), .i_cacheable(arb_cacheable),
        .o_rdata(arb_rdata), .o_busy(arb_busy), .o_fault(arb_fault),
        .o_fill_we(seq_fill_we), .o_fill_word(seq_fill_word),
        .o_fill_wdata(seq_fill_wdata), .o_fill_done(seq_fill_done),
        .o_fill_fault(seq_fill_fault),
        .o_l2_addr(l2_addr), .o_l2_wdata(l2_wdata), .o_l2_byte_en(l2_byte_en),
        .o_l2_re(l2_re), .o_l2_we(l2_we), .o_l2_cacheable(l2_cacheable),
        .i_l2_rdata(l2_rdata), .i_l2_busy(l2_busy), .i_l2_fault(l2_fault)
    );

    logic        l2_sys_we;
    logic [31:0] l2_sys_rdata;

    l2_cache u_l2 (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_addr(l2_addr), .i_wdata(l2_wdata), .i_byte_en(l2_byte_en),
        .i_we(l2_we), .i_re(l2_re), .i_cacheable(l2_cacheable),
        .o_rdata(l2_rdata), .o_busy(l2_busy), .o_fault(l2_fault),
        .o_mem_addr(o_bus_addr), .o_mem_wdata(o_bus_wdata),
        .o_mem_byte_en(o_bus_byte_en),
        .o_mem_we(o_bus_we), .o_mem_re(o_bus_re),
        .i_mem_rdata(i_bus_rdata), .i_mem_busy(i_bus_busy),
        .i_mem_fault(i_bus_fault),
        .i_sys_reg(sys_reg_sel), .i_sys_wdata(sys_wdata),
        .i_sys_we(l2_sys_we), .o_sys_rdata(l2_sys_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Sysreg device complex — RDSYS read / WRSYS write targets
    // ══════════════════════════════════════════════════════════
    // Write fanout: one strobe per device, decoded from the WRSYS commit.
    assign mmu_sys_we    = sys_we && (sys_wr_dev == SYSDEV_MMU);
    assign dcache_sys_we = sys_we && (sys_wr_dev == SYSDEV_L1_DCACHE);
    assign icache_sys_we = sys_we && (sys_wr_dev == SYSDEV_L1_ICACHE);
    assign l2_sys_we     = sys_we && (sys_wr_dev == SYSDEV_L2_CACHE);
    assign mmu_sys_re    = sys_re && (sys_dev == SYSDEV_MMU);

    // One register selector for every device: a WRSYS commit presents its
    // register on sys_wr_reg, an RDSYS read presents on sys_reg, and the two
    // never coincide (asserted below) — so a single mux serves all devices. A
    // device not selected this cycle sees a don't-care index: its own write
    // strobe (above) gates whether it latches, and the read mux downstream
    // picks whose response is returned.
    assign sys_reg_sel = sys_we ? sys_wr_reg : sys_reg;

    // CPU identity (read-only, combinational).
    logic [31:0] cpuid_rdata;
    cpuid #(
        .CPU_NAME2(32'h0000322F)   // "/2\0\0" — same ISA, second implementation
    ) u_cpuid (
        .i_sys_reg(sys_reg), .o_sys_rdata(cpuid_rdata)
    );

    // Machine identity (read-only, combinational) — board-supplied constants.
    logic [31:0] machid_rdata;
    machid #(
        .MACH_FEAT_VALUE(MACH_FEAT_VALUE),
        .MACH_NAME0(MACH_NAME0), .MACH_NAME1(MACH_NAME1),
        .MACH_NAME2(MACH_NAME2), .MACH_NAME3(MACH_NAME3),
        .CPU_FREQ(CPU_FREQ)
    ) u_machid (
        .i_sys_reg(sys_reg), .o_sys_rdata(machid_rdata)
    );

    // CPU performance counters (SYSDEV_CPU regs 5+): free-running cycle and
    // retired-instruction counts, reset to 0. insn_retired includes drain-commit
    // ops, so CPI stays >= 1. The gen1 stall counters (regs 7-10) are tied to a
    // microarchitecture-specific stall taxonomy and are not modeled here; those
    // registers read 0 (no events) until a gen2 stall breakdown is defined.
    logic [31:0] perfctr_cycles, perfctr_insns;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            perfctr_cycles <= 32'b0;
            perfctr_insns  <= 32'b0;
        end else begin
            perfctr_cycles <= perfctr_cycles + 32'd1;
            if (insn_retired) perfctr_insns <= perfctr_insns + 32'd1;
        end
    end

    logic [31:0] perfctr_rdata;
    always_comb begin
        case (sys_reg)
            SYSREG_CPU_CYCLES:        perfctr_rdata = perfctr_cycles;
            SYSREG_CPU_INSNS_RETIRED: perfctr_rdata = perfctr_insns;
            default:                  perfctr_rdata = 32'b0;
        endcase
    end

    // ── Programmable interval timer (SYSDEV_TIMER) ───────────────
    // Self-contained: a prescaler divides the CPU clock to a ~1 MHz reference
    // (toggled, since timer.sv edge-detects the tick), and the device raises
    // o_irq once software programs it and the counter underflows. CPU_FREQ is
    // board-supplied; the CPU_FREQ=0 default (the bare timing probe) keeps the
    // prescaler legal but leaves the timer unused — it is never programmed there.
    localparam int TICK_TARGET_HZ = 1_000_000;
    localparam int PRESCALE_RAW   = (CPU_FREQ + TICK_TARGET_HZ) / (2 * TICK_TARGET_HZ);
    localparam int PRESCALE_DIV   = (PRESCALE_RAW >= 2) ? PRESCALE_RAW : 2;
    localparam int TIMER_TICK_HZ  = CPU_FREQ / (2 * PRESCALE_DIV);

    logic [$clog2(PRESCALE_DIV)-1:0] prescale_cnt;
    logic timer_tick;
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            prescale_cnt <= '0;
            timer_tick   <= 1'b0;
        end else if (prescale_cnt == ($bits(prescale_cnt))'(PRESCALE_DIV - 1)) begin
            prescale_cnt <= '0;
            timer_tick   <= ~timer_tick;   // toggle: one edge per full period
        end else begin
            prescale_cnt <= prescale_cnt + 1'b1;
        end
    end

    logic        timer_sys_we;
    logic [31:0] timer_rdata;
    logic        timer_o_irq;
    assign timer_sys_we = sys_we && (sys_wr_dev == SYSDEV_TIMER);

    timer #(.TICK_FREQ_HZ(TIMER_TICK_HZ)) u_timer (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_tick(timer_tick),
        .i_sys_reg(sys_reg_sel), .i_sys_wdata(sys_wdata),
        .i_sys_we(timer_sys_we), .o_sys_rdata(timer_rdata),
        .o_irq(timer_o_irq)
    );

    assign core_timer_irq = timer_o_irq | i_timer_irq;

    // ── Bus controller (SYSDEV_BUS) — autoconfig + bus reset ─────
    // Two flops the ROM's autoconfig loop drives: o_cfg_en gates the config
    // address window and daisy chain, o_bus_rst is the software-timed bus
    // reset. Both leave the machine to the wrapper's autoconfig_dev chain.
    logic        busctl_sys_we;
    logic [31:0] busctl_rdata;
    assign busctl_sys_we = sys_we && (sys_wr_dev == SYSDEV_BUS);

    busctl u_busctl (
        .i_clk(i_clk), .i_rst(i_rst),
        .i_sys_reg(sys_reg_sel), .i_sys_wdata(sys_wdata),
        .i_sys_we(busctl_sys_we), .o_sys_rdata(busctl_rdata),
        .o_bus_rst(o_bus_rst), .o_cfg_en(o_bus_cfg_en)
    );

    // Writable scratch sysreg (4 words) — exercises the full WRSYS-commit →
    // device-write → RDSYS-read-back path with no side effects, which no
    // real device offers (their writes enable MMUs and flash caches).
    localparam logic [3:0] SYSDEV_SCRATCH = 4'd6;
    logic [31:0] scratch_q [0:3];
    always_ff @(posedge i_clk)
        if (sys_we && sys_wr_dev == SYSDEV_SCRATCH && sys_wr_reg[3:2] == 2'b00)
            scratch_q[sys_wr_reg[1:0]] <= sys_wdata;

    // Read response: the selected combinational response is captured at the
    // launch strobe (the registered-response contract RDSYS's data-ready
    // timing relies on — selectors are MEM-held, so the capture is stable).
    // The MMU's TLB readback is itself a registered BRAM read launched by
    // mmu_sys_re — not valid at the launch edge — so a registered select
    // flag routes the response mux to the MMU's held output instead.
    logic [31:0] sys_rdata_sel, sys_rdata_q;
    logic        mmu_sys_sel_q;

    always_comb begin
        case (sys_dev)
            SYSDEV_CPU:       sys_rdata_sel = (sys_reg <= SYSREG_CPU_NAME3)
                                              ? cpuid_rdata : perfctr_rdata;
            SYSDEV_L1_DCACHE: sys_rdata_sel = dcache_sys_rdata;
            SYSDEV_L1_ICACHE: sys_rdata_sel = icache_sys_rdata;
            SYSDEV_BUS:       sys_rdata_sel = busctl_rdata;
            SYSDEV_L2_CACHE:  sys_rdata_sel = l2_sys_rdata;
            SYSDEV_MACH:      sys_rdata_sel = machid_rdata;
            SYSDEV_TIMER:     sys_rdata_sel = timer_rdata;
            SYSDEV_SCRATCH:   sys_rdata_sel = scratch_q[sys_reg[1:0]];
            default:          sys_rdata_sel = 32'b0;
        endcase
    end

    always_ff @(posedge i_clk)
        if (sys_re) begin
            sys_rdata_q   <= sys_rdata_sel;
            mmu_sys_sel_q <= (sys_dev == SYSDEV_MMU);
        end

    assign sys_rdata_rsp = mmu_sys_sel_q ? mmu_sys_rdata : sys_rdata_q;

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════

    // The shared per-device register selectors rely on the WRSYS write (a
    // drain-commit into an empty pipe) and the RDSYS read (a MEM access)
    // being mutually exclusive in time.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(sys_we && sys_re))
        else $error("machine_penumbra2: sysreg write and read collide");

    /* verilator lint_on PINCONNECTEMPTY */
endmodule
