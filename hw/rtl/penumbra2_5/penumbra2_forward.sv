// penumbra2_forward — Penumbra/2.5 GPR operand forwarding select.
//
// Combinational. The 32-bit-operand analogue of penumbra2_flag_bypass:
// supplies one EX operand with the youngest in-flight value its source
// register has, so a reader need not stall until its producer commits.
// Specified by the gen2.5 forwarding section of
// doc/internals/penumbra2/hazard-model.md.
//
// Two forward sources, youngest-first, falling back to the operand as read
// in ID (which already carries the committed / write-through value):
//   - the EX/MEM-stage producer (one ahead), if it writes this source   (youngest)
//   - else the MEM/WB-stage producer (two ahead), if it writes this source
//   - else i_fallback
//
// EX/MEM beats MEM/WB because the MEM-stage instruction is younger. The
// caller gates each source's *valid* with the right readiness condition: the
// EX/MEM source must exclude a load/RDSYS (its value is not yet in the
// register, only an address/EA there), while the MEM/WB source admits them
// (MEM has produced the value into the register). This module only selects;
// it does not know which producer is a load.
//
// i_fwd_en is the per-operand forwardability bit (the operand is a
// scoreboarded register read, not an immediate / PC / SPR-file source). It
// gates the whole select off, so an immediate operand is never overwritten.

module penumbra2_forward
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    // The consumer's source: physical entry + whether it may forward.
    input  logic [SB_IDX_W-1:0] i_phys_src,
    input  logic                i_fwd_en,
    input  logic [31:0]         i_fallback,    // operand as read in ID

    // EX/MEM-stage producer (youngest forward source).
    input  logic [SB_IDX_W-1:0] i_mem_dst,
    input  logic [31:0]         i_mem_value,
    input  logic                i_mem_valid,   // gated writer (already excludes load/RDSYS)

    // MEM/WB-stage producer.
    input  logic [SB_IDX_W-1:0] i_wb_dst,
    input  logic [31:0]         i_wb_value,
    input  logic                i_wb_valid,    // gated writer

    output logic [31:0]         o_value
);

    logic mem_hit, wb_hit;
    assign mem_hit = i_fwd_en & i_mem_valid & (i_phys_src == i_mem_dst);
    assign wb_hit  = i_fwd_en & i_wb_valid  & (i_phys_src == i_wb_dst);

    always_comb begin
        if      (mem_hit) o_value = i_mem_value;   // youngest: EX/MEM
        else if (wb_hit)  o_value = i_wb_value;     // MEM/WB
        else              o_value = i_fallback;     // committed / write-through
    end

endmodule
