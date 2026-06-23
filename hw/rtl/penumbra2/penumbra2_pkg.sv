// penumbra2_pkg — Penumbra/2 core-internal constants.
//
// Holds microarchitectural constants private to the gen2 pipelined
// core: scoreboard entry indices, and (as the core grows) pipeline
// stage and control-bundle encodings. These are implementation
// internals, not part of the ISA — the ISA contract and the
// system/peripheral register maps shared with the rest of the
// machine live in the common penumbra_pkg.

/* verilator lint_off UNUSEDPARAM */
package penumbra2_pkg;

    // ── Scoreboard entries ──────────────────────────────────────
    // The scoreboard is a flat array of physical entries, one valid
    // bit each (see doc/internals/penumbra2/hazard-model.md). Entries
    // are *physical*, so R14's two banks (USP/SSP) and the SPRs are
    // distinct entries — this is what catches the WRSPR-USP / R14
    // aliasing hazard.
    //
    // Entry 0 (R0) is reserved-unused: the decoder never emits it as
    // a source or destination, so its valid bit is tied 1. Entries
    // 1..13 map directly from architectural R1..R13. The named
    // entries below cover the banked SP and the scoreboarded SPRs.
    //
    // No part of SR is a scoreboard entry: the NZCV flags are resolved
    // by forwarding (MEM/WB->EX), and the S and I bits are serialized
    // by drain-commit. Neither needs a valid bit.
    localparam int SB_NUM_ENTRIES = 22;                  // entries 0..21 (21 live)
    localparam int SB_IDX_W       = $clog2(SB_NUM_ENTRIES);

    localparam logic [SB_IDX_W-1:0] SB_USP  = 5'd14;  // R14 (user) / RDSPR/WRSPR USP, any mode
    localparam logic [SB_IDX_W-1:0] SB_SSP  = 5'd15;  // R14 (supervisor)
    localparam logic [SB_IDX_W-1:0] SB_ESR  = 5'd16;  // RDSPR/WRSPR ESR
    localparam logic [SB_IDX_W-1:0] SB_EPC  = 5'd17;  // RDSPR/WRSPR EPC
    localparam logic [SB_IDX_W-1:0] SB_SCR0 = 5'd18;  // RDSPR/WRSPR SCR0
    localparam logic [SB_IDX_W-1:0] SB_SCR1 = 5'd19;  // RDSPR/WRSPR SCR1
    localparam logic [SB_IDX_W-1:0] SB_SCR2 = 5'd20;  // RDSPR/WRSPR SCR2
    localparam logic [SB_IDX_W-1:0] SB_SCR3 = 5'd21;  // RDSPR/WRSPR SCR3

    // ── ALU operation select ────────────────────────────────────
    // The `alu_op` control field the ID decoder hands to the EX-stage
    // ALU (see doc/internals/penumbra2/control-decode.md). Twelve
    // single-cycle functions; MOV is realised as pass (the operand
    // mux routes the moved value onto B, so pass = pass-B).
    //
    // These values equal the ISA Format R op[3:0] in the single-cycle
    // (op[4]=0) region, so the decoder forwards op[3:0] directly as
    // alu_op with no remap.
    localparam int ALU_OP_W = 4;

    localparam logic [ALU_OP_W-1:0] ALU_ADD  = 4'd0;
    localparam logic [ALU_OP_W-1:0] ALU_SUB  = 4'd1;
    localparam logic [ALU_OP_W-1:0] ALU_AND  = 4'd2;
    localparam logic [ALU_OP_W-1:0] ALU_OR   = 4'd3;
    localparam logic [ALU_OP_W-1:0] ALU_XOR  = 4'd4;
    localparam logic [ALU_OP_W-1:0] ALU_SHL  = 4'd5;
    localparam logic [ALU_OP_W-1:0] ALU_SHR  = 4'd6;
    localparam logic [ALU_OP_W-1:0] ALU_SAR  = 4'd7;
    localparam logic [ALU_OP_W-1:0] ALU_PASS = 4'd8;   // MOV: pass operand B
    localparam logic [ALU_OP_W-1:0] ALU_NOT  = 4'd9;
    localparam logic [ALU_OP_W-1:0] ALU_ADC  = 4'd10;  // add with carry
    localparam logic [ALU_OP_W-1:0] ALU_SBC  = 4'd11;  // subtract with borrow

    // ── ID control-bundle op_class ──────────────────────────────
    // The coarse instruction class the ID decoder hands downstream
    // (see doc/internals/penumbra2/control-decode.md). Downstream
    // stages branch on op_class for *structural* routing — EX pulses
    // divmul.start on OPC_DIVMUL, enters the drain-commit FSM on the
    // drain-commit classes, resolves a branch on OPC_BRANCH — while the
    // finer control bits (alu_op, mem_size, the write-enables) carry
    // the per-instruction specifics.
    //
    // The ALU-result classes the doc lists separately (alu, alu_imm,
    // move) collapse into one OPC_ALU here: MOV/LLI/LLIS decode to a
    // PASS through the ALU and LUI to an OR, so the alu_op + operand-mux
    // fields already distinguish them — a separate class would buy
    // nothing. This encoding is a core-internal decode→pipeline
    // contract, so it lives here, not in the shared ISA package.
    localparam int OPC_W = 4;

    localparam logic [OPC_W-1:0] OPC_ALU     = 4'd0;   // ALU/move → GPR and/or flags
    localparam logic [OPC_W-1:0] OPC_LOAD    = 4'd1;
    localparam logic [OPC_W-1:0] OPC_STORE   = 4'd2;
    localparam logic [OPC_W-1:0] OPC_BRANCH  = 4'd3;   // Format B (incl. BL)
    localparam logic [OPC_W-1:0] OPC_JMP     = 4'd4;   // Format L JMP/JALR
    localparam logic [OPC_W-1:0] OPC_DIVMUL  = 4'd5;
    localparam logic [OPC_W-1:0] OPC_RDSPR   = 4'd6;
    localparam logic [OPC_W-1:0] OPC_WRSPR   = 4'd7;
    localparam logic [OPC_W-1:0] OPC_RDSYS   = 4'd8;
    localparam logic [OPC_W-1:0] OPC_WRSYS   = 4'd9;
    localparam logic [OPC_W-1:0] OPC_ERET    = 4'd10;
    localparam logic [OPC_W-1:0] OPC_EI      = 4'd11;
    localparam logic [OPC_W-1:0] OPC_DI      = 4'd12;
    localparam logic [OPC_W-1:0] OPC_SYSCALL = 4'd13;
    localparam logic [OPC_W-1:0] OPC_BREAK   = 4'd14;
    localparam logic [OPC_W-1:0] OPC_ILLEGAL = 4'd15;  // reserved/undefined opcode

    // ── Memory operation select (ctrl_mem) ──────────────────────
    localparam int MEM_OP_W = 2;
    localparam logic [MEM_OP_W-1:0] MEM_NONE  = 2'd0;
    localparam logic [MEM_OP_W-1:0] MEM_LOAD  = 2'd1;
    localparam logic [MEM_OP_W-1:0] MEM_STORE = 2'd2;

    // ── Bubble cause tag (perfctr stall attribution) ────────────
    // Each pipeline bubble carries the cause that injected it, set where the
    // bubble is born and propagated unchanged as the bubble flows to the commit
    // point — so a non-retiring cycle is charged to its true cause regardless of
    // how far upstream, or how many cycles earlier, the originating stall was
    // (the live-stall-signal attribution it replaces mis-timed short stalls and
    // the latency tails of long ones into the front-end residual). Meaningful
    // only on a bubble (a valid slot retires and is charged to nothing); the
    // commit point decodes it into the SYSREG_CPU_STALL_* counters. One tag per
    // counter — see the stall-attribution contract in doc/system/sysregs.md.
    //
    // Back-end causes (LOAD/STORE/FUNIT/HAZARD) ride the bubble token through
    // ID→EX→MEM→WB. Front-end causes are split where the front-end bubble enters
    // ID: a redirect kill or cold/refill gap is FLUSH; an empty fetch while a
    // fetch is outstanding to memory is IFETCH.
    localparam int BCAUSE_W = 3;
    localparam logic [BCAUSE_W-1:0] BCAUSE_NONE   = 3'd0;  // a retiring slot — charged to nothing
    localparam logic [BCAUSE_W-1:0] BCAUSE_FLUSH  = 3'd1;  // front-end redirect / fill, not memory-bound
    localparam logic [BCAUSE_W-1:0] BCAUSE_IFETCH = 3'd2;  // front-end starved on a memory-bound fetch
    localparam logic [BCAUSE_W-1:0] BCAUSE_LOAD   = 3'd3;  // MEM holding for a load access
    localparam logic [BCAUSE_W-1:0] BCAUSE_STORE  = 3'd4;  // MEM holding for a store access
    localparam logic [BCAUSE_W-1:0] BCAUSE_FUNIT  = 3'd5;  // EX waiting on the multi-cycle unit (divmul)
    localparam logic [BCAUSE_W-1:0] BCAUSE_HAZARD = 3'd6;  // ID issue blocked by a pipeline interlock

endpackage
/* verilator lint_on UNUSEDPARAM */
