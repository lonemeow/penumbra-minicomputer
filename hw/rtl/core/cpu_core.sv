// Penumbra CPU Core — complete processor with MMU and split I/D cache
//
// Wires together: datapath, micro-sequencer, microcode ROM, MMU,
// and split I/D caches. Includes inline fetch logic that waits for
// the memory busy signal (latency-agnostic, works with any backing
// store).
//
// External interfaces:
//   - Memory bus (post-cache): address, data, byte enables, busy
//   - Sysreg bus (device 1+): device ID, register, data, cycle/we
//   - IRQ, debug/observation ports
//
// The MMU (device 0) and cache (devices 2–3) sysreg interfaces are
// handled internally. External sysreg devices (sysid, timer, etc.)
// are wired by the machine-level integration module.
//
// Bus arbiter: the I-cache and D-cache each have a memory-side port.
// Since fetch (S_FETCH) and data access (S_EXEC) are mutually
// exclusive, only one cache requests memory at a time. The internal
// `cpu_bus_arbiter` (D-cache priority) serializes their requests
// onto the single external memory port and gives each cache a
// private busy/rdata net so the two caches never combinationally
// cross-couple through the shared bus.

// verilator lint_off UNUSEDSIGNAL
// verilator lint_off UNSIGNED

module cpu_core
    import penumbra_pkg::*;
