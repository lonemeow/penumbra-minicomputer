// verilator lint_off UNUSEDSIGNAL
// Penumbra Micro-Sequencer — micro-PC management and micro-word decode
//
// Manages the micro-PC that indexes into the microcode ROM, decodes
// branch_cond to determine the next micro-PC, and fans out the packed
// 51-bit micro-word into individual control signals for the datapath.
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
    input  logic        i_mem_fault,      // MMU fault (TLB miss / protection)
    input  logic        i_arith_fault,    // divmul DIV0/overflow → VEC_ARITH
    input  logic        i_cond_result,    // Condition evaluator output
    input  logic        i_sr_s,           // Supervisor mode (for priv bit check)

    // ── Microcode ROM interface ──────────────────────────────
    output logic [7:0]  o_upc,            // Micro-PC → ROM address
    // verilator lint_off UNUSEDSIGNAL
    input  logic [51:0] i_uword,          // Micro-word from ROM
    // verilator lint_on UNUSEDSIGNAL

    // ── Datapath control outputs (decoded micro-word) ────────
    output logic [2:0]  o_a_src,
    output logic [3:0]  o_reg_a_sel,
    output logic [3:0]  o_reg_b_sel,
    output logic [3:0]  o_reg_w_sel,
    output logic        o_reg_w_en,
    output logic [4:0]  o_alu_op,
    output logic [1:0]  o_b_mux_sel,
    output logic [1:0]  o_wb_src,        // writeback source: RBUS/MDR/DML_LO/DML_HI
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
    output logic [1:0]  o_sys_op,        // 00=NONE, 01=SPR_WRITE, 10=SYS_READ, 11=SYS_WRITE
    output logic        o_alu_start,
    output logic        o_pc_load,

    // ── State output ────────────────────────────────────────────
    // Direct flop output — combinational `state == S_FETCH`.  Driven
    // out so cpu_top can route mmu_vaddr / cache muxing from it
    // without depending on the priv-block-gated o_pc_load chain.
    output logic        o_fetch_active,

    // ── Illegal instruction / privilege violation detection ─────
    output logic        o_illegal,        // First micro-op is sentinel (branch=7)
    output logic        o_priv_violation, // First micro-op has priv=1 in user mode

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
    // Extract from the 52-bit packed word (bits 51:0)
    logic        uw_priv;
    logic [2:0]  uw_a_src;
    logic [3:0]  uw_reg_a, uw_reg_b, uw_reg_w;
    logic        uw_w_en;
    logic [4:0]  uw_alu_op;
    logic [1:0]  uw_bmux;
    logic [1:0]  uw_wb_src;
    logic [1:0]  uw_imm_mode;
    logic        uw_flag_w_en, uw_sr_load;
    logic        uw_mar_load, uw_mdr_load_mem, uw_mdr_load_a;
    logic        uw_mem_read, uw_mem_write;
    logic [1:0]  uw_mem_size;
    logic        uw_sign_ext;
    logic [2:0]  uw_pc_src;
    logic [1:0]  uw_sys_op;
    logic        uw_alu_start;
    logic [2:0]  uw_branch;
    logic [2:0]  uw_fwd_offset;
    logic        uw_ei_set;
    logic        uw_di_set;

    assign uw_priv         = i_uword[51];
    assign uw_a_src        = i_uword[50:48];
    assign uw_reg_a        = i_uword[47:44];
    assign uw_reg_b        = i_uword[43:40];
    assign uw_reg_w        = i_uword[39:36];
    assign uw_w_en         = i_uword[35];
    assign uw_alu_op       = i_uword[34:30];
    assign uw_bmux         = i_uword[29:28];
    assign uw_wb_src       = i_uword[27:26];
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
    assign uw_sys_op       = i_uword[10:9];
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
    // BR_PRIV removed — privilege check moved to dispatch time in cpu_top
    localparam logic [2:0] BR_SKIP    = 3'd6;
    localparam logic [2:0] BR_ILLEGAL = 3'd7;

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
                if (i_mem_fault || i_arith_fault)
                    go_fetch = 1'b1;  // Abort on MMU or arithmetic (divmul) fault
                else if (!busy)
                    advance = 1'b1;
                // busy → hold (neither advance nor fetch)
            end
            BR_BRT:   go_fetch = 1'b1;
            BR_BRF:   go_fetch = 1'b1;
            BR_SKIP:    advance  = 1'b1;  // fwd_offset handled in next_upc calc
            BR_ILLEGAL: go_fetch = 1'b1;  // Abort: unused ROM entry (sentinel)
            default: ;  // 3'd5 unused
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

    // ── Privilege check: suppress all enables on priv violation ─
    // priv=1 in micro-word + user mode → block execution, abort to fetch.
    // In discrete: one AND gate (priv & !SR.S) per enable line.
    logic priv_block;
    assign priv_block = executing & uw_priv & !i_sr_s;

    logic exec_en;  // executing AND not blocked by privilege
    assign exec_en = executing & !priv_block;

    assign o_upc         = upc;
    assign o_fetch_go    = (state == S_EXEC) && (go_fetch || priv_block);

    assign o_a_src       = exec_en ? uw_a_src        : 3'b0;
    assign o_reg_a_sel   = exec_en ? uw_reg_a        : 4'b0;
    assign o_reg_b_sel   = exec_en ? uw_reg_b        : 4'b0;
    // reg_w_sel is NOT gated — it must remain stable through the posedge
    // where the last micro-op's write completes. w_en is gated, so a
    // stale address during S_FETCH or priv_block is harmless.
    assign o_reg_w_sel   = uw_reg_w;
    assign o_reg_w_en    = exec_en ? uw_w_en         : 1'b0;
    assign o_alu_op      = exec_en ? uw_alu_op       : 5'b0;
    assign o_b_mux_sel   = exec_en ? uw_bmux         : 2'b0;
    assign o_wb_src      = uw_wb_src;  // Not gated — must be stable at write posedge
    assign o_imm_mode    = exec_en ? uw_imm_mode     : 2'b0;
    assign o_flag_w_en   = exec_en ? uw_flag_w_en    : 1'b0;
    assign o_sr_load     = exec_en ? uw_sr_load      : 1'b0;
    assign o_mar_load    = exec_en ? uw_mar_load     : 1'b0;
    assign o_mdr_load_mem= exec_en ? uw_mdr_load_mem : 1'b0;
    assign o_mdr_load_a  = exec_en ? uw_mdr_load_a   : 1'b0;
    assign o_mem_read    = exec_en ? uw_mem_read     : 1'b0;
    assign o_mem_write   = exec_en ? uw_mem_write    : 1'b0;
    assign o_mem_size    = exec_en ? uw_mem_size     : 2'b0;
    assign o_sign_ext    = exec_en ? uw_sign_ext     : 1'b0;
    assign o_pc_src      = exec_en ? effective_pc_src : 3'b0;
    assign o_sys_op      = exec_en ? uw_sys_op       : 2'b0;
    assign o_alu_start   = exec_en ? uw_alu_start    : 1'b0;
    assign o_pc_load     = exec_en;   // Suppressed on priv violation (no PC change)

    // ── EI/DI decode and ei_shadow_clr tracking ───────────────
    assign o_ei_set = exec_en ? uw_ei_set : 1'b0;
    assign o_di_set = exec_en ? uw_di_set : 1'b0;

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

    // ── Illegal instruction: sentinel detected on first micro-op ─
    assign o_illegal = executing & (uw_branch == BR_ILLEGAL);

    // ── Privilege violation: priv=1 attempted in user mode ────────
    assign o_priv_violation = priv_block;

    // ── Fetch-state flag: pure flop output, no dependency on
    //    priv_block/pc_load.  cpu_top uses this to drive mmu_vaddr
    //    and cache muxing in parallel with the priv check.
    assign o_fetch_active = (state == S_FETCH);

endmodule
