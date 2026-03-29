// Penumbra Datapath Top Module — structural wiring of all leaf modules
//
// This module instantiates and connects: register file, ALU, field extractor,
// immediate extractor, A-mux, B-mux, W-mux, PC unit, PC mux, status register,
// condition evaluator, MAR, MDR, and the IR register.
//
// The datapath has NO control logic — it is entirely driven by micro-word
// fields from the micro-sequencer and signals from the fetch unit.
// The only "logic" here is:
//   - IR register (gated latch, controlled by fetch unit)
//   - Register address routing (mux between IR fields and micro-word literals)
//   - F-bit write-enable gating (AND gate)
//   - Vector address computation (shift-by-2, just wiring)

module datapath
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ══════════════════════════════════════════════════════════════
    // Micro-word control signals (from micro-sequencer / ROM)
    // ══════════════════════════════════════════════════════════════
    input  logic [1:0]  i_a_src,        // A-bus source mux
    input  logic [3:0]  i_reg_a_sel,    // Register address A (IR-indirect or literal)
    input  logic [3:0]  i_reg_b_sel,    // Register address B (IR-indirect or literal)
    input  logic [3:0]  i_reg_w_sel,    // Register address W (IR-indirect or literal)
    input  logic        i_reg_w_en,     // Register write enable (pre-F-bit gating)
    input  logic [4:0]  i_alu_op,       // ALU operation
    input  logic [1:0]  i_b_mux_sel,    // B-bus source mux
    input  logic        i_w_mux_sel,    // Write-back source mux
    input  logic [1:0]  i_imm_mode,     // Immediate extension mode
    input  logic        i_flag_w_en,    // Latch NZCV from ALU
    input  logic        i_sr_load,      // Bulk-load SR from W-mux
    input  logic        i_mar_load,     // Load MAR from R-bus
    input  logic        i_mdr_load_mem, // Load MDR from memory
    input  logic        i_mdr_load_a,   // Load MDR from A-bus
    input  logic [1:0]  i_mem_size,     // Memory access size (from microcode)
    input  logic        i_sign_ext,     // Sign-extend sub-word load (from microcode)
    input  logic [2:0]  i_pc_src,       // PC source mux
    input  logic        i_alu_start,    // Start multi-cycle ALU op
    input  logic        i_pc_load,      // Load PC from pc_mux output
    input  logic        i_cross_bank,   // R14 opposite bank (GETUSP/SETUSP)

    // ══════════════════════════════════════════════════════════════
    // Exception / interrupt control (from fetch unit)
    // ══════════════════════════════════════════════════════════════
    input  logic        i_except_entry, // Hardware pre-action: snapshot + mode switch
    input  logic [3:0]  i_vector_num,   // Exception vector number (from priority encoder)

    // ══════════════════════════════════════════════════════════════
    // EI / DI control (from micro-sequencer)
    // ══════════════════════════════════════════════════════════════
    input  logic        i_ei_set,       // EI instruction
    input  logic        i_di_set,       // DI instruction
    input  logic        i_ei_shadow_clr,// Fetch unit clears ei_shadow

    // ══════════════════════════════════════════════════════════════
    // Instruction Register control (from fetch unit)
    // ══════════════════════════════════════════════════════════════
    input  logic        i_ir_load,      // Latch IR from i_mem_rdata

    // ══════════════════════════════════════════════════════════════
    // D-cache / memory interface
    // ══════════════════════════════════════════════════════════════
    input  logic [31:0] i_mem_rdata,    // Memory read data → MDR, IR
    output logic [31:0] o_mem_addr,     // MAR → D-cache address
    output logic [31:0] o_mem_wdata,    // MDR → D-cache write data

    // ══════════════════════════════════════════════════════════════
    // Status outputs (to micro-sequencer / fetch unit)
    // ══════════════════════════════════════════════════════════════
    output logic        o_alu_busy,     // ALU multi-cycle in progress
    output logic        o_sr_s,         // Supervisor bit
    output logic        o_sr_i,         // Interrupt enable bit
    output logic        o_ei_shadow,    // EI one-instruction delay active
    output logic        o_cond_result,  // Condition evaluator output (for BRT/BRF)

    // ══════════════════════════════════════════════════════════════
    // PC output (to I-cache, fetch unit)
    // ══════════════════════════════════════════════════════════════
    output logic [31:0] o_pc,           // Current PC → I-cache address

    // ══════════════════════════════════════════════════════════════
    // Field extractor outputs (to fetch unit for dispatch, micro-sequencer)
    // ══════════════════════════════════════════════════════════════
    output logic [1:0]  o_format,       // IR[31:30] — instruction format
    output logic [4:0]  o_r_op,         // Format R opcode (for dispatch)
    output logic [2:0]  o_l_op,         // Format L opcode (for dispatch)
    output logic        o_m_load,       // Format M load/store bit
    output logic [1:0]  o_m_size,       // Format M access size
    output logic        o_m_sign_ext,   // Format M sign-extend
    output logic [3:0]  o_b_cond,       // Format B condition code
    output logic [3:0]  o_r_sys_dev,    // WRSYS/RDSYS device field
    output logic [3:0]  o_r_sys_reg,    // WRSYS/RDSYS register field

    // ══════════════════════════════════════════════════════════════
    // A-bus output (for sysreg write data path)
    // ══════════════════════════════════════════════════════════════
    output logic [31:0] o_a_bus,        // A-bus value → sysreg write data

    // ── Debug port ───────────────────────────────────────────
    input  logic [3:0]  i_dbg_reg_addr,
    output logic [31:0] o_dbg_reg_data
);

    // ── Register address routing encoding ────────────────────
    // The micro-word's reg_*_sel fields use this encoding:
    //   4'b0000 = IR_RD: format-dependent destination register
    //   4'b0001 = IR_RS: format-dependent source/base register
    //   4'b0010–4'b1111 = literal register R2–R15
    localparam logic [3:0] REG_SEL_IR_RD = 4'd0;
    localparam logic [3:0] REG_SEL_IR_RS = 4'd1;

    // ── Internal buses ───────────────────────────────────────
    logic [31:0] a_bus;         // A-bus (from A-mux)
    logic [31:0] b_bus;         // B-bus (from B-mux)
    logic [31:0] r_bus;         // R-bus (ALU result)

    assign o_a_bus = a_bus;     // Expose for sysreg write data
    logic [31:0] w_bus;         // W-bus (from W-mux → regfile write, SR load)

    // ── IR register ──────────────────────────────────────────
    logic [31:0] ir;

    always_ff @(posedge i_clk) begin
        if (i_rst)
            ir <= 32'b0;
        else if (i_ir_load)
            ir <= i_mem_rdata;
    end

    // ── Field extractor ──────────────────────────────────────
    logic [1:0]  fmt;
    logic [4:0]  fe_r_op;
    logic [3:0]  fe_r_rd, fe_r_rs;
    logic        fe_r_f;
    logic [3:0]  fe_r_sys_dev, fe_r_sys_reg;
    logic [2:0]  fe_l_op;
    logic [3:0]  fe_l_rd;
    logic        fe_m_load;
    logic [1:0]  fe_m_size;
    logic        fe_m_sign_ext;
    logic [3:0]  fe_m_rd, fe_m_rb;
    logic [3:0]  fe_b_cond;
    logic [15:0] fe_imm16, fe_m_offset16;
    logic [21:0] fe_b_offset22;

    field_ext u_field_ext (
        .i_ir           (ir),
        .o_format       (fmt),
        .o_r_op         (fe_r_op),
        .o_r_rd         (fe_r_rd),
        .o_r_rs         (fe_r_rs),
        .o_r_f          (fe_r_f),
        .o_r_sys_dev    (fe_r_sys_dev),
        .o_r_sys_reg    (fe_r_sys_reg),
        .o_l_op         (fe_l_op),
        .o_l_rd         (fe_l_rd),
        .o_m_load       (fe_m_load),
        .o_m_size       (fe_m_size),
        .o_m_sign_ext   (fe_m_sign_ext),
        .o_m_rd         (fe_m_rd),
        .o_m_rb         (fe_m_rb),
        .o_b_cond       (fe_b_cond),
        .o_imm16        (fe_imm16),
        .o_m_offset16   (fe_m_offset16),
        .o_b_offset22   (fe_b_offset22)
    );

    // Expose field extractor outputs for dispatch and control
    assign o_format     = fmt;
    assign o_r_op       = fe_r_op;
    assign o_l_op       = fe_l_op;
    assign o_m_load     = fe_m_load;
    assign o_m_size     = fe_m_size;
    assign o_m_sign_ext = fe_m_sign_ext;
    assign o_b_cond     = fe_b_cond;
    assign o_r_sys_dev  = fe_r_sys_dev;
    assign o_r_sys_reg  = fe_r_sys_reg;

    // ── Register address routing ─────────────────────────────
    // Resolves IR-indirect codes (0=IR_RD, 1=IR_RS) into actual
    // register addresses based on the instruction format. Codes
    // 2–15 pass through as literal register numbers.
    logic [3:0] resolved_a, resolved_b, resolved_w;

    // IR_RD: format-dependent destination register
    logic [3:0] ir_rd_resolved;
    always_comb begin
        case (fmt)
            2'b00:   ir_rd_resolved = fe_r_rd;   // Format R: IR[24:21]
            2'b01:   ir_rd_resolved = fe_l_rd;   // Format L: IR[26:23]
            2'b10:   ir_rd_resolved = fe_m_rd;   // Format M: IR[25:22]
            default: ir_rd_resolved = 4'b0000;   // Format B: don't care
        endcase
    end

    // IR_RS: format-dependent source/base register
    logic [3:0] ir_rs_resolved;
    always_comb begin
        case (fmt)
            2'b00:   ir_rs_resolved = fe_r_rs;   // Format R: IR[20:17]
            2'b10:   ir_rs_resolved = fe_m_rb;   // Format M: IR[21:18]
            default: ir_rs_resolved = 4'b0000;   // Formats L, B: don't care
        endcase
    end

    // Resolve each register address field
    function automatic logic [3:0] resolve_reg_sel;
        input logic [3:0] sel;
        input logic [3:0] rd;
        input logic [3:0] rs;
        case (sel)
            REG_SEL_IR_RD: resolve_reg_sel = rd;
            REG_SEL_IR_RS: resolve_reg_sel = rs;
            default:       resolve_reg_sel = sel;  // Literal: value IS the register number
        endcase
    endfunction

    assign resolved_a = resolve_reg_sel(i_reg_a_sel, ir_rd_resolved, ir_rs_resolved);
    assign resolved_b = resolve_reg_sel(i_reg_b_sel, ir_rd_resolved, ir_rs_resolved);
    assign resolved_w = resolve_reg_sel(i_reg_w_sel, ir_rd_resolved, ir_rs_resolved);

    // ── F-bit write-enable gating ────────────────────────────
    // For Format R flag-only variants (CMP, TEST): IR[16]=1 suppresses
    // the register write. Only applies when the write address comes from
    // IR (IR_RD encoding) — literal register writes (exception entry etc.)
    // are never gated by the F bit.
    logic actual_w_en;
    assign actual_w_en = i_reg_w_en
        & ~(fmt == 2'b00 && fe_r_f && i_reg_w_sel == REG_SEL_IR_RD);

    // ── Register file ────────────────────────────────────────
    logic [31:0] reg_a_data, reg_b_data;
    logic [31:0] pc_value;

    regfile u_regfile (
        .i_clk        (i_clk),
        .i_rst        (i_rst),
        .i_rd_addr_a  (resolved_a),
        .o_rd_data_a  (reg_a_data),
        .i_rd_addr_b  (resolved_b),
        .o_rd_data_b  (reg_b_data),
        .i_wr_addr    (resolved_w),
        .i_wr_data    (w_bus),
        .i_wr_en      (actual_w_en),
        .i_pc         (pc_value),
        .i_supervisor  (sr_s_wire),
        .i_cross_bank  (i_cross_bank),
        .i_dbg_addr    (i_dbg_reg_addr),
        .o_dbg_data    (o_dbg_reg_data)
    );

    // ── Status register ──────────────────────────────────────
    logic        sr_flag_n, sr_flag_z, sr_flag_c, sr_flag_v;
    logic        sr_s_wire, sr_i_wire;
    // verilator lint_off UNUSEDSIGNAL
    logic [31:0] sr_read;   // Full SR word — used by GETSR (future micro-op path)
    // verilator lint_on UNUSEDSIGNAL
    logic [31:0] esr;
    logic        ei_shadow_wire;

    status_reg u_status_reg (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_alu_flag_n   (alu_flag_n),
        .i_alu_flag_z   (alu_flag_z),
        .i_alu_flag_c   (alu_flag_c),
        .i_alu_flag_v   (alu_flag_v),
        .i_flag_w_en    (i_flag_w_en),
        .i_sr_load      (i_sr_load),
        .i_wdata        (w_bus),
        .i_except_entry (i_except_entry),
        .i_ei_set       (i_ei_set),
        .i_di_set       (i_di_set),
        .i_ei_shadow_clr(i_ei_shadow_clr),
        .o_flag_n       (sr_flag_n),
        .o_flag_z       (sr_flag_z),
        .o_flag_c       (sr_flag_c),
        .o_flag_v       (sr_flag_v),
        .o_sr_s         (sr_s_wire),
        .o_sr_i         (sr_i_wire),
        .o_sr_read      (sr_read),
        .o_esr    (esr),
        .o_ei_shadow    (ei_shadow_wire)
    );

    assign o_sr_s      = sr_s_wire;
    assign o_sr_i      = sr_i_wire;
    assign o_ei_shadow = ei_shadow_wire;

    // ── A-bus source mux ─────────────────────────────────────
    // Vector address = vec_num << 2 (pre-shifted, just wiring)
    logic [31:0] vector_addr;
    assign vector_addr = {26'b0, i_vector_num, 2'b00};

    logic [31:0] epc;

    amux u_amux (
        .i_reg_a       (reg_a_data),
        .i_esr   (esr),
        .i_epc   (epc),
        .i_vector_addr (vector_addr),
        .i_sel         (i_a_src),
        .o_a_bus       (a_bus)
    );

    // ── Immediate source routing ─────────────────────────────
    // Format L uses IR[15:0] (fe_imm16), Format M uses IR[17:2] (fe_m_offset16).
    // Select based on instruction format.
    logic [15:0] imm16_routed;
    assign imm16_routed = (fmt == 2'b10) ? fe_m_offset16 : fe_imm16;

    // ── Immediate extractor ──────────────────────────────────
    logic [31:0] imm32;

    imm_ext u_imm_ext (
        .i_imm16 (imm16_routed),
        .i_mode  (i_imm_mode),
        .o_imm32 (imm32)
    );

    // ── B-bus source mux ─────────────────────────────────────
    bmux u_bmux (
        .i_reg_b  (reg_b_data),
        .i_imm32  (imm32),
        .i_sel    (i_b_mux_sel),
        .o_b_bus  (b_bus)
    );

    // ── ALU ──────────────────────────────────────────────────
    logic        alu_flag_n, alu_flag_z, alu_flag_c, alu_flag_v;

    alu u_alu (
        .i_clk    (i_clk),
        .i_rst    (i_rst),
        .i_a      (a_bus),
        .i_b      (b_bus),
        .i_op     (i_alu_op),
        .i_start  (i_alu_start),
        .o_busy   (o_alu_busy),
        .o_result (r_bus),
        .o_flag_z (alu_flag_z),
        .o_flag_n (alu_flag_n),
        .o_flag_c (alu_flag_c),
        .o_flag_v (alu_flag_v)
    );

    // ── MDR ──────────────────────────────────────────────────
    logic [31:0] mdr_data;

    mdr u_mdr (
        .i_clk      (i_clk),
        .i_rst      (i_rst),
        .i_load_mem (i_mdr_load_mem),
        .i_load_a   (i_mdr_load_a),
        .i_mem_data (i_mem_rdata),
        .i_a_bus    (a_bus),
        .o_data     (mdr_data)
    );

    assign o_mem_wdata = mdr_data;

    // ── Byte extractor (sub-word load support) ───────────────
    // Extracts and sign/zero-extends byte or halfword from the
    // full 32-bit MDR value, based on the MAR address offset and
    // mem_size/sign_ext from the microcode. For word-size accesses
    // (including RDSYS), passes through unchanged.
    logic [31:0] mdr_extracted;

    byte_ext u_byte_ext (
        .i_data     (mdr_data),
        .i_addr_lo  (mar_out[1:0]),
        .i_size     (i_mem_size),
        .i_sign_ext (i_sign_ext),
        .o_data     (mdr_extracted)
    );

    // ── W-bus source mux ─────────────────────────────────────
    wmux u_wmux (
        .i_r_bus  (r_bus),
        .i_mdr    (mdr_extracted),
        .i_sel    (i_w_mux_sel),
        .o_wr_data(w_bus)
    );

    // ── MAR ──────────────────────────────────────────────────
    logic [31:0] mar_out;

    mar u_mar (
        .i_clk  (i_clk),
        .i_rst  (i_rst),
        .i_load (i_mar_load),
        .i_rbus (r_bus),
        .o_addr (mar_out)
    );

    assign o_mem_addr = mar_out;

    // ── PC unit ──────────────────────────────────────────────
    logic [31:0] pc_plus4, pc_offset, pc_next;

    pc_reg u_pc_reg (
        .i_clk          (i_clk),
        .i_rst          (i_rst),
        .i_pc_load      (i_pc_load),
        .i_pc_next      (pc_next),
        .i_offset22     (fe_b_offset22),
        .i_except_entry (i_except_entry),
        .o_pc           (pc_value),
        .o_pc_plus4     (pc_plus4),
        .o_pc_offset    (pc_offset),
        .o_epc    (epc)
    );

    assign o_pc = pc_value;

    // ── PC source mux ────────────────────────────────────────
    pc_mux u_pc_mux (
        .i_pc_current (pc_value),
        .i_pc_plus4   (pc_plus4),
        .i_pc_offset  (pc_offset),
        .i_a_bus      (a_bus),
        .i_mdr        (mdr_data),
        .i_sel        (i_pc_src),
        .o_pc_next    (pc_next)
    );

    // ── Condition evaluator ──────────────────────────────────
    cond_eval u_cond_eval (
        .i_flag_z (sr_flag_z),
        .i_flag_n (sr_flag_n),
        .i_flag_c (sr_flag_c),
        .i_flag_v (sr_flag_v),
        .i_cond   (fe_b_cond),
        .o_taken  (o_cond_result)
    );

endmodule
