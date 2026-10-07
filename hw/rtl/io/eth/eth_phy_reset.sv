// Penumbra Ethernet PHY hardware reset
//
// Resets a PHY by its reset pin along with the board, and says when
// software may talk to it. A PHY asks for the pin to be held low for
// some minimum, and then answers no management frame until it has
// settled. Both times are the PHY's, from its datasheet, so the board
// gives them, in milliseconds.
//
// The pin is low from configuration and through the board's reset, and
// for RESET_MS beyond it, counted here: nothing relies on how long the
// board's reset lasts. o_ready rises SETTLE_MS after the pin does. A
// board holds its machine in reset until then, so that software never
// meets a PHY that is not ready. A board reset at any point starts over.
//
// This is for the board's hardware reset alone. Software's bus reset
// runs device discovery again and leaves a device's own state to its
// driver (doc/hardware/bus-protocol.md, "Reset Timing"), and a PHY takes
// far longer to come back than that reset lasts.

module eth_phy_reset #(
    // None of the three has a default: the clock is the board's and the
    // times are its PHY's.
    parameter int unsigned CLK_HZ,      // i_clk's rate
    parameter int unsigned RESET_MS,    // the least the reset pin is held low
    parameter int unsigned SETTLE_MS    // from the pin's release until the PHY answers
)(
    input  logic i_clk,
    input  logic i_rst,         // the board's reset
    output logic o_phy_rst_n,   // the PHY's reset pin: low resets it
    output logic o_ready        // the PHY has been reset and has settled
);

    // Cycles in each wait, rounded up, so that neither falls short at any
    // clock rate. o_ready follows the pin by SETTLE_CYCLES.
    localparam longint unsigned RESET_CYCLES  = (longint'(CLK_HZ) * RESET_MS  + 999) / 1000;
    localparam longint unsigned SETTLE_CYCLES = (longint'(CLK_HZ) * SETTLE_MS + 999) / 1000;
    localparam longint unsigned TOTAL_CYCLES  = RESET_CYCLES + SETTLE_CYCLES;

    // Zero is what a parameter with no default reads as where a tool
    // elaborates a module's own copy (build_check.sv).
    localparam int COUNT_W = (TOTAL_CYCLES > 1) ? $clog2(TOTAL_CYCLES + 1) : 1;

    // Cycles since the board's reset ended, up to TOTAL_CYCLES and no
    // further: that is the most its width is sure to hold.
    logic [COUNT_W-1:0] count_q;

    // The pin's register. The FPGA configures it low, so the PHY is in
    // reset before the clock runs.
    /* verilator lint_off PROCASSINIT */
    logic phy_rst_n_q = 1'b0;
    /* verilator lint_on PROCASSINIT */

    assign o_phy_rst_n = phy_rst_n_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            count_q     <= '0;
            phy_rst_n_q <= 1'b0;
            o_ready     <= 1'b0;
        end else begin
            if (count_q != COUNT_W'(TOTAL_CYCLES))
                count_q <= count_q + COUNT_W'(1);
            if (count_q >= COUNT_W'(RESET_CYCLES))
                phy_rst_n_q <= 1'b1;
            if (count_q == COUNT_W'(TOTAL_CYCLES))
                o_ready <= 1'b1;
        end
    end

endmodule
