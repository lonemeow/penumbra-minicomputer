// penumbra2_regfile — Penumbra/2 register file.
// 16-entry, 2-read, 1-write, 32-bit, physically indexed.
//
// Specified by doc/internals/penumbra2/regfile.md. Stores the GPR/SP
// slice (entries 0-15) of the scoreboard's physical namespace; the
// SPR entries 16-22 live in other modules. Addressed by physical
// entry index on both read ports (in ID) and the write port (the
// phys_dst carried to WB).
//
// The ISA->physical mapping — R14->USP/SSP by SR.S, the cross-bank
// RDSPR/WRSPR USP case, R0/R15 — is done once in ID by
// penumbra2_regmap. By the time an index reaches this file it is a
// plain physical entry, so the file does no banking conditional and
// sees no SR.S: index 14 is USP, index 15 is SSP, full stop.
//
// Entry layout:
//   0      R0       — read override forces 0 (array slot 0 is dead)
//   1-13   R1-R13   — distributed RAM, replicated per read port
//   14     USP      — dedicated flop
//   15     SSP      — dedicated flop
//
// R15 (PC) is not an entry here: R15 reads are resolved by the ID
// operand mux selecting the PC, and R15 "writes" are branches, so
// R15 never reaches a regfile port.
//
// Reads are asynchronous and writes synchronous, with no
// write-through: a read in the same cycle a register is written
// returns the old value, visible on the next cycle.
//
// A divmul result writes two destinations; the writeback stage drives
// this single write port on two consecutive cycles (low half then
// high half). From this module's view that is two ordinary writes.

module penumbra2_regfile
    import penumbra_pkg::*;
    import penumbra2_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // Read port A (combinational — read by ID in the decode cycle)
    input  logic [SB_IDX_W-1:0]   i_rd_idx_a,
    output logic [31:0]           o_rd_data_a,

    // Read port B (combinational)
    input  logic [SB_IDX_W-1:0]   i_rd_idx_b,
    output logic [31:0]           o_rd_data_b,

    // Write port (synchronous, driven at WB)
    input  logic [SB_IDX_W-1:0]   i_wr_idx,
    input  logic [31:0]           i_wr_data,
    input  logic                  i_wr_en
);

    // ── Storage ─────────────────────────────────────────────────
    // Two 16x32 distributed-RAM banks, one per read port. Writes fan
    // out to both so the banks stay identical. Slots 0/14/15 are
    // written too but never read — the override mux wins for them.
    (* ram_style = "distributed" *) logic [31:0] regs_a [0:15];
    (* ram_style = "distributed" *) logic [31:0] regs_b [0:15];

    // Banked stack pointers — flops with sync reset to a defined 0.
    logic [31:0] usp;
    logic [31:0] ssp;

    // ── Read logic ──────────────────────────────────────────────
    // Each port reads its replicated array copy and override_read
    // picks the final value by physical index:
    //
    //   idx == 0  → 0    (R0 hardwired zero)
    //   idx == 14 → USP
    //   idx == 15 → SSP
    //   else      → stored value
    //
    function automatic logic [31:0] override_read(
        input logic [SB_IDX_W-1:0] idx,
        input logic [31:0]         stored_value
    );
        case (idx)
            '0:      return 32'd0;
            SB_USP:  return usp;
            SB_SSP:  return ssp;
            default: return stored_value;
        endcase
    endfunction

    assign o_rd_data_a = override_read(i_rd_idx_a, regs_a[i_rd_idx_a[3:0]]);
    assign o_rd_data_b = override_read(i_rd_idx_b, regs_b[i_rd_idx_b[3:0]]);

    // ── Write logic ─────────────────────────────────────────────
    // Synchronous write to both array banks plus, for index 14/15,
    // the selected SP flop. The array write is decode-free (every
    // index writes its slot; 0/14/15 land in dead slots).
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            usp <= 32'd0;
            ssp <= 32'd0;
        end else if (i_wr_en) begin
            if (i_wr_idx == SB_USP)
                usp <= i_wr_data;
            else if (i_wr_idx == SB_SSP)
                ssp <= i_wr_data;
            regs_a[i_wr_idx[3:0]] <= i_wr_data;
            regs_b[i_wr_idx[3:0]] <= i_wr_data;
        end
    end

    // ══════════════════════════════════════════════════════════
    // Assertions — sim-only (Verilator --assert); stripped at synth.
    // Only GPR/SP entries (0..15) are written here; an SPR (16..22)
    // write means an SPR-targeted write was misrouted to the regfile
    // instead of its owning module.
    // ══════════════════════════════════════════════════════════
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_wr_en && i_wr_idx > SB_SSP))
        else $error("penumbra2_regfile: write index outside GPR/SP range");

endmodule
