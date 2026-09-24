// Nexys Video clocking and power-on reset, shared by every Nexys top
//
// The board's 100 MHz oscillator drives an MMCME2_BASE whose dividers
// xc7_mmcm_pkg derives from CPU_HZ, so the CPU clock is a one-number
// edit and the build stops when no exact divider set reaches it.
//
// o_rst is the machine's synchronous, active-high reset. It holds while
// the MMCM is unlocked or the CPU RESET button is pressed, and once both
// clear it holds for at least the 10 ms hardware reset minimum
// (doc/hardware/bus-protocol.md, Reset Timing). A press or a loss of
// lock restarts the hold, so every bounce of the button restarts it too.
//
// The MMCM's own reset is tied off: the oscillator runs before the FPGA
// configures, and keeping the button out of the MMCM leaves o_locked
// reporting the MMCM alone.

module nexysvideo_clk_rst #(
    parameter longint CPU_HZ = 37_500_000
) (
    input  logic i_clk_100mhz,   // board oscillator
    input  logic i_cpu_resetn,   // CPU RESET button, low while pressed
    output logic o_clk,          // CPU_HZ, on a global clock buffer
    output logic o_locked,       // MMCM lock, asynchronous to o_clk
    output logic o_rst
);
    import xc7_mmcm_pkg::*;

    localparam longint BOARD_CLK_HZ = 100_000_000;
    localparam xc7_mmcm_cfg_t MMCM = xc7_mmcm_compute(BOARD_CLK_HZ, CPU_HZ);

    build_check #(
        .COND (MMCM.valid),
        .MSG  ("xc7_mmcm: no exact MMCM config for CPU_HZ")
    ) u_check_mmcm_valid ();

    logic clkfb;
    logic clk_mmcm;

    // CLKFBOUT feeds CLKFBIN directly: nothing needs CPU_HZ phase-aligned
    // to the oscillator, so the feedback path takes no global buffer.
    /* verilator lint_off PINMISSING */
    MMCME2_BASE #(
        .CLKIN1_PERIOD    (1.0e9 / BOARD_CLK_HZ),
        .DIVCLK_DIVIDE    (MMCM.divclk_divide),
        .CLKFBOUT_MULT_F  (MMCM.mult_x8 / 8.0),
        .CLKOUT0_DIVIDE_F (MMCM.out0_div_x8 / 8.0)
    ) u_mmcm (
        .CLKIN1   (i_clk_100mhz),
        .CLKFBIN  (clkfb),
        .CLKFBOUT (clkfb),
        .CLKOUT0  (clk_mmcm),
        .LOCKED   (o_locked),
        .RST      (1'b0),
        .PWRDWN   (1'b0)
    );
    /* verilator lint_on PINMISSING */

    BUFG u_clk_bufg (
        .I (clk_mmcm),
        .O (o_clk)
    );

    // LOCKED is asynchronous to o_clk and the button to everything; each
    // reaches the o_clk domain through its own two flops, and they combine
    // only after, so no logic sits in front of a synchronizer.
    (* ASYNC_REG = "TRUE" *) logic locked_sync1, locked_sync2;
    (* ASYNC_REG = "TRUE" *) logic resetn_sync1, resetn_sync2;
    always_ff @(posedge o_clk) begin
        locked_sync1 <= o_locked;
        locked_sync2 <= locked_sync1;
        resetn_sync1 <= i_cpu_resetn;
        resetn_sync2 <= resetn_sync1;
    end

    logic hold;
    assign hold = !locked_sync2 || !resetn_sync2;

    // 10 ms of o_clk, rounded up so the hold meets the minimum at any
    // CPU_HZ; the counter runs to the next power of two above it.
    localparam longint RESET_CYCLES     = (CPU_HZ + 99) / 100;
    localparam int     RESET_COUNT_BITS = $clog2(RESET_CYCLES);

    logic [RESET_COUNT_BITS:0] reset_cnt_q;   // top bit set: hold complete

    always_ff @(posedge o_clk) begin
        if (hold)
            reset_cnt_q <= '0;
        else if (!reset_cnt_q[RESET_COUNT_BITS])
            reset_cnt_q <= reset_cnt_q + 1;
    end

    assign o_rst = hold || !reset_cnt_q[RESET_COUNT_BITS];

endmodule
