// penumbra2_regmap — ISA-to-physical register mapping for the ID stage.
//
// Specified by doc/internals/penumbra2/hazard-model.md. Translates
// the architectural register references of an instruction into the
// physical scoreboard entries the scoreboard and regfile use, and
// raises cross_bank for the SPR-USP access.
//
// This module does *translation*, not *composition*: it is told which
// references are live (the enables) and whether each is a GPR or an
// SPR reference, and maps each to its physical entry. Deciding which
// instruction fields are sources/destinations — which depends on the
// specific opcode (e.g. MOV reads Rs but not Rd, while LUI reads and
// writes Rd) — belongs to the ID decoder, which drives this module's
// inputs.
//
// The mapping is physical, so R14's two banks are distinct entries:
// R14 maps to USP or SSP by SR.S, while RDSPR/WRSPR USP maps to USP
// regardless of mode (raising cross_bank). This is what lets the
// scoreboard catch a supervisor WRSPR-USP aliasing a later user-mode
// R14 read — both touch physical entry USP.

module penumbra2_regmap
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_supervisor,   // SR.S

    // Source A reference: a GPR (i_src_a_is_spr=0) or SPR number.
    input  logic [3:0]            i_src_a_sel,
    input  logic                  i_src_a_is_spr,
    input  logic                  i_src_a_en,
    // Source B reference.
    input  logic [3:0]            i_src_b_sel,
    input  logic                  i_src_b_is_spr,
    input  logic                  i_src_b_en,
    // Destination reference.
    input  logic [3:0]            i_dst_sel,
    input  logic                  i_dst_is_spr,
    input  logic                  i_dst_en,
    // Divmul second destination (Rdh) — always a GPR.
    input  logic [3:0]            i_dst_hi_sel,
    input  logic                  i_dst_hi_en,

    // Physical scoreboard entries + their enables.
    output logic [SB_IDX_W-1:0]   o_src_a,
    output logic                  o_src_a_en,
    output logic [SB_IDX_W-1:0]   o_src_b,
    output logic                  o_src_b_en,
    output logic [SB_IDX_W-1:0]   o_dst,
    output logic                  o_dst_en,
    output logic [SB_IDX_W-1:0]   o_dst_hi,
    output logic                  o_dst_hi_en,

    // SPR-USP cross-bank access (reaches USP while SR.S=1).
    output logic                  o_cross_bank
);

    // reg_to_phys: an architectural GPR reference (R0-R15) to its
    // physical scoreboard entry. The interesting case is R14, which
    // is physically two entries (USP/SSP) selected by SR.S — the
    // banking that makes the scoreboard physically addressed.
    function automatic logic [SB_IDX_W-1:0] reg_to_phys(
        input logic [3:0] arch,
        input logic       sup
    );
        case (arch)
            REG_SP:  return sup ? SB_SSP : SB_USP;
            REG_PC:  return '0;
            default: return SB_IDX_W'(arch);   // R0..R13 → entries 0..13
        endcase
    endfunction

    // spr_to_phys: an SPR number (RDSPR/WRSPR) to its physical entry.
    // USP aliases R14's user bank. SR has no entry: its NZCV bits are
    // forwarded and its S/I bits are drain-serialized, so the decoder
    // never presents SR as an enabled scoreboard reference (asserted
    // below).
    function automatic logic [SB_IDX_W-1:0] spr_to_phys(input logic [3:0] spr);
        case (spr)
            SPR_USP:  return SB_USP;
            SPR_ESR:  return SB_ESR;
            SPR_EPC:  return SB_EPC;
            SPR_SCR0: return SB_SCR0;
            SPR_SCR1: return SB_SCR1;
            SPR_SCR2: return SB_SCR2;
            SPR_SCR3: return SB_SCR3;
            default:  return '0;   // includes SR — never a live ref
        endcase
    endfunction

    // Map one reference (GPR or SPR) to a physical entry.
    function automatic logic [SB_IDX_W-1:0] map_ref(
        input logic [3:0] sel,
        input logic       is_spr,
        input logic       sup
    );
        return is_spr ? spr_to_phys(sel) : reg_to_phys(sel, sup);
    endfunction

    assign o_src_a    = map_ref(i_src_a_sel, i_src_a_is_spr, i_supervisor);
    assign o_src_a_en = i_src_a_en;
    assign o_src_b    = map_ref(i_src_b_sel, i_src_b_is_spr, i_supervisor);
    assign o_src_b_en = i_src_b_en;
    assign o_dst      = map_ref(i_dst_sel, i_dst_is_spr, i_supervisor);
    assign o_dst_en   = i_dst_en;
    assign o_dst_hi   = reg_to_phys(i_dst_hi_sel, i_supervisor);
    assign o_dst_hi_en = i_dst_hi_en;

    // cross_bank: a live SPR reference to USP. Only RDSPR/WRSPR USP
    // produce this; normal R14 access (is_spr=0) never does.
    assign o_cross_bank =
          (i_src_a_en && i_src_a_is_spr && i_src_a_sel == SPR_USP)
        || (i_src_b_en && i_src_b_is_spr && i_src_b_sel == SPR_USP)
        || (i_dst_en   && i_dst_is_spr   && i_dst_sel   == SPR_USP);

    // ══════════════════════════════════════════════════════════
    // Assertions — contract violations a correct decoder cannot
    // produce. Sim-only (Verilator --assert); stripped at synth.
    // ══════════════════════════════════════════════════════════
    always_comb begin
        // PC (R15) is never a scoreboard entry: it resolves to the PC
        // value, not a regfile/scoreboard slot. A live GPR
        // reference to it would silently map to entry 0.
        assert (!(i_src_a_en && !i_src_a_is_spr && i_src_a_sel == REG_PC))
            else $error("penumbra2_regmap: R15/PC as live GPR source A");
        assert (!(i_src_b_en && !i_src_b_is_spr && i_src_b_sel == REG_PC))
            else $error("penumbra2_regmap: R15/PC as live GPR source B");
        assert (!(i_dst_en && !i_dst_is_spr && i_dst_sel == REG_PC))
            else $error("penumbra2_regmap: R15/PC as live GPR destination");
        assert (!(i_dst_hi_en && i_dst_hi_sel == REG_PC))
            else $error("penumbra2_regmap: R15/PC as divmul high destination");

        // SPR references must name a real SPR (ESR..SCR3 = 0..7);
        // anything else falls through spr_to_phys to entry 0.
        assert (!(i_src_a_en && i_src_a_is_spr && i_src_a_sel > SPR_SCR3))
            else $error("penumbra2_regmap: invalid SPR number on source A");
        assert (!(i_src_b_en && i_src_b_is_spr && i_src_b_sel > SPR_SCR3))
            else $error("penumbra2_regmap: invalid SPR number on source B");
        assert (!(i_dst_en && i_dst_is_spr && i_dst_sel > SPR_SCR3))
            else $error("penumbra2_regmap: invalid SPR number on destination");

        // SR is not a scoreboard entry — its NZCV bits are forwarded
        // and its S/I bits are drain-serialized. A correct decoder
        // never presents SR as an enabled scoreboard reference; doing
        // so would fall through spr_to_phys to entry 0.
        assert (!(i_src_a_en && i_src_a_is_spr && i_src_a_sel == SPR_SR))
            else $error("penumbra2_regmap: SR as a scoreboard source A");
        assert (!(i_src_b_en && i_src_b_is_spr && i_src_b_sel == SPR_SR))
            else $error("penumbra2_regmap: SR as a scoreboard source B");
        assert (!(i_dst_en && i_dst_is_spr && i_dst_sel == SPR_SR))
            else $error("penumbra2_regmap: SR as a scoreboard destination");
    end

endmodule
