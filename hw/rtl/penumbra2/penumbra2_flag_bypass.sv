// penumbra2_flag_bypass — Penumbra/2 EX-stage NZCV flag forwarding.
//
// Combinational. Supplies the EX flag consumer (Bcc condition eval,
// ADC/SBC carry-in) with the most recent NZCV value, so a flag reader
// never stalls on its producer. Specified by the flag-forwarding model
// in doc/internals/penumbra2/hazard-model.md.
//
// NZCV is not scoreboarded; it is forwarded. The youngest in-flight
// flag producer ahead of the EX reader wins, falling back to the
// committed architectural SR when none is in flight:
//
//   - the MEM-stage instruction, if it writes flags  (youngest)
//   - else the WB-stage instruction, if it writes flags
//   - else the committed SR flag bits
//
// MEM beats WB because the MEM-stage instruction entered the pipeline
// later and is therefore younger. There is deliberately no EX→EX leg:
// a reader in EX can never share EX with its producer, so the closest a
// producer can be is one stage ahead, in MEM. After a flush the
// pipeline holds no in-flight producers, so the consumer reads
// committed SR — which is why this needs no scoreboard or squash logic.
//
// NZCV is four condition flags (N, Z, C, V); the 4-bit bundle's
// internal packing is the EX ALU's convention and is opaque here — the
// bypass only selects among three same-shaped values.

module penumbra2_flag_bypass (
    // Committed architectural NZCV (the SR flag bits).
    input  logic [3:0] i_sr_flags,

    // MEM-stage in-flight flag producer (from the EX/MEM register).
    input  logic [3:0] i_mem_flags,
    input  logic       i_mem_writes_flags,

    // WB-stage in-flight flag producer (from the MEM/WB register).
    input  logic [3:0] i_wb_flags,
    input  logic       i_wb_writes_flags,

    // Forwarded NZCV for the EX flag consumer.
    output logic [3:0] o_flags
);

    // ── Youngest-first flag select ───────────────────────────────
    always_comb begin
        if (i_mem_writes_flags)
            o_flags = i_mem_flags;
        else if (i_wb_writes_flags)
            o_flags = i_wb_flags;
        else
            o_flags = i_sr_flags;
    end

endmodule
