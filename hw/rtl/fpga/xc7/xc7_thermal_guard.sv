// 7-series thermal guard: the XADC's over-temperature shutdown, armed
//
// Every board top on a 7-series part instantiates this, and the xc7
// flow refuses a bitstream without it (gates.tcl): an XADC watching
// the die temperature, armed to shut the device down at SHUTDOWN_C.
// The shutdown is the XADC's own (UG480, "Thermal Management"): once
// the die passes the threshold, the device disables itself 10 ms later
// and releases every I/O, with no logic of the design involved, so it
// still acts when the design has hung. The bitstream also has to allow
// it (BITSTREAM.CONFIG.OVERTEMPPOWERDOWN, set by build.tcl).
//
// A shut-down device starts up again, from the state it stopped in,
// once the die cools below the release threshold. By then the DRAM has
// gone unrefreshed and the devices around it have been cut off
// mid-transfer, so a system resuming there could write garbage wherever
// it writes. SHUTDOWN_RELEASE_C is set below any temperature the
// board sees, so the device never restarts by itself: it stays off
// until it is reconfigured, which starts a design from reset, or its
// power is removed, as UG480 advises after a thermal shutdown anyway.
//
// A warning below the shutdown comes out as o_warn: high from WARN_C
// until the die cools below WARN_CLEAR_C. A top shows it on an LED; a
// kernel-visible sensor device takes it as an interrupt, and reads the
// temperatures and supply voltages through the XADC's register port
// (DRP), tied off here until that device exists.
//
// The XADC runs off i_clk, the board's free-running oscillator rather
// than any clock derived from it, so the guard keeps measuring whatever
// the design's clocking and reset are doing. o_warn is synchronous to
// that clock.
//
// The defaults are the limits of a commercial-grade part, whose
// junction is specified to 85 C: past that its timing is no longer
// guaranteed, so there is nothing to gain from letting it run hotter.

module xc7_thermal_guard #(
    parameter longint DCLK_HZ            = 100_000_000,
    parameter int     SHUTDOWN_C         = 85,
    parameter int     SHUTDOWN_RELEASE_C = 0,
    parameter int     WARN_C             = 75,
    parameter int     WARN_CLEAR_C       = 70
) (
    input  logic i_clk,     // free-running board oscillator
    output logic o_warn     // die above WARN_C, until below WARN_CLEAR_C
);
    // The XADC's temperature transfer function (UG480, Equation 4-1):
    // the 12-bit ADC code a die temperature in degrees C reads as.
    function automatic logic [11:0] xadc_temp_code(input int celsius);
        // (T + 273.15) × 4096 / 503.975
        longint mk = celsius * 1000 + 273150;
        return 12'(mk * 4096 / 503975);
    endfunction

    // UG480's worked example: 125 C is code CA3h.
    build_check #(
        .COND (xadc_temp_code(125) == 12'hCA3),
        .MSG  ("xc7_thermal_guard: xadc_temp_code(125) is not UG480's CA3h")
    ) u_check_transfer ();

    build_check #(
        .COND (WARN_CLEAR_C < WARN_C && WARN_C < SHUTDOWN_C
               && SHUTDOWN_RELEASE_C < SHUTDOWN_C),
        .MSG  ("xc7_thermal_guard: thresholds out of order")
    ) u_check_order ();

    // ── Configuration register 1 (41h) ──────────────────────────────
    // Independent-ADC sequencer mode keeps ADC A on the on-chip sensors,
    // as default mode does, but with the alarm outputs live; default
    // mode keeps only the over-temperature one. Of the alarms, only the
    // temperature alarm (ALM0) and over-temperature (OT) are enabled:
    // the supply alarms would need thresholds nothing reads yet.
    localparam logic [3:0] SEQ_INDEPENDENT_ADC = 4'b1000;
    localparam logic [3:0] ALM6_TO_3_DISABLE   = 4'b1111;
    localparam logic [3:0] CAL_ALL             = 4'b1111;
    localparam logic [2:0] ALM2_TO_0_DISABLE   = 3'b110;   // ALM0 enabled
    localparam logic       OT_DISABLE          = 1'b0;

    localparam logic [15:0] CONFIG1 = {SEQ_INDEPENDENT_ADC, ALM6_TO_3_DISABLE,
                                       CAL_ALL, ALM2_TO_0_DISABLE, OT_DISABLE};

    // ── Configuration register 2 (42h) ──────────────────────────────
    // ADCCLK is DCLK divided by CD, at most 26 MHz and a division of at
    // least 2; both ADCs stay powered.
    localparam longint ADCCLK_MAX_HZ = 26_000_000;
    localparam longint CD_MIN        = (DCLK_HZ + ADCCLK_MAX_HZ - 1) / ADCCLK_MAX_HZ;
    localparam int     CD            = int'(CD_MIN < 2 ? 2 : CD_MIN);

    build_check #(
        .COND (CD <= 255),
        .MSG  ("xc7_thermal_guard: DCLK_HZ too fast for the ADC clock divider")
    ) u_check_divider ();

    localparam logic [15:0] CONFIG2 = {8'(CD), 8'h00};

    // ── Alarm thresholds (50h-57h) ──────────────────────────────────
    // Codes are MSB-justified. The over-temperature limit's four LSBs
    // are 0011b, which arms the automatic shutdown at this limit in
    // place of the 125 C default.
    localparam logic [3:0]  OT_ARM      = 4'b0011;
    localparam logic [15:0] WARN_UPPER  = {xadc_temp_code(WARN_C), 4'h0};
    localparam logic [15:0] WARN_LOWER  = {xadc_temp_code(WARN_CLEAR_C), 4'h0};
    localparam logic [15:0] OT_LIMIT    = {xadc_temp_code(SHUTDOWN_C), OT_ARM};
    localparam logic [15:0] OT_RELEASE  = {xadc_temp_code(SHUTDOWN_RELEASE_C), 4'h0};

    logic       dclk;
    logic [7:0] alarm;

    BUFG u_dclk_bufg (
        .I (i_clk),
        .O (dclk)
    );

    /* verilator lint_off PINCONNECTEMPTY */
    XADC #(
        .INIT_40 (16'h0000),
        .INIT_41 (CONFIG1),
        .INIT_42 (CONFIG2),
        .INIT_50 (WARN_UPPER),
        .INIT_53 (OT_LIMIT),
        .INIT_54 (WARN_LOWER),
        .INIT_57 (OT_RELEASE)
    ) u_xadc (
        .DCLK         (dclk),
        .RESET        (1'b0),
        .DEN          (1'b0),
        .DWE          (1'b0),
        .DADDR        (7'd0),
        .DI           (16'h0000),
        .DO           (),
        .DRDY         (),
        .CONVST       (1'b0),
        .CONVSTCLK    (1'b0),
        .VP           (1'b0),
        .VN           (1'b0),
        .VAUXP        (16'h0000),
        .VAUXN        (16'h0000),
        .ALM          (alarm),
        .OT           (),
        .BUSY         (),
        .CHANNEL      (),
        .EOC          (),
        .EOS          (),
        .JTAGBUSY     (),
        .JTAGLOCKED   (),
        .JTAGMODIFIED (),
        .MUXADDR      ()
    );
    /* verilator lint_on PINCONNECTEMPTY */

    assign o_warn = alarm[0];

    /* verilator lint_off UNUSEDSIGNAL */
    logic [6:0] alarm_unused;
    assign alarm_unused = alarm[7:1];
    /* verilator lint_on UNUSEDSIGNAL */

endmodule
