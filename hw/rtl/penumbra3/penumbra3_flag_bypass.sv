// penumbra3_flag_bypass -- Penumbra/3 EX-stage NZCV flag forwarding.
//
// Combinational. Supplies the EX flag consumer (Bcc condition eval and the
// ADC/SBC carry-in) with the most recent NZCV, so a flag reader never stalls
// on its producer. NZCV is forwarded, never scoreboarded.
//
// The youngest in-flight flag writer ahead of the EX reader wins, falling
// back to the committed architectural SR when none is in flight. Three
// pipeline stages sit ahead of EX (MEM1, MEM2, WB), so there are three
// forwardable producer slots:
//
//   - the MEM1-stage instruction, if it writes flags   (youngest)
//   - else the MEM2-stage instruction, if it writes flags
//   - else the WB-stage instruction, if it writes flags
//   - else the committed SR flag bits
//
// A reader in EX can never share EX with its producer, so the closest a
// producer sits is one stage ahead, in MEM1 -- there is deliberately no
// EX->EX leg. After a flush the pipe holds no in-flight producer and the
// consumer reads committed SR, so this needs no scoreboard or flush logic.
//
// The 4-bit NZCV packing is the EX ALU's convention and is opaque here: the
// bypass only selects among four same-shaped values.

module penumbra3_flag_bypass (
    // Committed architectural NZCV (the SR flag bits).
    input  logic [3:0] i_sr_flags,

    // MEM1-stage in-flight producer (from the EX/MEM1 register).
    input  logic [3:0] i_mem1_flags,
    input  logic       i_mem1_writes_flags,

    // MEM2-stage in-flight producer (from the MEM1/MEM2 register).
    input  logic [3:0] i_mem2_flags,
    input  logic       i_mem2_writes_flags,

    // WB-stage in-flight producer (from the MEM2/WB register).
    input  logic [3:0] i_wb_flags,
    input  logic       i_wb_writes_flags,

    // Forwarded NZCV for the EX flag consumer.
    output logic [3:0] o_flags
);

    // -- Youngest-first flag select ---------------------------------
    always_comb begin
        if      (i_mem1_writes_flags) o_flags = i_mem1_flags;
        else if (i_mem2_writes_flags) o_flags = i_mem2_flags;
        else if (i_wb_writes_flags)   o_flags = i_wb_flags;
        else                          o_flags = i_sr_flags;
    end

endmodule
