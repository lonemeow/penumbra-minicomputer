// Penumbra CPU Top — minimal integration for simulation
//
// Wires together: datapath, micro-sequencer, microcode ROM, and a
// simple synchronous memory. Includes inline fetch logic (phase 1:
// 1-cycle memory read, no I-cache, no prefetch).
//
// Memory is unified (instruction and data share one port). Fetch and
// data access never overlap because fetch happens in S_FETCH state
// and data access happens in S_EXEC state.
//
// This module is sufficient to execute programs in simulation.
// A real SoC build would replace the simple memory with cache/bus
// and factor out the fetch unit into a proper state machine.

// verilator lint_off UNUSEDSIGNAL
// verilator lint_off UNSIGNED

module cpu_top
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── External interrupt ──────────────────────────────────
    input  logic        i_irq,          // Active-high interrupt request

    // ── Debug / observation ports ────────────────────────────
    output logic [31:0] o_pc,           // Current PC
    output logic        o_halted,       // No forward progress (future)
    input  logic [3:0]  i_dbg_reg_addr, // Debug: register address to read
    output logic [31:0] o_dbg_reg_data  // Debug: register value
);

    // ══════════════════════════════════════════════════════════
    // Simple synchronous memory (unified I/D, 4K words = 16KB)
    // ══════════════════════════════════════════════════════════
    localparam MEM_WORDS = 4096;

    logic [31:0] mem [0:MEM_WORDS-1];
    logic [31:0] mem_addr;
    logic [31:0] mem_rdata;
    logic [31:0] mem_wdata;
    logic        mem_we;

    initial begin
        for (int i = 0; i < MEM_WORDS; i++)
            mem[i] = 32'b0;
        $readmemh("program.hex", mem);
    end

    // Word-addressed (drop lower 2 bits)
    logic [11:0] mem_word_addr;
    assign mem_word_addr = mem_addr[13:2];

    always_ff @(posedge i_clk) begin
        if (mem_we)
            mem[mem_word_addr] <= mem_wdata;
    end

    // Synchronous read (1-cycle latency)
    logic [31:0] mem_rdata_reg;
    always_ff @(posedge i_clk) begin
        mem_rdata_reg <= mem[mem_word_addr];
    end
    assign mem_rdata = mem_rdata_reg;

    // ══════════════════════════════════════════════════════════
    // Microcode ROM
    // ══════════════════════════════════════════════════════════
    logic [7:0]  upc;
    logic [48:0] uword;

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
    logic        ctl_ei_set, ctl_di_set, ctl_ei_shadow_clr;

    sequencer u_sequencer (
        .i_clk           (i_clk),
        .i_rst           (i_rst),
        .i_ir_valid      (ir_valid),
        .i_dispatch_addr (effective_dispatch),
        .o_fetch_go      (fetch_go),
        .i_alu_busy      (alu_busy),
        .i_mem_busy      (1'b0),          // Simple memory never stalls
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
        .o_ei_set        (ctl_ei_set),
        .o_di_set        (ctl_di_set),
        .o_ei_shadow_clr (ctl_ei_shadow_clr)
    );

    // ══════════════════════════════════════════════════════════
    // Fetch logic (phase 1: inline, 1-cycle memory read)
    // ══════════════════════════════════════════════════════════
    //
    // When the sequencer is in S_FETCH, memory address = PC.
    // The synchronous memory delivers data one cycle later.
    // We use a simple state to track when data is ready.

    // Fetch uses the sequencer's executing signal (ctl_pc_load = 1 during
    // S_EXEC, 0 during S_FETCH). When NOT executing, memory address = PC.
    // After 1 cycle of memory read, data is valid → assert ir_valid.
    //
    // Timing per instruction (single micro-op):
    //   Cycle N:   execute, PC advances on clock edge, → S_FETCH
    //   Cycle N+1: S_FETCH, memory reads at new PC
    //   Cycle N+2: memory data valid, ir_valid fires
    //   Cycle N+3: S_EXEC, micro-word active, IR latched
    //   Cycle N+4: execute, results latch, → S_FETCH

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
            // Memory data is valid after 1 cycle in fetch state
            ir_valid   <= fetch_active & fetch_prev;
        end
    end

    assign ir_load = ir_valid;

    // ── Dispatch address computation ─────────────────────────
    // Computed from memory read data (same data that loads IR).
    // This is combinational — dispatch_addr is valid when ir_valid=1.
    logic [1:0]  fetch_format;
    assign fetch_format = mem_rdata[31:30];

    // Dispatch: 7-bit address, zero-extended to 8
    // Format R: {00, op[4:0]}           → 0x00–0x1F
    // Format L: {01, op[2:0], 00}       → 0x20–0x3C (×4 spacing)
    // Format M: {10, L, sz[1:0], SE, 0} → 0x40–0x5E (×2 spacing)
    // Format B: {11, 00000}             → 0x60
    always_comb begin
        case (fetch_format)
            2'b00:   dispatch_addr = {1'b0, 2'b00, mem_rdata[29:25]};
            2'b01:   dispatch_addr = {1'b0, 2'b01, mem_rdata[29:27], 2'b00};
            2'b10:   dispatch_addr = {1'b0, 2'b10, mem_rdata[29:26], 1'b0};
            2'b11:   dispatch_addr = 8'h60;
            default: dispatch_addr = 8'h00;
        endcase
    end

    // ══════════════════════════════════════════════════════════
    // Interrupt check at dispatch
    // ══════════════════════════════════════════════════════════
    //
    // When ir_valid fires, we have a fetched instruction and its
    // dispatch_addr ready. Before entering S_EXEC, check whether
    // an external interrupt should preempt the fetched instruction.
    //
    // Available signals for the check:
    //   i_irq       — external interrupt request (active high)
    //   sr_i        — SR.I bit (1 = interrupts enabled, 0 = disabled)
    //   ei_shadow   — 1 = EI just executed, suppress this check for one instruction
    //
    // When an interrupt is taken:
    //   irq_taken        — override dispatch_addr with 0x70 (int_entry)
    //   except_entry     — pulse to save shadow_PC/SR, set S=1, I=0
    //   irq_vector_num   — vector number for handler address

    logic        irq_taken;
    logic        except_entry;
    logic [3:0]  irq_vector_num;

    // Interrupt check: combinational — sr_i is registered so no race
    // with except_entry modifying it on the same clock edge.
    // ir_valid gates except_entry to a single-cycle pulse at dispatch.
    assign irq_taken      = i_irq & sr_i & !ei_shadow;
    assign except_entry   = irq_taken & ir_valid;
    assign irq_vector_num = 4'd1;  // IRQ = vector 1 -> handler at 0x04

    // Override dispatch address when interrupt taken
    logic [7:0] effective_dispatch;
    assign effective_dispatch = irq_taken ? 8'h70 : dispatch_addr;

    // ── Memory address mux ───────────────────────────────────
    // During fetch: address = PC (for instruction read)
    // During execute: address = MAR (for data read/write)
    logic [31:0] mar_addr;

    assign mem_addr  = fetch_active ? pc : mar_addr;
    assign mem_we    = ctl_mem_write && !fetch_active;
    // mem_wdata comes from MDR (via datapath o_mem_wdata)

    // ══════════════════════════════════════════════════════════
    // Datapath
    // ══════════════════════════════════════════════════════════
    logic [31:0] dp_mem_wdata;

    // Field extractor outputs (from datapath, for dispatch — not used
    // here since we compute dispatch from raw memory data)
    logic [1:0]  dp_format;
    logic [4:0]  dp_r_op;
    logic [2:0]  dp_l_op;
    logic        dp_m_load;
    logic [1:0]  dp_m_size;
    logic        dp_m_sign_ext;
    logic [3:0]  dp_b_cond;
    logic [3:0]  dp_r_sys_dev, dp_r_sys_reg;

    datapath u_datapath (
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
        .i_pc_src       (ctl_pc_src),
        .i_alu_start    (ctl_alu_start),
        .i_pc_load      (ctl_pc_load),

        // Exception / interrupt entry
        .i_except_entry (except_entry),
        .i_vector_num   (irq_vector_num),

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

        .i_dbg_reg_addr (i_dbg_reg_addr),
        .o_dbg_reg_data (o_dbg_reg_data)
    );

    assign mem_wdata = dp_mem_wdata;
    assign o_pc      = pc;
    assign o_halted  = 1'b0;  // Stub

endmodule

// verilator lint_on UNUSEDSIGNAL
// verilator lint_on UNSIGNED
