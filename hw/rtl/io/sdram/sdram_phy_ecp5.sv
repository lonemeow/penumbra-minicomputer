// SDRAM PHY for Lattice ECP5.
//
// Pins every controller-driven SDRAM signal through an IOB-resident
// flop, samples the DQ inputs through an IOB-resident flop, and
// forwards the SDRAM clock through an ODDRX1F clock-out cell.  The
// goal is to put a known-bounded I/O-cell delay on every path between
// the FPGA and the SDRAM chip so that placement noise can't shift
// timing — exactly the trap the v1 controller fell into when it routed
// the SDRAM clock combinationally through fabric.
//
// Latency contract with the controller (see sdram_ctrl PHY_*_LATENCY
// parameters):
//   • Output flops on cmd/cke/a/ba/dqm/dq_out/dq_oe add ONE cycle
//     between the controller's o_phy_* register and the SDRAM pin.
//   • Input flop on dq_in adds ONE cycle between the SDRAM pin and
//     i_phy_dq_in observed by the controller.
//
// Step-3 clocking: i_clk_sdram == i_clk (no phase shift).  The PLL
// hands both ports the same 25 MHz signal, and the deterministic IOB
// delay is the only thing keeping cmd/data setup margins.  Step 4
// will swap i_clk_sdram for a phase-shifted (~270°) PLL output so
// the SDRAM samples our drives near the centre of the data window.
//
// (* keep *) on the IOB flops is informational — yosys-ecp5 packs
// flops that drive output ports cleanly into IOBs by topology, but
// the keep stops any aggressive optimisation pass from merging two
// of these flops with fabric-side users.

module sdram_phy_ecp5
#(
    parameter int ROW_BITS = 13,
    parameter int BA_BITS  = 2,
    parameter int DQ_BITS  = 16
)(
    // ── Clocks ─────────────────────────────────────────────
    // i_clk        — controller / IOB flop clock
    // i_clk_sdram  — clock forwarded out the SDRAM clock pin via
    //                ODDRX1F.  In step 3 the board top ties this to
    //                i_clk; in step 4 it becomes a phase-shifted PLL
    //                output so the SDRAM sees a centred data window.
    input  logic                   i_clk,
    input  logic                   i_clk_sdram,
    input  logic                   i_rst,

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

    // ── Clock forwarding ───────────────────────────────────
    // ODDRX1F on D0=1, D1=0 emits a clean copy of i_clk_sdram out
    // of the I/O cell.  The toggling register lives inside the IOB,
    // so the clock-out pin tracks i_clk_sdram with a deterministic
    // delay rather than the fabric routing delay we used to get from
    // `assign o_sdram_clk = i_clk;`.
    (* keep *) ODDRX1F u_clk_oddr (
        .D0   (1'b1),
        .D1   (1'b0),
        .SCLK (i_clk_sdram),
        .RST  (1'b0),
        .Q    (o_sdram_clk)
    );

    // ── Output IOB flops ────────────────────────────────────
    (* iob = "true", keep *) logic [3:0]            cmd_r;
    (* iob = "true", keep *) logic                  cke_r;
    (* iob = "true", keep *) logic [ROW_BITS-1:0]   a_r;
    (* iob = "true", keep *) logic [BA_BITS-1:0]    ba_r;
    (* iob = "true", keep *) logic [DQ_BITS/8-1:0]  dqm_r;
    (* iob = "true", keep *) logic [DQ_BITS-1:0]    dq_out_r;
    (* iob = "true", keep *) logic                  dq_oe_r;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            cmd_r    <= 4'b1111;     // INHIBIT — keeps SDRAM idle during reset
            cke_r    <= 1'b0;
            a_r      <= '0;
            ba_r     <= '0;
            dqm_r    <= '1;
            dq_out_r <= '0;
            dq_oe_r  <= 1'b0;
        end else begin
            cmd_r    <= i_phy_cmd;
            cke_r    <= i_phy_cke;
            a_r      <= i_phy_a;
            ba_r     <= i_phy_ba;
            dqm_r    <= i_phy_dqm;
            dq_out_r <= i_phy_dq_out;
            dq_oe_r  <= i_phy_dq_oe;
        end
    end

    assign {o_sdram_csn, o_sdram_rasn, o_sdram_casn, o_sdram_wen} = cmd_r;
    assign o_sdram_cke = cke_r;
    assign o_sdram_a   = a_r;
    assign o_sdram_ba  = ba_r;
    assign o_sdram_dqm = dqm_r;

    // DQ tristate: yosys-ecp5 maps `oe ? d : 1'bz` to the IOB tristate
    // buffer when the controlling flops drive the IOB cleanly.
    assign io_sdram_d = dq_oe_r ? dq_out_r : {DQ_BITS{1'bz}};

    // ── Input IOB flop ──────────────────────────────────────
    (* iob = "true", keep *) logic [DQ_BITS-1:0] dq_in_r;
    always_ff @(posedge i_clk) dq_in_r <= io_sdram_d;
    assign o_phy_dq_in = dq_in_r;

endmodule
