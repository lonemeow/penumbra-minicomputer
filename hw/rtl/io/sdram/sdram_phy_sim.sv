// SDRAM PHY for simulation (Verilator).
//
// Pure pass-through: connects the controller's logical PHY ports to
// the SDRAM chip pin signals with no FPGA primitives.  In hardware,
// `sdram_phy_ecp5.sv` (added in step 3) replaces this with IOB-resident
// flops + ODDRX1F clock forwarding.
//
// The controller registers all PHY-side outputs internally.  This
// PHY is therefore a *combinational* wrapper — keeping it simple and
// matching the timing model the controller assumes (one cycle from
// controller-internal flop to SDRAM observation, one cycle from SDRAM
// drive to controller capture, exactly like the hardware path).

module sdram_phy_sim
#(
    parameter int ROW_BITS = 13,
    parameter int BA_BITS  = 2,
    parameter int DQ_BITS  = 16
)(
    input  logic                   i_clk,

    // ── Controller side ─────────────────────────────────
    input  logic [3:0]             i_phy_cmd,         // {csn, rasn, casn, wen}
    input  logic                   i_phy_cke,
    input  logic [ROW_BITS-1:0]    i_phy_a,
    input  logic [BA_BITS-1:0]     i_phy_ba,
    input  logic [DQ_BITS/8-1:0]   i_phy_dqm,
    input  logic [DQ_BITS-1:0]     i_phy_dq_out,
    input  logic                   i_phy_dq_oe,
    output logic [DQ_BITS-1:0]     o_phy_dq_in,

    // ── SDRAM pin side ──────────────────────────────────
    output logic                   o_sdram_clk,
    output logic                   o_sdram_cke,
    output logic                   o_sdram_csn,
    output logic                   o_sdram_rasn,
    output logic                   o_sdram_casn,
    output logic                   o_sdram_wen,
    output logic [ROW_BITS-1:0]    o_sdram_a,
    output logic [BA_BITS-1:0]     o_sdram_ba,
    output logic [DQ_BITS/8-1:0]   o_sdram_dqm,
    inout  wire  [DQ_BITS-1:0]     io_sdram_d
);
    assign {o_sdram_csn, o_sdram_rasn, o_sdram_casn, o_sdram_wen} = i_phy_cmd;
    assign o_sdram_cke = i_phy_cke;
    assign o_sdram_a   = i_phy_a;
    assign o_sdram_ba  = i_phy_ba;
    assign o_sdram_dqm = i_phy_dqm;
    assign o_sdram_clk = i_clk;

    assign io_sdram_d  = i_phy_dq_oe ? i_phy_dq_out : {DQ_BITS{1'bz}};
    assign o_phy_dq_in = io_sdram_d;
endmodule
