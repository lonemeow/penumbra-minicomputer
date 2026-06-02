// penumbra2_regfile — Penumbra/2 register file.
// 16-register, 2-read, 1-write, 32-bit.
//
// Specified by doc/internals/penumbra2/regfile.md. The ISA-visible
// register model (R0=zero, R1-R13 general purpose, R14 banked
// USP/SSP, R15=PC alias) is fixed by doc/system/architecture.md;
// this module realises it for the pipelined core.
//
// Storage:
//   R1-R13 live in 16-deep distributed-RAM banks, replicated once
//   per read port (A, B) so each bank is 1W/1R — the canonical ECP5
//   SLICEMEM (DPR16X*) shape. Both banks are written in lockstep so
//   they hold identical data. Slots 0/14/15 are written too but
//   never read: the override mux always wins for those addresses.
//
//   R0 (zero), R14 (banked USP/SSP) and R15 (PC alias) are an output
//   override on the storage read. USP and SSP are two dedicated
//   flops, selected by a single XOR (regfile.md §5):
//       sp_select = SR.S ^ cross_bank      // 0 -> USP, 1 -> SSP
//
// Register liveness is not tracked here: the scoreboard gates issue
// on in-flight writers, so the storage reads as undefined until
// written (boot establishes it).
//
// Reads are asynchronous and writes synchronous, with no
// write-through: a read in the same cycle a register is written
// returns the old value, visible on the next cycle (regfile.md §7).
//
// A divmul result writes two destinations; the writeback stage
// drives this single write port on two consecutive cycles (low half
// then high half). From this module's view that is two ordinary
// writes (regfile.md §4).

module penumbra2_regfile
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // Read port A (combinational — read by ID in the decode cycle)
    input  logic [3:0]  i_rd_addr_a,
    output logic [31:0] o_rd_data_a,

    // Read port B (combinational)
    input  logic [3:0]  i_rd_addr_b,
    output logic [31:0] o_rd_data_b,

    // Write port (synchronous, driven at WB)
    input  logic [3:0]  i_wr_addr,
    input  logic [31:0] i_wr_data,
    input  logic        i_wr_en,

    // Special inputs
    input  logic [31:0] i_pc,          // PC value — returned when reading R15
    input  logic        i_supervisor,  // SR.S — selects SSP (1) vs USP (0) for R14
    input  logic        i_cross_bank   // RDSPR/WRSPR USP from supervisor: flip the bank
);

    // ── Storage ─────────────────────────────────────────────────
    // Two 16x32 distributed-RAM banks, one per read port. Writes
    // fan out to both so the banks stay identical.
    (* ram_style = "distributed" *) logic [31:0] regs_a [0:15];
    (* ram_style = "distributed" *) logic [31:0] regs_b [0:15];

    // Banked stack pointer — flops with sync reset to a defined 0.
    logic [31:0] usp;
    logic [31:0] ssp;

    // sp_select: 1 = SSP, 0 = USP, after applying cross_bank.
    logic sp_select;
    assign sp_select = i_supervisor ^ i_cross_bank;

    // ── Read logic ──────────────────────────────────────────────
    // Each port reads its replicated storage copy in parallel with
    // the address decode; override_read picks the final value:
    //
    //   addr == R0  → 0          (hardwired zero)
    //   addr == R14 → banked SP  (sp_sel ? ssp : usp)
    //   addr == R15 → i_pc       (PC alias)
    //   else        → stored value
    //
    function automatic logic [31:0] override_read(
        input logic [3:0]  addr,
        input logic [31:0] stored_value,
        input logic        sp_sel
    );
        case (addr)
            REG_ZERO: return 32'd0;
            REG_SP:   return sp_sel ? ssp : usp;
            REG_PC:   return i_pc;
            default:  return stored_value;
        endcase
    endfunction

    assign o_rd_data_a = override_read(i_rd_addr_a, regs_a[i_rd_addr_a], sp_select);
    assign o_rd_data_b = override_read(i_rd_addr_b, regs_b[i_rd_addr_b], sp_select);

    // ── Write logic ─────────────────────────────────────────────
    // Synchronous write to both array banks plus (for R14) the
    // active SP flop. R0/R15 writes land in the array but are never
    // read out — the override always wins — so the write enable
    // skips the address compare and stays decode-free.
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            usp <= 32'd0;
            ssp <= 32'd0;
        end else if (i_wr_en) begin
            if (i_wr_addr == REG_SP) begin
                if (sp_select)
                    ssp <= i_wr_data;
                else
                    usp <= i_wr_data;
            end
            regs_a[i_wr_addr] <= i_wr_data;
            regs_b[i_wr_addr] <= i_wr_data;
        end
    end

    // No assertions here: the file tolerates any 4-bit write address
    // by design (R0/R15 land in dead slots the read override wins
    // over), so it has no "can't happen" of its own. The invariant
    // that R15 is never a GPR write target belongs to the decoder and
    // is asserted in penumbra2_regmap.

endmodule
