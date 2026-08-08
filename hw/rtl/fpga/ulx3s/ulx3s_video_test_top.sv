// ULX3S text-video bring-up probe — the display output path on glass,
// no CPU.
//
// Synthesizes the sim-verified output chain (video_chain_test: timing
// generator, test pattern, TMDS encoders, DDR serializers) plus exactly
// the pieces simulation could not prove — the dedicated video PLL, the
// ODDRX1F output cells, and the GPDI pads. Built via the TOP= escape
// hatch (make fpga TOP=ulx3s_video_test_top); a probe, not a registered
// machine — there is no CPU, bus, or autoconfig here.
//
// Verify: a stable test card on a DVI/HDMI monitor. Triage:
//   no signal          → PLL or clock lane (read led[2:0] first);
//   rolling / tearing  → serializer word alignment;
//   stable, wrong hues → data-lane order at the GPDI pads.
//
// Clocking: VESA mode 0 wants a 25.175 MHz pixel clock, but integer
// EHXPLLL ratios cannot make that from the 25 MHz crystal; 25 MHz even
// (~59 Hz refresh) is within monitor tolerance and the norm on this
// board. The serial clock is 5x the pixel clock off the same VCO —
// the serializer's related-clock contract.

module ulx3s_video_test_top (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    // Only btn[1] (FIRE1 = manual reset) is used; other bits are
    // physical inputs the design doesn't consume.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [6:0] btn,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic       wifi_en,     // LOW = hold ESP32 in reset
    // GPDI pseudo-differential pairs: LVCMOS33D on each _dp pin drives
    // its _dn mate in complement. [0]=blue [1]=green [2]=red [3]=clock,
    // matching video_chain_test's lane order and the board LPF.
    output logic [3:0] gpdi_dp
);
    import ecp5_pll_pkg::*;

    // ── ESP32 disable ──────────────────────────────────────────
    assign wifi_en = 1'b0;

    // ── Video PLL: 25 MHz crystal → serial + pixel off one VCO ──
    // ecp5_pll_compute's two outputs map onto the video roles:
    // CLKOP (the feedback output) carries the TMDS serial clock,
    // CLKOS the pixel clock. Exact-only by construction; for these
    // targets it resolves to {CLKI 1, CLKFB 5, CLKOP 5, CLKOS 25}
    // on a 625 MHz VCO.
    localparam longint SERIAL_HZ = 125_000_000;
    localparam longint PIXEL_HZ  =  25_000_000;
    localparam ecp5_pll_cfg_t PLL = ecp5_pll_compute(25_000_000, SERIAL_HZ, PIXEL_HZ);

    logic clk_serial;   // CLKOP — TMDS bit-pair rate (5x pixel, DDR)
    logic clk_pixel;    // CLKOS — pixel clock
    logic pll_lock;

    (* keep *) EHXPLLL #(
        .CLKI_DIV      (PLL.clki_div),
        .CLKFB_DIV     (PLL.clkfb_div),
        .CLKOP_DIV     (PLL.clkop_div),
        .CLKOP_ENABLE  ("ENABLED"),
        .CLKOP_CPHASE  (PLL.clkop_cphase),   // 0° phase
        .CLKOP_FPHASE  (0),
        .CLKOS_DIV     (PLL.clkos_div),
        .CLKOS_ENABLE  ("ENABLED"),
        .CLKOS_CPHASE  (PLL.clkos_cphase),   // 0° phase
        .CLKOS_FPHASE  (0),
        .FEEDBK_PATH   ("CLKOP")
    ) u_pll (
        .CLKI         (clk_25mhz),
        .CLKFB        (clk_serial),
        .CLKOP        (clk_serial),
        .CLKOS        (clk_pixel),
        .CLKOS2       (),
        .CLKOS3       (),
        .LOCK         (pll_lock),
        .RST          (1'b0),
        .STDBY        (1'b0),
        .PHASESEL0    (1'b0),
        .PHASESEL1    (1'b0),
        .PHASEDIR     (1'b0),
        .PHASESTEP    (1'b0),
        .PHASELOADREG (1'b0),
        .PLLWAKESYNC  (1'b0),
        .ENCLKOP      (1'b1),
        .ENCLKOS      (1'b1),
        .ENCLKOS2     (1'b0),
        .ENCLKOS3     (1'b0)
    );

    // The serializer's clock contract is an exact 5x serial:pixel
    // ratio; this top owns that invariant, so check the divider pair
    // the PLL actually built, not just the requested targets.
    initial begin
        assert (PLL.valid)
            else $fatal(1, "ecp5_pll: no legal PLL config for %0d/%0d Hz video clocks",
                        SERIAL_HZ, PIXEL_HZ);
        assert (PLL.clkos_div == 5 * PLL.clkop_div)
            else $fatal(1, "ecp5_pll: CLKOS_DIV %0d != 5x CLKOP_DIV %0d; serial:pixel ratio broken",
                        PLL.clkos_div, PLL.clkop_div);
    end

    // ── Reset ──────────────────────────────────────────────────
    // Produces rst, the synchronous reset for the whole video chain;
    // both clock domains consume it (the serializer re-locks within
    // five words of release, so release needs no minimum hold beyond
    // PLL lock). Inputs: pll_lock (asynchronous, from the PLL) and
    // btn[1] (FIRE1, raw — the board's manual-reset convention;
    // synchronize before use). Invariant: rst is asserted whenever
    // pll_lock is low — the chain never runs on an unlocked clock.
    logic rst;

    logic btn1_sync1, btn1_sync2;
    always_ff @(posedge clk_25mhz) begin
        btn1_sync1 <= btn[1];
        btn1_sync2 <= btn1_sync1;
    end

    always_ff @(posedge clk_25mhz) begin
        rst <= !pll_lock || btn1_sync2;
    end

    // ── The sim-verified output chain ──────────────────────────
    // Exactly the module the frame-compare testbench passed; this top
    // adds only the clocks, the DDR output cells, and the pads.
    logic [3:0] lane_d0, lane_d1;

    video_chain_test u_chain (
        .i_pclk (clk_pixel),
        .i_sclk (clk_serial),
        .i_rst  (rst),
        .o_d0   (lane_d0),
        .o_d1   (lane_d1)
    );

    // ── GPDI pads: one ODDRX1F per lane ────────────────────────
    // The serializer emits D0 = serial-clock-high half-bit and
    // D1 = low half, matching ODDRX1F's sampling, so the connection
    // is direct. The toggling register lives inside the I/O cell —
    // deterministic pad delay, no fabric routing on the bit stream.
    for (genvar i = 0; i < 4; i++) begin : g_pad
        (* keep *) ODDRX1F u_oddr (
            .D0   (lane_d0[i]),
            .D1   (lane_d1[i]),
            .SCLK (clk_serial),
            .RST  (1'b0),
            .Q    (gpdi_dp[i])
        );
    end

    // ── Triage LEDs ────────────────────────────────────────────
    // led[0] — PLL lock (solid on).
    // led[1] — pixel-domain heartbeat  (~1 Hz blink: clock runs).
    // led[2] — serial-domain heartbeat (~1 Hz blink: clock runs).
    logic [24:0] beat_pixel_q;
    logic [26:0] beat_serial_q;

    always_ff @(posedge clk_pixel)  beat_pixel_q  <= beat_pixel_q + 1'b1;
    always_ff @(posedge clk_serial) beat_serial_q <= beat_serial_q + 1'b1;

    assign led = {5'b0, beat_serial_q[26], beat_pixel_q[24], pll_lock};

endmodule
