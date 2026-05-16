// Penumbra Scratch SPR File — SCR0..SCR(N-1)
//
// N 32-bit storage registers exposed to supervisor code via
// RDSPR/WRSPR.  See doc/system/architecture.md ("Scratch SPRs
// (SCR0–SCR3)") for the ISA-level contract.
//
// Hardware contract is intentionally minimal:
//   - i_we=1 with i_wr_sel=k latches i_wdata into SCRk on the
//     rising clock edge
//   - o_rd_data combinationally reflects SCR[i_rd_sel]
//
// SCRn reset values are architecturally undefined; software must
// write before it reads.  This implementation resets to zero
// anyway, matching the rest of the project's SPR-holding modules
// and giving cleaner trace dumps.  The doc still says "undefined"
// because the architectural contract should not over-specify what
// software is allowed to depend on.
//
// Parameterized so the count lives in one place: the ISA fixes
// N=4 (SPR indices 4-7) but the module is correct at any N.
//
// Discrete build mapping: this module is one small board.
// Encoded write port means the boundary needs only a single
// strobe + a 2-bit index + 32-bit data, not 4 per-slot enables —
// less wiring across the backplane.

module spr_scratch
    import penumbra_pkg::*;
#(
    parameter int N = 4
)
(
    input  logic                 i_clk,
    input  logic                 i_rst,

    // ── Write port (encoded) ─────────────────────────────────
    input  logic                 i_we,        // 1 = write this cycle
    input  logic [$clog2(N)-1:0] i_wr_sel,    // Which SCR to write
    input  logic [31:0]          i_wdata,

    // ── Read port (combinational) ────────────────────────────
    input  logic [$clog2(N)-1:0] i_rd_sel,
    output logic [31:0]          o_rd_data
);

    // No `ram_style` attribute: N=4 is below the 16-deep minimum of any
    // ECP5 distributed-RAM primitive, so Yosys correctly infers FFs +
    // a LUT-based read mux.  Forcing "distributed" here makes Yosys
    // fail with "no valid mapping found".
    logic [31:0] scratch [N];

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            for (int k = 0; k < N; k++)
                scratch[k] <= 32'd0;
        end else if (i_we) begin
            scratch[i_wr_sel] <= i_wdata;
        end
    end

    assign o_rd_data = scratch[i_rd_sel];

endmodule
