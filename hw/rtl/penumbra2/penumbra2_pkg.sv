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
    localparam int SB_NUM_ENTRIES = 23;                  // entries 0..22 (22 live)
    localparam int SB_IDX_W       = $clog2(SB_NUM_ENTRIES);

    localparam logic [SB_IDX_W-1:0] SB_USP  = 5'd14;  // R14 (user) / RDSPR/WRSPR USP, any mode
    localparam logic [SB_IDX_W-1:0] SB_SSP  = 5'd15;  // R14 (supervisor)
    localparam logic [SB_IDX_W-1:0] SB_ESR  = 5'd16;  // RDSPR/WRSPR ESR
    localparam logic [SB_IDX_W-1:0] SB_EPC  = 5'd17;  // RDSPR/WRSPR EPC
    localparam logic [SB_IDX_W-1:0] SB_NZCV = 5'd18;  // SR condition flags (only NZCV; not S/I)
    localparam logic [SB_IDX_W-1:0] SB_SCR0 = 5'd19;  // RDSPR/WRSPR SCR0
    localparam logic [SB_IDX_W-1:0] SB_SCR1 = 5'd20;  // RDSPR/WRSPR SCR1
    localparam logic [SB_IDX_W-1:0] SB_SCR2 = 5'd21;  // RDSPR/WRSPR SCR2
    localparam logic [SB_IDX_W-1:0] SB_SCR3 = 5'd22;  // RDSPR/WRSPR SCR3

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

endpackage
/* verilator lint_on UNUSEDPARAM */
