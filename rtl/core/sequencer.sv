// Penumbra Micro-Sequencer — micro-PC management and micro-word decode
//
// Manages the micro-PC that indexes into the microcode ROM, decodes
// branch_cond to determine the next micro-PC, and fans out the packed
// 49-bit micro-word into individual control signals for the datapath.
//
// States:
//   FETCH — waiting for fetch unit to provide a new dispatch address
//   EXEC  — executing micro-ops from ROM, advancing micro-PC
//
// During FETCH, all datapath control signals are driven to safe defaults
// (zeros = hold PC, no writes, no memory access).

module sequencer
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── Fetch unit interface ─────────────────────────────────
    input  logic        i_ir_valid,       // Fetch complete, dispatch ready
    input  logic [7:0]  i_dispatch_addr,  // Micro-PC start for new instruction
    output logic        o_fetch_go,       // Request new instruction fetch

    // ── Datapath status inputs ───────────────────────────────
    input  logic        i_alu_busy,       // ALU multi-cycle in progress
    input  logic        i_mem_busy,       // Memory/cache busy
    input  logic        i_cond_result,    // Condition evaluator output
    input  logic        i_sr_s,           // Supervisor bit (for PRIV check)

    // ── Microcode ROM interface ──────────────────────────────
    output logic [7:0]  o_upc,            // Micro-PC → ROM address
    // verilator lint_off UNUSEDSIGNAL
    input  logic [48:0] i_uword,          // Micro-word from ROM (bits 1:0 are spare)
    // verilator lint_on UNUSEDSIGNAL

    // ── Datapath control outputs (decoded micro-word) ────────
    output logic [1:0]  o_a_src,
    output logic [3:0]  o_reg_a_sel,
    output logic [3:0]  o_reg_b_sel,
    output logic [3:0]  o_reg_w_sel,
    output logic        o_reg_w_en,
    output logic [4:0]  o_alu_op,
    output logic [1:0]  o_b_mux_sel,
    output logic        o_w_mux_sel,
    output logic [1:0]  o_imm_mode,
    output logic        o_flag_w_en,
    output logic        o_sr_load,
    output logic        o_mar_load,
    output logic        o_mdr_load_mem,
    output logic        o_mdr_load_a,
    output logic        o_mem_read,
    output logic        o_mem_write,
    output logic [1:0]  o_mem_size,
    output logic        o_sign_ext,
    output logic [2:0]  o_pc_src,
    output logic        o_sys_cycle,
    output logic        o_sys_we,
    output logic        o_alu_start,
    output logic        o_pc_load,

    // ── EI/DI outputs ─────────────────────────────────────────
    output logic        o_ei_set,
    output logic        o_di_set,
    output logic        o_ei_shadow_clr  // Clears ei_shadow after one instruction
);

    // ── State machine ────────────────────────────────────────
    typedef enum logic {
        S_FETCH,
        S_EXEC
    } state_t;

    state_t state, next_state;
    logic [7:0] upc, next_upc;

    // ── Micro-word field extraction ──────────────────────────
    // Extract from the 49-bit packed word (bits 48:0)
    logic [1:0]  uw_a_src;
    logic [3:0]  uw_reg_a, uw_reg_b, uw_reg_w;
    logic        uw_w_en;
    logic [4:0]  uw_alu_op;
    logic [1:0]  uw_bmux;
    logic        uw_wmux;
    logic [1:0]  uw_imm_mode;
    logic        uw_flag_w_en, uw_sr_load;
    logic        uw_mar_load, uw_mdr_load_mem, uw_mdr_load_a;
    logic        uw_mem_read, uw_mem_write;
    logic [1:0]  uw_mem_size;
    logic        uw_sign_ext;
    logic [2:0]  uw_pc_src;
    logic        uw_sys_cycle, uw_sys_we, uw_alu_start;
    logic [2:0]  uw_branch;
    logic [2:0]  uw_fwd_offset;
    logic        uw_ei_set;
    logic        uw_di_set;

    assign uw_a_src        = i_uword[48:47];
    assign uw_reg_a        = i_uword[46:43];
    assign uw_reg_b        = i_uword[42:39];
    assign uw_reg_w        = i_uword[38:35];
    assign uw_w_en         = i_uword[34];
    assign uw_alu_op       = i_uword[33:29];
    assign uw_bmux         = i_uword[28:27];
    assign uw_wmux         = i_uword[26];
    assign uw_imm_mode     = i_uword[25:24];
    assign uw_flag_w_en    = i_uword[23];
    assign uw_sr_load      = i_uword[22];
    assign uw_mar_load     = i_uword[21];
    assign uw_mdr_load_mem = i_uword[20];
    assign uw_mdr_load_a   = i_uword[19];
    assign uw_mem_read     = i_uword[18];
    assign uw_mem_write    = i_uword[17];
    assign uw_mem_size     = i_uword[16:15];
    assign uw_sign_ext     = i_uword[14];
    assign uw_pc_src       = i_uword[13:11];
    assign uw_sys_cycle    = i_uword[10];
    assign uw_sys_we       = i_uword[9];
    assign uw_alu_start    = i_uword[8];
    assign uw_branch       = i_uword[7:5];
    assign uw_fwd_offset   = i_uword[4:2];
    assign uw_ei_set       = i_uword[1];
    assign uw_di_set       = i_uword[0];

    // ── Branch condition encoding ────────────────────────────
    localparam logic [2:0] BR_SEQ   = 3'd0;
    localparam logic [2:0] BR_FETCH = 3'd1;
    localparam logic [2:0] BR_STALL = 3'd2;
    localparam logic [2:0] BR_BRT   = 3'd3;
    localparam logic [2:0] BR_BRF   = 3'd4;
    localparam logic [2:0] BR_PRIV  = 3'd5;
    localparam logic [2:0] BR_SKIP  = 3'd6;

    // ── Unified busy signal ──────────────────────────────────
    logic busy;
    assign busy = i_alu_busy | i_mem_busy;

    // ── Branch condition decode ──────────────────────────────
    // Determines: should we go to fetch? should we advance micro-PC?
    logic go_fetch;     // Transition to FETCH state
    logic advance;      // micro-PC++ (or skip)

    always_comb begin
        go_fetch = 1'b0;
        advance  = 1'b0;

        case (uw_branch)
            BR_SEQ:   advance = 1'b1;
            BR_FETCH: go_fetch = 1'b1;
            BR_STALL: begin
                if (!busy)
                    advance = 1'b1;
                // busy → hold (neither advance nor fetch)
            end
            BR_BRT:   go_fetch = 1'b1;
            BR_BRF:   go_fetch = 1'b1;
            BR_PRIV: begin
                if (i_sr_s)
                    advance = 1'b1;     // Supervisor: proceed
                else
                    go_fetch = 1'b1;    // User: exception (fetch unit handles)
            end
            BR_SKIP: advance = 1'b1;    // fwd_offset handled in next_upc calc
            default: ;
        endcase
    end

    // ── PC source override for conditional branches ──────────
    // BRT: if condition false, force pc_src to NEXT (PC+4)
    // BRF: if condition true, force pc_src to NEXT (PC+4)
    logic [2:0] effective_pc_src;
    always_comb begin
        effective_pc_src = uw_pc_src;
        if (uw_branch == BR_BRT && !i_cond_result)
            effective_pc_src = 3'd1;  // NEXT
        if (uw_branch == BR_BRF && i_cond_result)
            effective_pc_src = 3'd1;  // NEXT
    end

    // ── State and micro-PC update ────────────────────────────
    always_comb begin
        next_state = state;
        next_upc   = upc;

        case (state)
            S_FETCH: begin
                if (i_ir_valid) begin
                    next_state = S_EXEC;
                    next_upc   = i_dispatch_addr;
                end
            end

            S_EXEC: begin
                if (go_fetch) begin
                    next_state = S_FETCH;
                end else if (advance) begin
                    if (uw_branch == BR_SKIP)
                        next_upc = upc + 8'(uw_fwd_offset) + 8'd1;
                    else
                        next_upc = upc + 8'd1;
                end
                // else: hold (STALL with busy=1)
            end
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            state <= S_FETCH;
            upc   <= 8'd0;
        end else begin
            state <= next_state;
            upc   <= next_upc;
        end
    end

    // ── Output mux: ROM fields during EXEC, zeros during FETCH ─
    logic executing;
    assign executing = (state == S_EXEC);

    assign o_upc         = upc;
    assign o_fetch_go    = (state == S_EXEC) && go_fetch;

    assign o_a_src       = executing ? uw_a_src        : 2'b0;
    assign o_reg_a_sel   = executing ? uw_reg_a        : 4'b0;
    assign o_reg_b_sel   = executing ? uw_reg_b        : 4'b0;
    assign o_reg_w_sel   = executing ? uw_reg_w        : 4'b0;
    assign o_reg_w_en    = executing ? uw_w_en         : 1'b0;
    assign o_alu_op      = executing ? uw_alu_op       : 5'b0;
    assign o_b_mux_sel   = executing ? uw_bmux         : 2'b0;
    assign o_w_mux_sel   = executing ? uw_wmux         : 1'b0;
    assign o_imm_mode    = executing ? uw_imm_mode     : 2'b0;
    assign o_flag_w_en   = executing ? uw_flag_w_en    : 1'b0;
    assign o_sr_load     = executing ? uw_sr_load      : 1'b0;
    assign o_mar_load    = executing ? uw_mar_load     : 1'b0;
    assign o_mdr_load_mem= executing ? uw_mdr_load_mem : 1'b0;
    assign o_mdr_load_a  = executing ? uw_mdr_load_a   : 1'b0;
    assign o_mem_read    = executing ? uw_mem_read     : 1'b0;
    assign o_mem_write   = executing ? uw_mem_write    : 1'b0;
    assign o_mem_size    = executing ? uw_mem_size     : 2'b0;
    assign o_sign_ext    = executing ? uw_sign_ext     : 1'b0;
    assign o_pc_src      = executing ? effective_pc_src : 3'b0;
    assign o_sys_cycle   = executing ? uw_sys_cycle    : 1'b0;
    assign o_sys_we      = executing ? uw_sys_we       : 1'b0;
    assign o_alu_start   = executing ? uw_alu_start    : 1'b0;
    assign o_pc_load     = executing;  // PC loads from mux every exec cycle
                                       // (pc_src=HOLD is a safe no-op)

    // ── EI/DI decode and ei_shadow_clr tracking ───────────────
    assign o_ei_set = executing ? uw_ei_set : 1'b0;
    assign o_di_set = executing ? uw_di_set : 1'b0;

    // ei_pending tracks that EI executed; the NEXT go_fetch clears ei_shadow
    logic ei_pending;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            ei_pending <= 1'b0;
        end else begin
            if (executing && uw_ei_set)
                ei_pending <= 1'b1;
            else if (ei_pending && go_fetch && executing)
                ei_pending <= 1'b0;
        end
    end

    assign o_ei_shadow_clr = ei_pending & go_fetch & executing;

endmodule
