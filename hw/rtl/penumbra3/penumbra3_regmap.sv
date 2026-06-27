// penumbra3_regmap -- architectural register field -> scoreboard index.
//
// Maps one instruction register field (R0-R15) to its physical scoreboard
// index, resolving the R14 bank by privilege. It stays in ID (not pre-mapped
// at enqueue) precisely because it depends on live i_supervisor -- a
// mode-change in flight ahead of the slot must not see a stale bank. It is a
// shallow index select feeding the issue cone P0.2 measures; instantiated
// once per source/destination field.
module penumbra3_regmap #(
    parameter int IDX_BITS = 5
) (
    input  logic [3:0]          i_areg,        // register field, R0-R15
    input  logic                i_supervisor,  // 1 = supervisor (R14 -> SSP)
    output logic [IDX_BITS-1:0] o_pidx,        // physical scoreboard index
    output logic                o_tracked      // 0 for untracked regs (R0, PC)
);

    import penumbra_pkg::*;

    // Scoreboard index assignment: R1..R13 map directly to 1..13; R14 banks
    // by privilege; R0 (zero) and R15 (PC) are untracked.
    localparam logic [IDX_BITS-1:0] SB_USP = 5'd14;  // R14 in user mode
    localparam logic [IDX_BITS-1:0] SB_SSP = 5'd15;  // R14 in supervisor mode

    // R0 (zero) and R15 (PC) are untracked; R1..R13 map directly to their
    // index; R14 banks by privilege. i_areg is a registered field, so this
    // flat select starts from a flop. Defaults first to keep it latch-free.
    always_comb begin
        o_pidx    = '0;
        o_tracked = 1'b0;
        if (i_areg != REG_ZERO && i_areg != REG_PC) begin
            o_pidx    = (i_areg == REG_SP) ? (i_supervisor ? SB_SSP : SB_USP)
                                           : IDX_BITS'(i_areg);
            o_tracked = 1'b1;
        end
    end

endmodule