#(
    parameter logic [31:0] RESET_PC = 32'hFFFF_0000
)
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Memory bus (post-cache) ─────────────────────────────
    output logic [31:0] o_mem_addr,
    output logic [31:0] o_mem_wdata,
    output logic [3:0]  o_mem_byte_en,
    output logic        o_mem_we,
    output logic        o_mem_re,
    input  logic [31:0] i_mem_rdata,
    input  logic        i_mem_busy,
    input  logic        i_bus_fault,

    // ── Sysreg bus (external devices, dev_id >= 1) ──────────
    output logic [3:0]  o_sys_dev,
    output logic [3:0]  o_sys_reg,
    output logic [31:0] o_sys_wdata,
    output logic        o_sys_cycle,
    output logic        o_sys_we,
    input  logic [31:0] i_sys_rdata,

    // ── Interrupts ───────────────────────────────────────────
    input  logic        i_timer_irq,    // Timer interrupt (VEC_TIMER, higher priority)
    input  logic        i_irq,          // External device interrupt (VEC_EXT_IRQ)

    // ── Debug / observation ports ────────────────────────────
    output logic [31:0] o_pc,
    output logic        o_halted,
    input  logic [3:0]  i_dbg_reg_addr,
    output logic [31:0] o_dbg_reg_data,

    // ── Instruction trace ────────────────────────────────────
    output logic        o_trace_valid,
    output logic [31:0] o_trace_sr
);

    // ══════════════════════════════════════════════════════════
    // MMU → Cache → Memory chain
    // ══════════════════════════════════════════════════════════

    // ── MMU signals ────────────────────────────────────────
    logic [31:0] mmu_vaddr;
    logic [2:0]  mmu_access_type;
    logic        mmu_user_mode;
    logic        mmu_req;
    logic [1:0]  mmu_mem_size;
    logic [31:0] mmu_paddr;
    logic        mmu_cacheable;
    logic        mmu_fault;
    logic        mmu_hit;
    logic        mmu_align;
    logic [31:0] mmu_sys_rdata;

    // ── Cache signals ──────────────────────────────────────
    logic [31:0] icache_rdata, dcache_rdata;
    logic        icache_busy,  dcache_busy;
    logic [31:0] cache_rdata;
    logic        cache_busy;

    // Mux cache outputs based on active path
    assign cache_rdata = fetch_active ? icache_rdata : dcache_rdata;
    assign cache_busy  = fetch_active ? icache_busy  : dcache_busy;

    // ── Data access enables (only during execute, not fetch) ──
    logic data_re, data_we;

    // ── Cache sysreg signals ────────────────────────────────
    logic [31:0] dcache_sys_rdata, icache_sys_rdata;

    // ── CPU identity + performance counters (SYSDEV_CPU) ────
    // CPU identity (regs 0–4) and perfctrs (regs 5+) are both produced
    // inside cpu_core.  Outputs are merged by reg-range below.
    logic [31:0] cpuid_rdata, cpu_perfctr_rdata, cpu_sys_rdata;

    cpuid u_cpuid (
        .i_sys_reg  (dp_r_sys_reg),
        .o_sys_rdata(cpuid_rdata)
    );

    cpu_perfctr u_cpu_perfctr (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_insn_retired (ir_valid),
        .i_sys_reg      (dp_r_sys_reg),
        .o_sys_rdata    (cpu_perfctr_rdata)
    );

    // Identity occupies regs 0–4; counters live at 5+
    assign cpu_sys_rdata = (dp_r_sys_reg <= SYSREG_CPU_NAME3)
                         ? cpuid_rdata : cpu_perfctr_rdata;

    // ── Sysreg read mux ────────────────────────────────────
    // Devices handled internally: 0 (MMU), 1 (CPU), 2 (DCACHE), 3 (ICACHE);
    // remaining devices from external bus (BUS, TIMER, MACH, ...).
    logic [31:0] sys_rdata;
    always_comb begin
        case (dp_r_sys_dev)
            SYSDEV_MMU:    sys_rdata = mmu_sys_rdata;
            SYSDEV_CPU:    sys_rdata = cpu_sys_rdata;
            SYSDEV_DCACHE: sys_rdata = dcache_sys_rdata;
            SYSDEV_ICACHE: sys_rdata = icache_sys_rdata;
            default:       sys_rdata = i_sys_rdata;
        endcase
    end

    // ── Unified read data (used by both fetch and data path) ──
    // During sysreg read (sys_cycle && !sys_we), mux in sysreg data
    // instead of cache data so MDR can capture it via mdr_load_mem.
    logic [31:0] mem_rdata;
    assign mem_rdata = (ctl_sys_cycle && !ctl_sys_we) ? sys_rdata : cache_rdata;

    // ══════════════════════════════════════════════════════════
    // Microcode ROM
    // ══════════════════════════════════════════════════════════
    logic [7:0]  upc;
    logic [50:0] uword;

    ucode_rom u_ucode_rom (
        .i_addr  (upc),
        .o_uword (uword)
    );

    // ══════════════════════════════════════════════════════════
    // Sequencer
    // ══════════════════════════════════════════════════════════
    logic        fetch_go;
    logic        ir_valid;
    logic [7:0]  dispatch_addr;

    // Datapath status
    logic        alu_busy, cond_result, sr_s, sr_i, ei_shadow;
    logic [31:0] pc;

    // Sequencer → datapath control signals
    logic [2:0]  ctl_a_src;
    logic [3:0]  ctl_reg_a, ctl_reg_b, ctl_reg_w;
    logic        ctl_w_en;
    logic [4:0]  ctl_alu_op;
    logic [1:0]  ctl_bmux;
    logic        ctl_wmux;
    logic [1:0]  ctl_imm_mode;
    logic        ctl_flag_w_en, ctl_sr_load;
    logic        ctl_mar_load, ctl_mdr_load_mem, ctl_mdr_load_a;
    logic        ctl_mem_read, ctl_mem_write;
    logic [1:0]  ctl_mem_size;
    logic        ctl_sign_ext;
    logic [2:0]  ctl_pc_src;
    logic [1:0]  ctl_sys_op;
    logic        ctl_alu_start;
    logic        ctl_pc_load;
    logic        ctl_ei_set, ctl_di_set, ctl_ei_shadow_clr;

    // Decode sys_op enumeration
    logic        ctl_sys_cycle;  // Sysreg bus active (SYS_READ or SYS_WRITE)
    logic        ctl_sys_we;     // Sysreg bus write (SYS_WRITE only)
    logic        spr_write;      // SPR write (SPR_WRITE)
    assign ctl_sys_cycle = ctl_sys_op[1];              // bit 1 set for SYS_READ(2) and SYS_WRITE(3)
    assign ctl_sys_we    = (ctl_sys_op == 2'd3);       // SYS_WRITE only
    assign spr_write     = (ctl_sys_op == 2'd1);       // SPR_WRITE only
    logic        seq_illegal;
    logic        seq_priv_violation;

    sequencer u_sequencer (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_ir_valid      (ir_valid),
        .i_dispatch_addr (effective_dispatch),
        .o_fetch_go      (fetch_go),
        .i_alu_busy      (alu_busy),
        .i_mem_busy      (cache_busy),
        .i_mem_fault     (data_fault),
        .i_cond_result   (cond_result),
        .i_sr_s          (sr_s),
        .o_upc           (upc),
        .i_uword         (uword),
        .o_a_src         (ctl_a_src),
        .o_reg_a_sel     (ctl_reg_a),
        .o_reg_b_sel     (ctl_reg_b),
        .o_reg_w_sel     (ctl_reg_w),
        .o_reg_w_en      (ctl_w_en),
        .o_alu_op        (ctl_alu_op),
        .o_b_mux_sel     (ctl_bmux),
        .o_w_mux_sel     (ctl_wmux),
        .o_imm_mode      (ctl_imm_mode),
        .o_flag_w_en     (ctl_flag_w_en),
        .o_sr_load       (ctl_sr_load),
        .o_mar_load      (ctl_mar_load),
        .o_mdr_load_mem  (ctl_mdr_load_mem),
        .o_mdr_load_a    (ctl_mdr_load_a),
        .o_mem_read      (ctl_mem_read),
        .o_mem_write     (ctl_mem_write),
        .o_mem_size      (ctl_mem_size),
        .o_sign_ext      (ctl_sign_ext),
        .o_pc_src        (ctl_pc_src),
        .o_sys_op        (ctl_sys_op),
        .o_alu_start     (ctl_alu_start),
        .o_pc_load       (ctl_pc_load),
        .o_fetch_active  (fetch_active),
        .o_illegal       (seq_illegal),
        .o_priv_violation(seq_priv_violation),
        .o_ei_set        (ctl_ei_set),
        .o_di_set        (ctl_di_set),
        .o_ei_shadow_clr (ctl_ei_shadow_clr)
    );

    // ══════════════════════════════════════════════════════════
    // Fetch logic — busy-aware instruction fetch
    //
    // During S_FETCH, the address bus carries PC (via fetch_active)
    // and the I-cache serves the instruction read. The fetch waits
    // for busy to deassert before capturing the instruction into IR.
    // This makes fetch latency-agnostic, just like the STALL-based
    // data path. The I-cache has its own memory port, merged with
    // the D-cache port by the memory bus mux below.
    // ══════════════════════════════════════════════════════════

    // fetch_active comes directly from the sequencer's state register
    // (combinational `state == S_FETCH`).  Routing it through `!ctl_pc_load`
    // would couple the priv-violation check (which gates pc_load) into
    // every consumer of fetch_active — including mmu_vaddr, the cache
    // i_re inputs, and the icache/dcache rdata mux — so the priv cone
    // would land on the critical path before the TLB lookup.  Sourcing
    // from the state flop keeps the priv check parallel to the address
    // generation path; only the actual commit (pc_load, write enables)
    // needs to wait for it.
    logic fetch_active;

    logic ir_load;

    // This relies on cache_busy being driven combinationally so
    // it's immediately active if a bus cycle is needed.
    //
    // The !fault_except gate handles the cycle a fetch fault is first
    // detected: fault_pending hasn't latched yet, so effective_dispatch
    // and ir_load can't yet route to int_entry.  Suppressing for that
    // one cycle lets fault_pending register; the next cycle (with the
    // same fault still combinationally present) fetch_complete fires
    // normally, effective_dispatch picks 0x70, and ir_load stays gated
    // by !fault_pending so IR doesn't latch the garbage mem_rdata that
    // results from i_re being suppressed by mmu_fault upstream.
    logic fetch_complete;
    assign fetch_complete = fetch_active && !cache_busy && !fault_except;

    assign ir_valid = fetch_complete;
    assign ir_load  = fetch_complete && !fault_pending;

    // ── Fetch contract (SVA, simulation only) ────────────────
    // IR may only latch on a cycle where the cache has reported
    // the data is ready (cache_busy=0). The optimized fetch path
    // trusts cache_busy on the very first cycle of S_FETCH; if
    // anyone refactors and lets ir_load fire while cache_busy=1
    // (e.g., by registering cache_busy and creating a one-cycle
    // skew), this assertion catches it on the next test run.
    // Stripped by synthesis; no FPGA cost.
    assert property (@(posedge i_clk) disable iff (i_rst)
        ir_load |-> !cache_busy)
        else $error("cpu_core: ir_load fired while cache_busy=1");

    // ── Dispatch address computation ─────────────────────────
    // Source the dispatch decode directly from `icache_rdata`, NOT
    // from `mem_rdata`.  dispatch_addr only feeds the µPC update mux
    // when ir_valid=1 (state==S_FETCH), and during fetch the cache
    // mux always selects icache (mem_rdata == icache_rdata).  Reading
    // from mem_rdata makes synthesis include the dcache and sysreg
    // legs of the mux in the dispatch_addr cone — a phantom path that
    // can never produce a real dispatch but still has to settle each
    // cycle.  Going direct to icache_rdata cuts ~5 LUT-stages of
    // dcache/sysreg muxing out of the µPC update critical path.
    logic [31:0] fetch_rdata;
    assign fetch_rdata = icache_rdata;

    logic [1:0]  fetch_format;
    assign fetch_format = fetch_rdata[31:30];

    always_comb begin
        case (fetch_format)
            2'b00:   dispatch_addr = {1'b0, fetch_rdata[29], 1'b0, fetch_rdata[28:25], 1'b0};
            2'b01:   dispatch_addr = {1'b0, 2'b01, fetch_rdata[29:26], 1'b0};
            2'b10:   dispatch_addr = {2'b10, fetch_rdata[29:26], 2'b00};
            2'b11:   dispatch_addr = (fetch_rdata[29:26] == 4'b1111) ? 8'h62 : 8'h60;
            default: dispatch_addr = 8'h00;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Exception and interrupt handling
    // ══════════════════════════════════════════════════════════

    logic        irq_taken;
    logic        except_entry;
    logic [3:0]  vector_num;

    logic        data_fault;
    logic        fault_except;
    logic        fault_pending;
    logic [3:0]  fault_vector;

    logic fetch_fault;
    assign fetch_fault = (mmu_fault || i_bus_fault) && fetch_active;

    always_comb begin
        data_fault   = (mmu_fault || i_bus_fault) && !fetch_active;
        fault_except = (data_fault || fetch_fault) && !fault_pending;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            fault_pending <= 1'b0;
            fault_vector  <= 4'b0;
        end else if (fault_except) begin
            fault_pending <= 1'b1;
            fault_vector  <= i_bus_fault ? VEC_BUS_FAULT :
                             mmu_align   ? VEC_ALIGN :
                             (mmu_hit    ? VEC_TLB_PROT : VEC_TLB_MISS);
        end else if (fault_pending && ctl_pc_load) begin
            fault_pending <= 1'b0;
        end
    end

    // ── Illegal instruction detection (from sequencer) ───────
    logic        illegal_except;
    logic        illegal_pending;

    assign illegal_except = seq_illegal && !illegal_pending;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            illegal_pending <= 1'b0;
        else if (illegal_except)
            illegal_pending <= 1'b1;
        else if (illegal_pending && ctl_pc_load)
            illegal_pending <= 1'b0;
    end

    // BREAK detection at dispatch (dispatch_addr == 0x54 for op=26)
    // Gated by !fault_pending: during a fetch fault, dispatch_addr is
    // derived from stale/garbage mem_rdata — must not trigger BREAK.
    logic break_taken;
    assign break_taken = (dispatch_addr == 8'h54) && !fault_pending;

    // SYSCALL detection at dispatch (dispatch_addr == 0x52 for op=25)
    logic syscall_taken;
    assign syscall_taken = (dispatch_addr == 8'h52) && !fault_pending;

    // ── Privilege violation detection (from sequencer) ─────────
    logic        priv_except;
    logic        priv_pending;

    assign priv_except = seq_priv_violation && !priv_pending;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            priv_pending <= 1'b0;
        else if (priv_except)
            priv_pending <= 1'b1;
        else if (priv_pending && ctl_pc_load)
            priv_pending <= 1'b0;
    end

    // Timer IRQ has priority over external IRQ.
    // Both are gated by SR.I (global interrupt enable) and ei_shadow.
    logic        timer_irq_taken;
    logic        ext_irq_taken;
    assign timer_irq_taken = i_timer_irq & sr_i & !ei_shadow;
    assign ext_irq_taken   = i_irq & sr_i & !ei_shadow & !i_timer_irq;
    assign irq_taken       = timer_irq_taken | ext_irq_taken;

    // ── Dispatch-time exception: register the vector ─────────
    logic        dispatch_pending;
    logic [3:0]  dispatch_vector;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            dispatch_pending <= 1'b0;
            dispatch_vector  <= 4'b0;
        end else if (ir_valid && (break_taken || syscall_taken || irq_taken)) begin
            dispatch_pending <= 1'b1;
            dispatch_vector  <= break_taken     ? VEC_BREAK :
                                syscall_taken   ? VEC_SYSCALL :
                                timer_irq_taken ? VEC_TIMER :
                                                  VEC_EXT_IRQ;
        end else if (dispatch_pending && ctl_pc_load) begin
            dispatch_pending <= 1'b0;
        end
    end

    assign except_entry = fault_except | illegal_except | priv_except |
                          (break_taken & ir_valid) | (syscall_taken & ir_valid) |
                          (irq_taken & ir_valid);

    // ── Registered except_entry: slip ESR/EPC writes by one cycle ──
    // The combinational `except_entry` aggregates exception conditions
    // that all depend on the freshly-fetched instruction's bytes
    // (`mem_rdata` → field-extractor → dispatch_addr → break_taken /
    // syscall_taken / etc.).  Routing that long combinational cone
    // into status_reg's ESR-load gate makes ESR.LSR setup the
    // critical path tail (~6 ns on top of fetch-cycle logic).
    //
    // Registering the signal moves ESR/EPC writes — and the SR mode
    // switch (S=1, I=0) — to the cycle AFTER the dispatch cycle.
    // This is safe in this microarch because:
    //   - Fetch is hardwired in the sequencer, so no µROM micro-op
    //     runs during the dispatch cycle and no µ-op-driven SR/PC
    //     update happens at that clock edge.
    //   - The dispatched µ-op (cycle N+1) is `int_entry` op 0x70
    //     which has pc=HOLD and no SR-modifying control bits.
    //   - PC and SR are therefore stable across the slip cycle, so
    //     the delayed ESR/EPC capture still records pre-exception
    //     values.
    //   - vector_num/dispatch_vector latching, the µPC override
    //     (effective_dispatch), and vector_read all stay on the
    //     combinational `except_entry` — only the ESR/EPC path is
    //     deferred.
    //
    // Cost: +1 cycle on every exception entry.  That's 1 cycle on
    // BREAK/SYSCALL/IRQ/fault dispatch, paid once per exception —
    // negligible against the fmax win.
    logic except_entry_q;
    always_ff @(posedge i_clk) begin
        if (i_rst)
            except_entry_q <= 1'b0;
        else
            except_entry_q <= except_entry;
    end
    assign vector_num   = fault_pending    ? fault_vector :
                          illegal_pending  ? VEC_ILLEGAL :
                          priv_pending     ? VEC_PRIV :
                          dispatch_pending ? dispatch_vector :
                                             VEC_EXT_IRQ;

    // Override dispatch address when any exception taken
    logic [7:0] effective_dispatch;
    assign effective_dispatch = (fault_pending | illegal_pending | priv_pending | dispatch_pending | break_taken | syscall_taken | irq_taken) ? 8'h70 : dispatch_addr;

    // Debug observation: pulses when BREAK dispatches (testbench stop trigger)
    assign o_halted = break_taken & ir_valid;

    // ── Memory address and access mux ──────────────────────
    logic [31:0] mar_addr;

    assign mmu_vaddr       = fetch_active ? pc : mar_addr;
    assign mmu_access_type = fetch_active   ? ACC_EXEC  :
                             ctl_mem_write  ? ACC_WRITE  : ACC_READ;
    assign mmu_user_mode   = !sr_s;
    assign mmu_mem_size    = fetch_active ? 2'b10 : ctl_mem_size;  // fetch is always word
    assign mmu_req         = fetch_active || ctl_mem_read || ctl_mem_write;

    // Cache enables are NOT gated with !mmu_fault.  With VIPT, the
    // cache RAM lookup (data array, valid bits, tag read) is indexed
    // by virtual address and so can run in parallel with TLB
    // translation — gating cache.i_re with mmu_fault would serialize
    // the cache after the TLB and discard the entire VIPT benefit.
    //
    // Cache hit reads have no side effects, so they're safe to run
    // unconditionally — the CPU takes the exception via fault_except
    // → except_entry and naturally ignores the returned data.
    //
    // Cache STATE side effects (fill entry, write-hit data update)
    // are inhibited inside the cache via the registered i_fault
    // shadow flop — see cache_vipt.sv.
    //
    // BUS side effects (pass-through writes, uncached MMIO) are
    // inhibited at the bus arbiter's inputs — i_d_re / i_d_we /
    // i_i_re are gated with !mmu_fault below, so a faulting access
    // never gets latched into a real bus cycle.
    assign data_re = ctl_mem_read  && !fetch_active;
    assign data_we = ctl_mem_write && !fetch_active;

    // Note: with split I/D caches, each cache gets its own read enable:
    //   I-cache: i_re = fetch_active
    //   D-cache: i_re = data_re
    // The bus arbiter (instantiated below) owns the single external
    // bus port and serializes the two caches' requests onto it.

    // ── Byte enable generation ───────────────────────────────
    logic [3:0] byte_en;
    always_comb begin
        case (ctl_mem_size)
            2'b10:   byte_en = 4'b1111;                          // WORD
            2'b01:   byte_en = mmu_paddr[1] ? 4'b1100 : 4'b0011; // HALF
            2'b00:   begin                                         // BYTE
                case (mmu_paddr[1:0])
                    2'b00: byte_en = 4'b0001;
                    2'b01: byte_en = 4'b0010;
                    2'b10: byte_en = 4'b0100;
                    2'b11: byte_en = 4'b1000;
                endcase
            end
            default: byte_en = 4'b1111;
        endcase
    end

    // ── Vector table read bypass ────────────────────────────
    // int_entry (0x70-0x72) reads a handler address from the
    // vector table at physical 0x00. The data read at micro-op
    // 0x71 must bypass the MMU (vector table is physical).
    // Set when except_entry fires (one cycle before int_entry
    // starts), cleared when int_entry completes (fetch_go).
    logic vector_read;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            vector_read <= 1'b0;
        else if (except_entry)
            vector_read <= 1'b1;
        else if (fetch_go)
            vector_read <= 1'b0;
    end

    // ══════════════════════════════════════════════════════════
    // MMU — translates virtual→physical, checks alignment, signals faults
    // ══════════════════════════════════════════════════════════
    mmu u_mmu (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_vaddr       (mmu_vaddr),
        .i_access_type (mmu_access_type),
        .i_user_mode   (mmu_user_mode),
        .i_req         (mmu_req),
        .i_force_bypass(vector_read && !fetch_active),
        .i_mem_size    (mmu_mem_size),
        .i_bus_fault   (i_bus_fault),
        .o_paddr       (mmu_paddr),
        .o_cacheable   (mmu_cacheable),
        .o_fault       (mmu_fault),
        .o_hit         (mmu_hit),
        .o_align       (mmu_align),
        // Sysreg interface (device 0 only — handled internally)
        .i_sys_reg     (dp_r_sys_reg),
        .i_sys_wdata   (dp_a_bus),
        .i_sys_we      (ctl_sys_cycle && ctl_sys_we && (dp_r_sys_dev == SYSDEV_MMU)),
        .o_sys_rdata   (mmu_sys_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Split I/D Caches
    //
    // I-cache: serves instruction fetch (read-only)
    // D-cache: serves data loads/stores (read/write)
    //
    // Each has its own memory-side port, fed into the bus arbiter
    // below.  Both share the MMU's physical address and cacheable
    // output — safe because fetch and data are mutually exclusive.
    // The arbiter gives each cache a private busy/rdata pair so the
    // two caches' bus state never combinationally cross-couples.
    // ══════════════════════════════════════════════════════════

    // ── I-cache memory-side signals (to arbiter) ───────────
    logic [31:0] icache_mem_addr, icache_mem_wdata;
    logic [3:0]  icache_mem_byte_en;
    logic        icache_mem_we, icache_mem_re;
    // Each cache sees a private busy/rdata pair from the arbiter.
    logic [31:0] icache_arb_rdata, dcache_arb_rdata;
    logic        icache_arb_busy,  dcache_arb_busy;

    cache_vipt u_icache (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_vaddr      (mmu_vaddr),
        .i_paddr      (mmu_paddr),
        .i_wdata      (32'b0),
        .i_byte_en    (4'b0),
        .i_we         (1'b0),
        .i_re         (fetch_active),
        .i_cacheable  (mmu_cacheable),
        .i_fault      (mmu_fault),
        .o_rdata      (icache_rdata),
        .o_busy       (icache_busy),
        .o_mem_addr   (icache_mem_addr),
        .o_mem_wdata  (icache_mem_wdata),
        .o_mem_byte_en(icache_mem_byte_en),
        .o_mem_we     (icache_mem_we),
        .o_mem_re     (icache_mem_re),
        .i_mem_rdata  (icache_arb_rdata),
        .i_mem_busy   (icache_arb_busy),
        // Sysreg (device 3 = ICACHE)
        .i_sys_reg    (dp_r_sys_reg),
        .i_sys_wdata  (dp_a_bus),
        .i_sys_we     (ctl_sys_cycle && ctl_sys_we && (dp_r_sys_dev == SYSDEV_ICACHE)),
        .o_sys_rdata  (icache_sys_rdata)
    );

    // ── D-cache memory-side signals (to arbiter) ───────────
    logic [31:0] dcache_mem_addr, dcache_mem_wdata;
    logic [3:0]  dcache_mem_byte_en;
    logic        dcache_mem_we, dcache_mem_re;

    cache_vipt u_dcache (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_vaddr      (mmu_vaddr),
        .i_paddr      (mmu_paddr),
        .i_wdata      (dp_mem_wdata),
        .i_byte_en    (byte_en),
        .i_we         (data_we),
        .i_re         (data_re),
        .i_cacheable  (mmu_cacheable),
        .i_fault      (mmu_fault),
        .o_rdata      (dcache_rdata),
        .o_busy       (dcache_busy),
        .o_mem_addr   (dcache_mem_addr),
        .o_mem_wdata  (dcache_mem_wdata),
        .o_mem_byte_en(dcache_mem_byte_en),
        .o_mem_we     (dcache_mem_we),
        .o_mem_re     (dcache_mem_re),
        .i_mem_rdata  (dcache_arb_rdata),
        .i_mem_busy   (dcache_arb_busy),
        // Sysreg (device 2 = DCACHE)
        .i_sys_reg    (dp_r_sys_reg),
        .i_sys_wdata  (dp_a_bus),
        .i_sys_we     (ctl_sys_cycle && ctl_sys_we && (dp_r_sys_dev == SYSDEV_DCACHE)),
        .o_sys_rdata  (dcache_sys_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Internal bus arbiter — single external bus, two CPU clients
    //
    // Replaces the old combinational mux + busy-OR pattern.  The
    // arbiter registers requests on IDLE→BUSY and responses on
    // BUSY→DONE, structurally breaking the icache↔dcache cross-
    // coupling that previously pinned the CPU's critical path.
    //
    // mmu_fault is gated at the arbiter inputs so a faulting access
    // never starts an external bus cycle.  The gate is a single AND
    // on each port, no worse than the previous bus-output gate.
    // ══════════════════════════════════════════════════════════
    cpu_bus_arbiter u_bus_arbiter (
        .i_clk        (i_clk),
        .i_rst        (i_rst),

        // dcache port (priority)
        .i_d_addr     (dcache_mem_addr),
        .i_d_wdata    (dcache_mem_wdata),
        .i_d_byte_en  (dcache_mem_byte_en),
        .i_d_we       (dcache_mem_we && !mmu_fault),
        .i_d_re       (dcache_mem_re && !mmu_fault),
        .o_d_rdata    (dcache_arb_rdata),
        .o_d_busy     (dcache_arb_busy),

        // icache port (fetch-only)
        .i_i_addr     (icache_mem_addr),
        .i_i_re       (icache_mem_re && !mmu_fault),
        .o_i_rdata    (icache_arb_rdata),
        .o_i_busy     (icache_arb_busy),

        // External bus
        .o_mem_addr   (o_mem_addr),
        .o_mem_wdata  (o_mem_wdata),
        .o_mem_byte_en(o_mem_byte_en),
        .o_mem_we     (o_mem_we),
        .o_mem_re     (o_mem_re),
        .i_mem_rdata  (i_mem_rdata),
        .i_mem_busy   (i_mem_busy)
    );

    // ══════════════════════════════════════════════════════════
    // Sysreg bus — expose to external devices
    // ══════════════════════════════════════════════════════════
    assign o_sys_dev   = dp_r_sys_dev;
    assign o_sys_reg   = dp_r_sys_reg;
    assign o_sys_wdata = dp_a_bus;
    assign o_sys_cycle = ctl_sys_cycle;
    assign o_sys_we    = ctl_sys_we;  // Only asserted for SYS_WRITE (sys_op==3), never for SPR_WRITE

    // ══════════════════════════════════════════════════════════
    // Datapath
    // ══════════════════════════════════════════════════════════
    logic [31:0] dp_mem_wdata;
    logic [31:0] dp_a_bus;

    // Field extractor outputs
    // verilator lint_off UNUSEDSIGNAL
    logic [1:0]  dp_format;
    logic [4:0]  dp_r_op;
    logic [3:0]  dp_l_op;
    logic        dp_m_load;
    logic [1:0]  dp_m_size;
    logic        dp_m_sign_ext;
    logic [3:0]  dp_b_cond;
    // verilator lint_on UNUSEDSIGNAL
    logic [3:0]  dp_r_sys_dev, dp_r_sys_reg;

    datapath #(.RESET_PC(RESET_PC)) u_datapath (
        .i_clk          (i_clk),
        .i_rst          (i_rst),

        // Control signals from sequencer
        .i_a_src        (ctl_a_src),
        .i_reg_a_sel    (ctl_reg_a),
        .i_reg_b_sel    (ctl_reg_b),
        .i_reg_w_sel    (ctl_reg_w),
        .i_reg_w_en     (ctl_w_en),
        .i_alu_op       (ctl_alu_op),
        .i_b_mux_sel    (ctl_bmux),
        .i_w_mux_sel    (ctl_wmux),
        .i_imm_mode     (ctl_imm_mode),
        .i_flag_w_en    (ctl_flag_w_en),
        .i_sr_load      (ctl_sr_load),
        .i_mar_load     (ctl_mar_load),
        .i_mdr_load_mem (ctl_mdr_load_mem),
        .i_mdr_load_a   (ctl_mdr_load_a),
        .i_mem_size     (ctl_mem_size),
        .i_sign_ext     (ctl_sign_ext),
        .i_pc_src       (ctl_pc_src),
        .i_alu_start    (ctl_alu_start),
        .i_pc_load      (ctl_pc_load),
        .i_spr_write    (spr_write),

        // Exception / interrupt entry
        .i_except_entry (except_entry_q),
        .i_vector_num   (vector_num),

        // EI/DI
        .i_ei_set       (ctl_ei_set),
        .i_di_set       (ctl_di_set),
        .i_ei_shadow_clr(ctl_ei_shadow_clr),

        // IR load from fetch
        .i_ir_load      (ir_load),
        .i_mem_rdata    (mem_rdata),

        // Memory interface
        .o_mem_addr     (mar_addr),
        .o_mem_wdata    (dp_mem_wdata),

        // Status outputs
        .o_alu_busy     (alu_busy),
        .o_sr_s         (sr_s),
        .o_sr_i         (sr_i),
        .o_ei_shadow    (ei_shadow),
        .o_cond_result  (cond_result),

        // PC
        .o_pc           (pc),

        // Field extractor outputs
        .o_format       (dp_format),
        .o_r_op         (dp_r_op),
        .o_l_op         (dp_l_op),
        .o_m_load       (dp_m_load),
        .o_m_size       (dp_m_size),
        .o_m_sign_ext   (dp_m_sign_ext),
        .o_b_cond       (dp_b_cond),
        .o_r_sys_dev    (dp_r_sys_dev),
        .o_r_sys_reg    (dp_r_sys_reg),

        // A-bus output (for sysreg write data)
        .o_a_bus        (dp_a_bus),

        .i_dbg_reg_addr (i_dbg_reg_addr),
        .o_dbg_reg_data (o_dbg_reg_data),

        .o_trace_sr     (o_trace_sr)
    );

    // Trace: instruction-valid pulse
    assign o_trace_valid = ir_valid;

    assign o_pc = pc;

endmodule

// verilator lint_on UNUSEDSIGNAL
// verilator lint_on UNSIGNED
