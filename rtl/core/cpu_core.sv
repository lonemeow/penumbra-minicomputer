// Penumbra CPU Core — complete processor with MMU and cache
//
// Wires together: datapath, micro-sequencer, microcode ROM, MMU,
// and cache. Includes inline fetch logic (phase 1: 1-cycle memory
// read, no I-cache, no prefetch).
//
// External interfaces:
//   - Memory bus (post-cache): address, data, byte enables, busy
//   - Sysreg bus (device 1+): device ID, register, data, cycle/we
//   - IRQ, debug/observation ports
//
// The MMU's sysreg interface (device 0) is handled internally.
// External sysreg devices (sysid, timer, UART, etc.) are wired
// by the machine-level integration module.

// verilator lint_off UNUSEDSIGNAL
// verilator lint_off UNSIGNED

module cpu_core
    import penumbra_pkg::*;
#(
    parameter logic [31:0] RESET_PC = 32'hFFFF_E000
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

    // ── Sysreg bus (external devices, dev_id >= 1) ──────────
    output logic [3:0]  o_sys_dev,
    output logic [3:0]  o_sys_reg,
    output logic [31:0] o_sys_wdata,
    output logic        o_sys_cycle,
    output logic        o_sys_we,
    input  logic [31:0] i_sys_rdata,

    // ── External interrupt ──────────────────────────────────
    input  logic        i_irq,

    // ── Debug / observation ports ────────────────────────────
    output logic [31:0] o_pc,
    output logic        o_halted,
    input  logic [3:0]  i_dbg_reg_addr,
    output logic [31:0] o_dbg_reg_data
);

    // ══════════════════════════════════════════════════════════
    // MMU → Cache → Memory chain
    // ══════════════════════════════════════════════════════════

    // ── MMU signals ────────────────────────────────────────
    logic [31:0] mmu_vaddr;
    logic [2:0]  mmu_access_type;
    logic        mmu_user_mode;
    logic        mmu_req;
    logic [31:0] mmu_paddr;
    logic        mmu_cacheable;
    logic        mmu_fault;
    logic        mmu_hit;
    logic [31:0] mmu_sys_rdata;

    // ── Cache signals ──────────────────────────────────────
    logic [31:0] cache_rdata;
    logic        cache_busy;

    // ── Data access enables (only during execute, not fetch) ──
    logic data_re, data_we;

    // ── Sysreg read mux ────────────────────────────────────
    // Device 0 (MMU) handled internally; device 1+ from external bus.
    logic [31:0] sys_rdata;
    always_comb begin
        if (dp_r_sys_dev == SYSDEV_MMU)
            sys_rdata = mmu_sys_rdata;
        else
            sys_rdata = i_sys_rdata;
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
    logic        fetch_go, ir_valid;
    logic [7:0]  dispatch_addr;

    // Datapath status
    logic        alu_busy, cond_result, sr_s, sr_i, ei_shadow;
    logic [31:0] pc;

    // Sequencer → datapath control signals
    logic [1:0]  ctl_a_src;
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
    logic        ctl_sys_cycle, ctl_sys_we, ctl_alu_start;
    logic        ctl_pc_load;
    logic        ctl_cross_bank;
    logic        ctl_ei_set, ctl_di_set, ctl_ei_shadow_clr;
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
        .o_sys_cycle     (ctl_sys_cycle),
        .o_sys_we        (ctl_sys_we),
        .o_alu_start     (ctl_alu_start),
        .o_pc_load       (ctl_pc_load),
        .o_illegal       (seq_illegal),
        .o_priv_violation(seq_priv_violation),
        .o_cross_bank    (ctl_cross_bank),
        .o_ei_set        (ctl_ei_set),
        .o_di_set        (ctl_di_set),
        .o_ei_shadow_clr (ctl_ei_shadow_clr)
    );

    // ══════════════════════════════════════════════════════════
    // Fetch logic (phase 1: inline, 1-cycle memory read)
    // ══════════════════════════════════════════════════════════

    logic fetch_active;
    assign fetch_active = !ctl_pc_load;  // In S_FETCH state

    logic fetch_prev;   // fetch_active delayed by 1 cycle
    logic ir_load;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            fetch_prev <= 1'b0;
            ir_valid   <= 1'b0;
        end else begin
            fetch_prev <= fetch_active;
            ir_valid   <= fetch_active & fetch_prev;
        end
    end

    assign ir_load = ir_valid && fetch_active;

    // ── Dispatch address computation ─────────────────────────
    logic [1:0]  fetch_format;
    assign fetch_format = mem_rdata[31:30];

    always_comb begin
        case (fetch_format)
            2'b00:   dispatch_addr = {1'b0, mem_rdata[29], 1'b0, mem_rdata[28:25], 1'b0};
            2'b01:   dispatch_addr = {1'b0, 2'b01, mem_rdata[29:27], 2'b00};
            2'b10:   dispatch_addr = {2'b10, mem_rdata[29:26], 2'b00};
            2'b11:   dispatch_addr = (mem_rdata[29:26] == 4'b1111) ? 8'h62 : 8'h60;
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

    always_comb begin
        data_fault   = mmu_fault && !fetch_active;
        fault_except = data_fault && !fault_pending;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            fault_pending <= 1'b0;
            fault_vector  <= 4'b0;
        end else if (fault_except) begin
            fault_pending <= 1'b1;
            fault_vector  <= mmu_hit ? VEC_TLB_PROT : VEC_TLB_MISS;
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

    // BREAK detection at dispatch (dispatch_addr == 0x4A for op=21)
    logic break_taken;
    assign break_taken = (dispatch_addr == 8'h4A);

    // SYSCALL detection at dispatch (dispatch_addr == 0x48 for op=20)
    logic syscall_taken;
    assign syscall_taken = (dispatch_addr == 8'h48);

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

    assign irq_taken    = i_irq & sr_i & !ei_shadow;

    // ── Dispatch-time exception: register the vector ─────────
    logic        dispatch_pending;
    logic [3:0]  dispatch_vector;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            dispatch_pending <= 1'b0;
            dispatch_vector  <= 4'b0;
        end else if (ir_valid && (break_taken || syscall_taken || irq_taken)) begin
            dispatch_pending <= 1'b1;
            dispatch_vector  <= break_taken   ? VEC_BREAK :
                                syscall_taken ? VEC_SYSCALL :
                                                VEC_IRQ;
        end else if (dispatch_pending && ctl_pc_load) begin
            dispatch_pending <= 1'b0;
        end
    end

    assign except_entry = fault_except | illegal_except | priv_except |
                          (break_taken & ir_valid) | (syscall_taken & ir_valid) |
                          (irq_taken & ir_valid);
    assign vector_num   = fault_pending    ? fault_vector :
                          illegal_pending  ? VEC_ILLEGAL :
                          priv_pending     ? VEC_PRIV :
                          dispatch_pending ? dispatch_vector :
                                             VEC_IRQ;

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
    assign mmu_req         = fetch_active || ctl_mem_read || ctl_mem_write;

    assign data_re = ctl_mem_read  && !fetch_active;
    assign data_we = ctl_mem_write && !fetch_active;

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
    // MMU — translates virtual→physical, signals faults
    // ══════════════════════════════════════════════════════════
    mmu u_mmu (
        .i_clk         (i_clk),
        .i_rst         (i_rst),
        .i_vaddr       (mmu_vaddr),
        .i_access_type (mmu_access_type),
        .i_user_mode   (mmu_user_mode),
        .i_req         (mmu_req),
        .i_force_bypass(vector_read && !fetch_active),
        .o_paddr       (mmu_paddr),
        .o_cacheable   (mmu_cacheable),
        .o_fault       (mmu_fault),
        .o_hit         (mmu_hit),
        // Sysreg interface (device 0 only — handled internally)
        .i_sys_reg     (dp_r_sys_reg),
        .i_sys_wdata   (dp_a_bus),
        .i_sys_we      (ctl_sys_cycle && ctl_sys_we && (dp_r_sys_dev == SYSDEV_MMU)),
        .o_sys_rdata   (mmu_sys_rdata)
    );

    // ══════════════════════════════════════════════════════════
    // Cache (stub — pass-through to backing memory)
    // ══════════════════════════════════════════════════════════
    cache_stub u_cache (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_paddr      (mmu_paddr),
        .i_wdata      (dp_mem_wdata),
        .i_byte_en    (byte_en),
        .i_we         (data_we),
        .i_re         (data_re),
        .i_cacheable  (mmu_cacheable),
        .o_rdata      (cache_rdata),
        .o_busy       (cache_busy),
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
    assign o_sys_we    = ctl_sys_we;

    // ══════════════════════════════════════════════════════════
    // Datapath
    // ══════════════════════════════════════════════════════════
    logic [31:0] dp_mem_wdata;
    logic [31:0] dp_a_bus;

    // Field extractor outputs
    // verilator lint_off UNUSEDSIGNAL
    logic [1:0]  dp_format;
    logic [4:0]  dp_r_op;
    logic [2:0]  dp_l_op;
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
        .i_cross_bank   (ctl_cross_bank),

        // Exception / interrupt entry
        .i_except_entry (except_entry),
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
        .o_dbg_reg_data (o_dbg_reg_data)
    );

    assign o_pc = pc;

endmodule

// verilator lint_on UNUSEDSIGNAL
// verilator lint_on UNSIGNED
