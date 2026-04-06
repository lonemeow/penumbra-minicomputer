// Penumbra Status Register (SR) — flags, mode bits, ESR latch
//
// The SR holds the CPU's non-architectural state that affects execution:
//   - Condition flags (N, Z, C, V) updated by ALU operations
//   - Supervisor bit (S) for privilege level
//   - Interrupt enable bit (I) for global interrupt mask
//
// Three write sources, in priority order (highest first):
//   1. Exception entry (i_except_entry): atomically sets S=1, I=0,
//      and snapshots SR into ESR *before* the modification.
//   2. Bulk load (i_sr_load): overwrites entire SR from i_wdata
//      (used by ERET and WRSPR SR to restore saved state).
//   3. Flag update (i_flag_w_en): updates only NZCV from ALU outputs,
//      leaving S and I untouched.
//
// The exception SR (ESR) register captures the pre-exception SR value so
// the interrupt micro-routine can push it onto the kernel stack. It is
// readable on the A-bus via a_src=01.
//
// The ei_shadow flip-flop provides a one-instruction delay after EI:
// when set, the fetch unit skips the pending-interrupt check for one
// instruction cycle, then it auto-clears.

module status_reg
    import penumbra_pkg::*;
(
    input  logic        i_clk,
    input  logic        i_rst,

    // ── ALU flag inputs (active when i_flag_w_en=1) ──────────
    input  logic        i_alu_flag_n,   // Negative
    input  logic        i_alu_flag_z,   // Zero
    input  logic        i_alu_flag_c,   // Carry
    input  logic        i_alu_flag_v,   // Overflow

    // ── Micro-word control signals ───────────────────────────
    input  logic        i_flag_w_en,    // Latch NZCV from ALU
    input  logic        i_sr_load,      // Bulk-load SR from i_wdata
    input  logic [31:0] i_wdata,        // W-mux output (for sr_load)

    // ── Exception entry (hardware pre-action) ────────────────
    input  logic        i_except_entry, // Pulse: snapshot SR, then set S=1, I=0

    // ── Software ESR write (WRSPR ESR) ──────────────────────
    input  logic        i_esr_load,     // Pulse: write ESR from i_wdata

    // ── EI/DI control ────────────────────────────────────────
    input  logic        i_ei_set,       // EI instruction: set I=1, arm ei_shadow
    input  logic        i_di_set,       // DI instruction: set I=0
    input  logic        i_ei_shadow_clr,// Fetch unit clears ei_shadow after one insn

    // ── Outputs ──────────────────────────────────────────────
    output logic        o_flag_n,
    output logic        o_flag_z,
    output logic        o_flag_c,
    output logic        o_flag_v,
    output logic        o_sr_s,         // Supervisor bit (1=supervisor)
    output logic        o_sr_i,         // Interrupt enable (1=enabled)
    output logic [31:0] o_sr_read,      // Full SR as 32-bit word (for RDSPR SR)
    output logic [31:0] o_esr,    // Exception SR (ESR) — saved at exception entry
    output logic        o_ei_shadow     // EI delay: suppress next IRQ check
);

    // ── SR bit layout ────────────────────────────────────────
    // Condition flags in low bits [3:0], system bits in high bits [31:30].
    // Bits [29:4] are reserved (read as zero, ignored on write).
    // Bit positions are defined in penumbra_pkg (SR_N, SR_Z, etc.)
    // and shared with the assembler and OS headers.

    // ── Internal state ───────────────────────────────────────
    logic flag_n, flag_z, flag_c, flag_v;
    logic sr_s, sr_i;
    logic [31:0] esr;
    logic ei_shadow;

    // ── Pack SR into 32-bit word ─────────────────────────────
    // Uses the bit positions defined above.
    function automatic logic [31:0] pack_sr;
        input logic n, z, c, v, s, i;
        pack_sr = 32'b0;
        pack_sr[SR_N] = n;
        pack_sr[SR_Z] = z;
        pack_sr[SR_C] = c;
        pack_sr[SR_V] = v;

        pack_sr[SR_I] = i;
        pack_sr[SR_S] = s;
    endfunction

    // ── Output assignments ───────────────────────────────────
    assign o_flag_n    = flag_n;
    assign o_flag_z    = flag_z;
    assign o_flag_c    = flag_c;
    assign o_flag_v    = flag_v;
    assign o_sr_s      = sr_s;
    assign o_sr_i      = sr_i;
    assign o_sr_read   = pack_sr(flag_n, flag_z, flag_c, flag_v, sr_s, sr_i);
    assign o_esr = esr;
    assign o_ei_shadow = ei_shadow;

    // ── Main register update logic ───────────────────────────
    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            // Reset: supervisor mode, interrupts disabled, flags clear
            flag_n    <= 1'b0;
            flag_z    <= 1'b0;
            flag_c    <= 1'b0;
            flag_v    <= 1'b0;
            sr_s      <= 1'b1;  // Boot in supervisor mode
            sr_i      <= 1'b0;  // Interrupts disabled at reset
            esr <= 32'b0;
            ei_shadow <= 1'b0;
        end else begin
            // Priority 1: Exception entry (hardware pre-action)
            if (i_except_entry) begin
                // Snapshot BEFORE modification
                esr <= pack_sr(flag_n, flag_z, flag_c, flag_v, sr_s, sr_i);
                // Mode switch
                sr_s <= 1'b1;
                sr_i <= 1'b0;
            end
            // Priority 1b: Software ESR write (WRSPR ESR)
            else if (i_esr_load) begin
                esr <= i_wdata;
            end
            // Priority 2: Bulk load (ERET / WRSPR SR)
            else if (i_sr_load) begin
                flag_n <= i_wdata[SR_N];
                flag_z <= i_wdata[SR_Z];
                flag_c <= i_wdata[SR_C];
                flag_v <= i_wdata[SR_V];

                sr_i <= i_wdata[SR_I];
                sr_s <= i_wdata[SR_S];
            end
            // Priority 3: Individual updates
            else begin
                // Flag latch from ALU
                if (i_flag_w_en) begin
                    flag_n <= i_alu_flag_n;
                    flag_z <= i_alu_flag_z;
                    flag_c <= i_alu_flag_c;
                    flag_v <= i_alu_flag_v;
                end

                // EI/DI (these come from dedicated micro-ops,
                // never concurrent with flag_w_en in practice,
                // but logically independent — they modify I, not NZCV)
                if (i_ei_set) begin
                    sr_i      <= 1'b1;
                    ei_shadow <= 1'b1;
                end
                if (i_di_set)
                    sr_i <= 1'b0;
            end

            // ei_shadow auto-clear (from fetch unit, independent of above)
            if (i_ei_shadow_clr)
                ei_shadow <= 1'b0;
        end
    end

endmodule
