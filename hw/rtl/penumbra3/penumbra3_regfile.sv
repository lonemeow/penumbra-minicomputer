// penumbra3_regfile -- Penumbra/3 register file.
// 16-entry, 2-read, 1-write, 32-bit, physically indexed.
//
// Stores the GPR/SP slice (physical scoreboard entries 0-15) of the gen3
// namespace; the SPR entries 16-21 live in penumbra3_spr_file /
// penumbra3_scratch_file. Both read ports (driven in ID) and the write port
// (the phys_dst carried to WB) address a plain physical entry: penumbra3_regmap
// already resolved R14->USP/SSP by SR.S and the cross-bank RDSPR/WRSPR USP
// case, so this file does no banking conditional and never sees SR.S -- index
// 14 is USP, index 15 is SSP, full stop.
//
// Entry layout:
//   0      R0       -- read override forces 0 (array slot 0 is dead)
//   1-13   R1-R13   -- distributed RAM, replicated per read port
//   14     USP      -- dedicated flop
//   15     SSP      -- dedicated flop
//
// R15 (PC) is not an entry: an R15 source is resolved by the ID operand mux
// selecting the live PC, and an R15 "write" is a branch, so R15 never reaches a
// port here.
//
// Reads are combinational; writes are synchronous with no write-through (a read
// in the cycle a register is written returns the old value, visible next
// cycle). A divmul writes two destinations as two ordinary writes on
// consecutive cycles -- from this file's view, nothing special.

module penumbra3_regfile
    import penumbra_pkg::*;
    import penumbra3_pkg::*;
(
    input  logic                  i_clk,
    input  logic                  i_rst,

    // Read port A (combinational -- read by ID in the issue cycle)
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

    // -- Storage --------------------------------------------------
    // Two 16x32 distributed-RAM banks, one per read port; writes fan out to
    // both so they stay identical. Slots 0/14/15 are written too but never read
    // -- the override mux wins for them.
    (* ram_style = "distributed" *) logic [31:0] regs_a [0:15];
    (* ram_style = "distributed" *) logic [31:0] regs_b [0:15];

    // Banked stack pointers -- flops with sync reset to a defined 0.
    logic [31:0] usp;
    logic [31:0] ssp;

    // -- Read logic -----------------------------------------------
    // Each port reads its replicated array copy; override_read picks the final
    // value by physical index (R0 -> 0, 14 -> USP, 15 -> SSP, else stored).
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

    // -- Write logic ----------------------------------------------
    // Synchronous write to both array banks plus, for index 14/15, the selected
    // SP flop. The array write is decode-free (every index writes its slot;
    // 0/14/15 land in dead slots the override mux ignores).
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

    // -- Assertion (sim-only; Verilator --assert) -----------------
    // Only GPR/SP entries (0..15) are written here; an SPR (16..21) write means
    // an SPR-targeted commit was misrouted to the regfile instead of its owning
    // module.
    assert property (@(posedge i_clk) disable iff (i_rst)
        !(i_wr_en && i_wr_idx > SB_SSP))
        else $error("penumbra3_regfile: write index outside GPR/SP range");

endmodule
