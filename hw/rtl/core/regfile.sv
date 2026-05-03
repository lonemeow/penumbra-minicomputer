// Penumbra Register File — 16-register, 2-read, 1-write, 32-bit
//
// Architecture:
//   R0:      Hardwired to zero (reads always return 0, writes ignored)
//   R1–R13:  General-purpose registers
//   R14:     Stack pointer, banked: USP (user) or SSP (supervisor)
//            Selected by i_supervisor — when SR.S=1, R14 reads/writes SSP
//   R15:     Reads return i_pc (from the separate PC register)
//            R15 is not stored here — it's a read-only alias
//
// Ports:
//   Two combinational read ports (A, B) for the datapath, one
//   debug read port (also combinational), one synchronous write
//   port that latches data on rising clock edge.
//
// Storage strategy (distributed-RAM friendly):
//   R1-R13 live in 16-deep DPRAM banks, replicated per read port
//   (A, B, debug) so each bank is 1W/1R — the canonical ECP5
//   SLICEMEM (DPR16X*) shape.  All three banks are written in
//   lockstep so they always carry identical data.
//
//   R0 (hardwired zero), R14 (banked USP/SSP), and R15 (PC alias)
//   are special-cased by an output mux that overrides the DPRAM
//   read for those addresses.  Slots 0/14/15 of the DPRAM banks
//   are written along with everything else but their values are
//   never read — the override always wins.  Keeping the write
//   path address-independent avoids extra decode on the storage
//   write enables.
//
//   USP/SSP remain in flops because (a) supervisor banking is a
//   special-case anyway and (b) keeping SP under sync reset gives
//   us a clean post-reset stack pointer state.  A 16-bit
//   `regs_valid` mask in flops preserves reset-to-zero semantics
//   for R1-R13 without requiring SLICEMEM clear: each bit gates
//   the DPRAM read for one slot (clear → return 0; first write
//   sets it).  Costs one LUT4 level on the read path, well worth
//   it to keep the original "i_rst zeros R1-R13" contract.

module regfile
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // Read port A
    input  logic [3:0]  i_rd_addr_a,
    output logic [31:0] o_rd_data_a,

    // Read port B
    input  logic [3:0]  i_rd_addr_b,
    output logic [31:0] o_rd_data_b,

    // Write port (synchronous)
    input  logic [3:0]  i_wr_addr,
    input  logic [31:0] i_wr_data,
    input  logic        i_wr_en,

    // Special inputs
    input  logic [31:0] i_pc,          // PC value — returned when reading R15
    input  logic        i_supervisor,  // SR.S — selects SSP (1) vs USP (0) for R14
    input  logic        i_cross_bank,  // Access opposite R14 bank (GETUSP/SETUSP)

    // Debug read port
    input  logic [3:0]  i_dbg_addr,
    output logic [31:0] o_dbg_data
);

    // ── Storage ─────────────────────────────────────────────────
    // Three 16×32 DPRAM banks, one per read port.  Writes fan out
    // to all three so the banks stay identical.

    (* ram_style = "distributed" *) logic [31:0] regs_a   [0:15];
    (* ram_style = "distributed" *) logic [31:0] regs_b   [0:15];
    (* ram_style = "distributed" *) logic [31:0] regs_dbg [0:15];

    // Per-slot valid bits — flops with sync reset.  Cleared on
    // i_rst, set on first write to a slot.  Read path returns 0
    // for slots whose bit is clear, preserving "all zero after
    // reset" semantics.
    logic [15:0] regs_valid;

    // Banked stack pointer — flops with sync reset.
    logic [31:0] usp;
    logic [31:0] ssp;

    // sp_select: 1 = SSP, 0 = USP, after applying cross_bank.
    logic sp_select;
    assign sp_select = i_supervisor ^ i_cross_bank;

    // ── Read logic ──────────────────────────────────────────────
    // Each port: one SLICEMEM async read in parallel with the
    // address decode; the override mux picks the right value.
    //
    //   addr == 0  → return 0       (R0 hardwired)
    //   addr == 14 → return banked SP
    //   addr == 15 → return i_pc
    //   else       → return DPRAM[addr]

    function automatic logic [31:0] override_read(
        input logic [3:0]  addr,
        input logic [31:0] dpram_data,
        input logic        valid_bit,
        input logic        sp_sel
    );
        case (addr)
            REG_ZERO: override_read = 32'd0;
            REG_SP:   override_read = sp_sel ? ssp : usp;
            REG_PC:   override_read = i_pc;
            default:  override_read = valid_bit ? dpram_data : 32'd0;
        endcase
    endfunction

    assign o_rd_data_a = override_read(i_rd_addr_a, regs_a  [i_rd_addr_a],
                                       regs_valid[i_rd_addr_a], sp_select);
    assign o_rd_data_b = override_read(i_rd_addr_b, regs_b  [i_rd_addr_b],
                                       regs_valid[i_rd_addr_b], sp_select);
    assign o_dbg_data  = override_read(i_dbg_addr, regs_dbg[i_dbg_addr],
                                       regs_valid[i_dbg_addr], i_supervisor);

    // ── Write logic ─────────────────────────────────────────────
    // Synchronous write to all three DPRAM banks plus (for R14)
    // the active SP flop.  Writes to R0 and R15 are stored in
    // DPRAM but never read out — the override always wins, so
    // skipping the address compare on the write enable saves
    // logic and keeps the write-side wiring symmetric.

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            regs_valid <= 16'h0000;
            usp        <= 32'd0;
            ssp        <= 32'd0;
        end else if (i_wr_en) begin
            // R14 → banked SP flop (in addition to DPRAM, which is dead)
            if (i_wr_addr == REG_SP) begin
                if (sp_select)
                    ssp <= i_wr_data;
                else
                    usp <= i_wr_data;
            end
            // All addresses fan out to the DPRAM banks.  Slots
            // 0/14/15 are dead but writing them costs nothing.
            // Setting valid for those slots is harmless — the
            // override mux ignores the DPRAM read for them.
            regs_valid[i_wr_addr] <= 1'b1;
            regs_a    [i_wr_addr] <= i_wr_data;
            regs_b    [i_wr_addr] <= i_wr_data;
            regs_dbg  [i_wr_addr] <= i_wr_data;
        end
    end

endmodule
