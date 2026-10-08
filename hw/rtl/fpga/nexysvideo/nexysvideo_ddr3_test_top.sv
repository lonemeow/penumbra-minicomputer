// Nexys Video DDR3 test top: the board's DDR3 at the machine's configuration
//
// The DDR3 controller alone, in the configuration the machine will use
// (both byte lanes, all 15 row bits, byte-masked writes), calibrating
// and running its full self-test (BIST mode 2) over the whole chip, again
// and again, with a line per run on the console at 115200 8N1. No CPU or
// bus: a failure here is the memory, its clocks or its configuration.
// Built with the TOP= escape hatch (make fpga TOP=nexysvideo_ddr3_test_top).
//
// The LEDs:
//   led[0]  DDR3 clocks locked
//   led[1]  calibration and self-test complete
//   led[2]  heartbeat: 1 Hz off the controller clock
//   led[3]  a run in progress
//   led[4]  the last finished run passed
//   led[5]  some run has seen a self-test mismatch since reset
//   led[7]  the die above the thermal guard's warning threshold
// CPU RESET restarts the sequence from the banner.

module nexysvideo_ddr3_test_top (
    input  logic        clk,          // 100 MHz oscillator
    input  logic        cpu_resetn,   // CPU RESET button, low while pressed
    output logic [7:0]  led,
    output logic        uart_rx_out,  // FPGA TX, to the host's RX

    // ── DDR3 (MT41K256M16, x16) ───────────────────────────
    output logic [0:0]  ddr3_ck_p,
    output logic [0:0]  ddr3_ck_n,
    output logic        ddr3_reset_n,
    output logic [0:0]  ddr3_cke,
    output logic        ddr3_ras_n,
    output logic        ddr3_cas_n,
    output logic        ddr3_we_n,
    output logic [14:0] ddr3_addr,
    output logic [2:0]  ddr3_ba,
    inout  wire  [15:0] ddr3_dq,
    inout  wire  [1:0]  ddr3_dqs_p,
    inout  wire  [1:0]  ddr3_dqs_n,
    output logic [1:0]  ddr3_dm,
    output logic [0:0]  ddr3_odt
);
    localparam longint CTRL_HZ   = 83_333_333;
    localparam int     BIST_MODE = 2;

    logic ctrl_clk, ddr3_clk, ddr3_clk_90, ref_clk;
    logic locked;

    nexysvideo_ddr3_clk u_ddr3_clk (
        .i_clk_100mhz  (clk),
        .o_ctrl_clk    (ctrl_clk),
        .o_ddr3_clk    (ddr3_clk),
        .o_ddr3_clk_90 (ddr3_clk_90),
        .o_ref_clk     (ref_clk),
        .o_locked      (locked)
    );

    // Over-temperature shutdown, off the oscillator itself.
    logic thermal_warn;

    xc7_thermal_guard u_thermal (
        .i_clk  (clk),
        .o_warn (thermal_warn)
    );

    // ── Reset ───────────────────────────────────────────────
    // The button and the PLL's lock are asynchronous to the controller
    // clock; either restarts the sequence.
    (* ASYNC_REG = "TRUE" *) logic locked_sync1, locked_sync2;
    (* ASYNC_REG = "TRUE" *) logic resetn_sync1, resetn_sync2;

    always_ff @(posedge ctrl_clk) begin
        locked_sync1 <= locked;
        locked_sync2 <= locked_sync1;
        resetn_sync1 <= cpu_resetn;
        resetn_sync2 <= resetn_sync1;
    end

    logic rst;
    assign rst = !locked_sync2 || !resetn_sync2;

    // ── DDR3 controller and its self-test ───────────────────
    logic        ctrl_rst_n;
    logic        calib_complete;
    logic        dbg_stb, dbg_stall, dbg_ack;
    logic [6:0]  dbg_addr;
    logic [31:0] dbg_data;

    /* verilator lint_off PINCONNECTEMPTY */
    nexysvideo_ddr3 #(
        .BIST_MODE (BIST_MODE)
    ) u_ddr3 (
        .i_ctrl_clk       (ctrl_clk),
        .i_ddr3_clk       (ddr3_clk),
        .i_ddr3_clk_90    (ddr3_clk_90),
        .i_ref_clk        (ref_clk),
        .i_rst_n          (ctrl_rst_n),

        .i_stb            (1'b0),
        .i_we             (1'b0),
        .i_addr           ('0),
        .i_data           ('0),
        .i_sel            ('0),
        .i_aux            ('0),
        .o_stall          (),
        .o_ack            (),
        .o_data           (),
        .o_aux            (),

        .i_dbg_stb        (dbg_stb),
        .i_dbg_we         (1'b0),
        .i_dbg_addr       (dbg_addr),
        .i_dbg_data       (32'h0),
        .o_dbg_stall      (dbg_stall),
        .o_dbg_ack        (dbg_ack),
        .o_dbg_data       (dbg_data),

        .o_calib_complete (calib_complete),
        .o_calib_state    (),

        .o_ddr3_ck_p      (ddr3_ck_p[0]),
        .o_ddr3_ck_n      (ddr3_ck_n[0]),
        .o_ddr3_reset_n   (ddr3_reset_n),
        .o_ddr3_cke       (ddr3_cke[0]),
        .o_ddr3_ras_n     (ddr3_ras_n),
        .o_ddr3_cas_n     (ddr3_cas_n),
        .o_ddr3_we_n      (ddr3_we_n),
        .o_ddr3_addr      (ddr3_addr),
        .o_ddr3_ba        (ddr3_ba),
        .io_ddr3_dq       (ddr3_dq),
        .io_ddr3_dqs_p    (ddr3_dqs_p),
        .io_ddr3_dqs_n    (ddr3_dqs_n),
        .o_ddr3_dm        (ddr3_dm),
        .o_ddr3_odt       (ddr3_odt[0])
    );
    /* verilator lint_on PINCONNECTEMPTY */

    logic testing, last_pass, mismatch_seen;

    nexysvideo_ddr3_selftest #(
        .CLK_HZ    (CTRL_HZ),
        .BIST_MODE (BIST_MODE)
    ) u_selftest (
        .i_clk            (ctrl_clk),
        .i_rst            (rst),
        .o_ctrl_rst_n     (ctrl_rst_n),
        .i_calib_complete (calib_complete),
        .o_dbg_stb        (dbg_stb),
        .o_dbg_addr       (dbg_addr),
        .i_dbg_stall      (dbg_stall),
        .i_dbg_ack        (dbg_ack),
        .i_dbg_data       (dbg_data),
        .o_tx             (uart_rx_out),
        .o_testing        (testing),
        .o_last_pass      (last_pass),
        .o_mismatch_seen  (mismatch_seen)
    );

    // ── Heartbeat ───────────────────────────────────────────
    localparam int                BEAT_HALF = int'(CTRL_HZ / 2);
    localparam int                BEAT_W    = $clog2(BEAT_HALF);
    localparam logic [BEAT_W-1:0] BEAT_LAST = BEAT_W'(BEAT_HALF - 1);

    logic [BEAT_W-1:0] beat_cnt_q;
    logic              beat_q;

    always_ff @(posedge ctrl_clk) begin
        if (beat_cnt_q == BEAT_LAST) begin
            beat_cnt_q <= '0;
            beat_q     <= !beat_q;
        end else begin
            beat_cnt_q <= beat_cnt_q + BEAT_W'(1);
        end
    end

    assign led = {thermal_warn, 1'b0, mismatch_seen, last_pass, testing,
                  beat_q, calib_complete, locked};

endmodule
