// ULX3S audio bring-up probe — the output stage on the jack, no CPU.
//
// The sim-verified modulator against the board's resistor ladder, with
// the two things simulation cannot settle selectable at runtime: which
// ladder legs a code drives, and what signal drives it. Built via the
// TOP= escape hatch (make fpga TOP=ulx3s_audio_test_top); a probe, not
// a registered machine — no bus, no autoconfig, no register interface.
//
// Configurations (btn[3]/btn[4]), on led[2:0]:
//   0  2b 549/1.1k       0.66 Vpp — line level, no attenuator
//   1  3b 270/549/1.1k   1.53 Vpp
//   2  3b 130/270/549    3.06 Vpp
//   3  4b all legs       3.28 Vpp — 13 dB over line level
//   4  1b 1.1k           0.22 Vpp — linear reference
//   5  1b 130            1.76 Vpp — linear reference
//   6  2b 270/549        1.32 Vpp
//
// The one-bit configurations cannot distort, so what they measure is
// the recording chain's own floor.
//
// Signals (btn[5]/btn[6]), on led[5:3]: silence, two-tone, one tone at
// -6 dBFS, one tone at -42 dBFS, looping PCM clip.
//
// Both jack channels carry the same signal from the same modulators, so
// a left/right difference in a recording is the two ladders' component
// tolerance and nothing else.

// Depth comes from the build (sv2v -D) so the image and the RAM holding
// it cannot disagree. Bounded by timing, not capacity: the ROM reads as
// one wide array, so depth sets how many block RAMs are muxed onto the
// output, and that mux is combinational however rarely it is read.
`ifndef AUDIO_SAMPLES
`define AUDIO_SAMPLES 131072
`endif

module ulx3s_audio_test_top (
    input  logic       clk_25mhz,
    output logic [7:0] led,
    // Only btn[1] and btn[6:3] are used; the rest are physical inputs
    // the design does not consume.
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [6:0] btn,
    /* verilator lint_on UNUSEDSIGNAL */
    output logic       wifi_en,     // LOW = hold ESP32 in reset
    output logic [3:0] audio_l,     // [3] = 130 ohm leg ... [0] = 1.1k
    output logic [3:0] audio_r
);
    import ecp5_pll_pkg::*;

    assign wifi_en = 1'b0;

    // ── Clocks ─────────────────────────────────────────────────
    // The sample rate must not follow a system clock that gets retuned
    // per timing closure, so it comes off its own exactly-divisible
    // chain. ecp5_pll_aux_div is exact-only: an inexact retarget fails
    // elaboration rather than detuning the rate.
    localparam ecp5_pll_cfg_t PLL = ecp5_pll_compute(25_000_000, 100_000_000, 100_000_000);
    // 30 MHz, not the 60 the USB tier uses: the sample ROM's output mux
    // does not close 60 MHz at this depth, and a probe whose converter
    // violates setup measures its own timing failure rather than the
    // ladder. Oversampling is still 625 and 30 MHz divides exactly to
    // 48 kHz. A device carries no such ROM and runs at 60.
    localparam longint AUDIO_HZ   = 30_000_000;
    localparam int     AUDIO_DIV  = ecp5_pll_aux_div(PLL.vco_hz, AUDIO_HZ);
    localparam int     SAMPLE_DIV = int'(AUDIO_HZ) / 48_000;

    logic clk_anchor;   // CLKOP — feedback path, drives the heartbeat only
    logic clk_audio;    // CLKOS3 — 60 MHz modulator clock
    logic pll_lock;

    (* keep *) EHXPLLL #(
        .CLKI_DIV      (PLL.clki_div),
        .CLKFB_DIV     (PLL.clkfb_div),
        .CLKOP_DIV     (PLL.clkop_div),
        .CLKOP_ENABLE  ("ENABLED"),
        .CLKOP_CPHASE  (PLL.clkop_cphase),
        .CLKOP_FPHASE  (0),
        .CLKOS3_DIV    (AUDIO_DIV),
        .CLKOS3_ENABLE ("ENABLED"),
        .CLKOS3_CPHASE (AUDIO_DIV - 1),
        .CLKOS3_FPHASE (0),
        .FEEDBK_PATH   ("CLKOP")
    ) u_pll (
        .CLKI         (clk_25mhz),
        .CLKFB        (clk_anchor),
        .CLKOP        (clk_anchor),
        .CLKOS        (),
        .CLKOS2       (),
        .CLKOS3       (clk_audio),
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
        .ENCLKOS      (1'b0),
        .ENCLKOS2     (1'b0),
        .ENCLKOS3     (1'b1)
    );

    build_check #(
        .COND (PLL.valid),
        .MSG  ("ecp5_pll: no legal PLL config for the audio VCO")
    ) u_check_pll_valid ();

    build_check #(
        .COND (AUDIO_DIV != 0),
        .MSG  ("ecp5_pll: VCO cannot produce AUDIO_HZ exactly")
    ) u_check_audio_div ();

    build_check #(
        .COND (int'(AUDIO_HZ) % 48_000 == 0),
        .MSG  ("audio: AUDIO_HZ does not divide to 48 kHz")
    ) u_check_sample_rate ();

    // ── Reset ──────────────────────────────────────────────────
    // Everything below runs on clk_audio, so there is one domain and
    // the only asynchronous input is the PLL's lock.
    logic lock_sync1, lock_sync2;
    logic btn1_sync1, btn1_sync2;
    logic rst;

    always_ff @(posedge clk_audio) begin
        lock_sync1 <= pll_lock;
        lock_sync2 <= lock_sync1;
        btn1_sync1 <= btn[1];
        btn1_sync2 <= btn1_sync1;
        rst        <= !lock_sync2 || btn1_sync2;
    end

    // ── Buttons ────────────────────────────────────────────────
    // Sampled slower than any contact bounce, so a press is one edge in
    // the sampled sequence and needs no filter.
    logic [19:0] tick_div_q;
    logic        btn_tick;

    assign btn_tick = (tick_div_q == '0);
    always_ff @(posedge clk_audio) tick_div_q <= tick_div_q + 1'b1;

    // Only the four direction keys select anything, so only those are
    // synchronized and edge-detected.
    logic [6:3] btn_sync1, btn_sync2, btn_prev_q;
    logic [6:3] btn_press;

    always_ff @(posedge clk_audio) begin
        btn_sync1 <= btn[6:3];
        btn_sync2 <= btn_sync1;
        if (btn_tick) btn_prev_q <= btn_sync2;
    end

    assign btn_press = (btn_tick ? btn_sync2 : '0) & ~btn_prev_q;

    localparam int CFG_MAX = 6;

    localparam int SIG_MAX = 4;

    logic [2:0] cfg_q;
    logic [2:0] sig_q;

    always_ff @(posedge clk_audio) begin
        if (rst) begin
            cfg_q <= '0;
            sig_q <= '0;
        end else begin
            if (btn_press[3]) cfg_q <= (cfg_q == CFG_MAX[2:0]) ? '0 : cfg_q + 1'b1;
            if (btn_press[4]) cfg_q <= (cfg_q == '0) ? CFG_MAX[2:0] : cfg_q - 1'b1;
            if (btn_press[5]) sig_q <= (sig_q == SIG_MAX[2:0]) ? '0 : sig_q + 1'b1;
            if (btn_press[6]) sig_q <= (sig_q == '0) ? SIG_MAX[2:0] : sig_q - 1'b1;
        end
    end

    // ── Sample rate and stimulus ───────────────────────────────
    logic [10:0] sample_div_q;
    logic        sample_en;

    assign sample_en = (sample_div_q == 11'(SAMPLE_DIV - 1));

    always_ff @(posedge clk_audio) begin
        if (rst || sample_en) sample_div_q <= '0;
        else                  sample_div_q <= sample_div_q + 1'b1;
    end

    logic signed [15:0] tone_sample, pcm_sample;
    logic signed [15:0] sample;

    audio_tone_gen u_tone (
        .i_clk       (clk_audio),
        .i_rst       (rst),
        .i_sample_en (sample_en),
        .i_mode      (sig_q[1:0]),
        .o_sample    (tone_sample)
    );

    audio_sample_rom #(
        .SAMPLES (`AUDIO_SAMPLES)
    ) u_pcm (
        .i_clk       (clk_audio),
        .i_rst       (rst),
        .i_sample_en (sample_en),
        .o_sample    (pcm_sample)
    );

    // Registered before it reaches the modulators: selecting between the
    // ROM's block RAMs is a deep mux, and the modulator's loop is long
    // enough already. The sample rate hides the extra cycle.
    logic signed [15:0] source_q;

    always_ff @(posedge clk_audio) begin
        if (rst) source_q <= '0;
        else     source_q <= (sig_q == 3'd4) ? pcm_sample : tone_sample;
    end

    // Level match for listening: the spans differ by 23 dB, and unmatched
    // a listener hears loudness rather than character. Normalised to the
    // 549/1.1k span; the one configuration quieter than that cannot be
    // raised. Clip only — scaling the tones would move every
    // configuration's operating point and void the recorded figures.
    logic [15:0] level_gain;
    always_comb begin
        case (cfg_q)
            3'd1:    level_gain = 16'd28154;   // 270/549/1.1k, -7.3 dB
            3'd2:    level_gain = 16'd14099;   // 130/270/549,  -13.3 dB
            3'd3:    level_gain = 16'd13151;   // all four,     -14.0 dB
            3'd5:    level_gain = 16'd24679;   // 130,          -8.5 dB
            3'd6:    level_gain = 16'd32886;   // 270/549,      -6.0 dB
            default: level_gain = 16'd65535;   // 549/1.1k and 1.1k
        endcase
    end

    logic signed [32:0] levelled;
    assign levelled = source_q * signed'({1'b0, level_gain});

    always_ff @(posedge clk_audio) begin
        if (rst)                sample <= '0;
        else if (sig_q == 3'd4) sample <= 16'(levelled >>> 16);
        else                    sample <= source_q;
    end

    // ── Modulators ─────────────────────────────────────────────
    // All four run continuously on the same input; the configuration mux
    // selects which reaches the pads, so switching never restarts a loop.
    logic [0:0] code1;
    logic [1:0] code2;
    logic [2:0] code3;
    logic [3:0] code4;

    audio_sigma_delta #(.SAMPLE_W (16), .OUT_W (1)) u_sd1 (
        .i_clk (clk_audio), .i_rst (rst), .i_sample (sample), .o_code (code1));
    audio_sigma_delta #(.SAMPLE_W (16), .OUT_W (2)) u_sd2 (
        .i_clk (clk_audio), .i_rst (rst), .i_sample (sample), .o_code (code2));
    audio_sigma_delta #(.SAMPLE_W (16), .OUT_W (3)) u_sd3 (
        .i_clk (clk_audio), .i_rst (rst), .i_sample (sample), .o_code (code3));
    audio_sigma_delta #(.SAMPLE_W (16), .OUT_W (4)) u_sd4 (
        .i_clk (clk_audio), .i_rst (rst), .i_sample (sample), .o_code (code4));

    // Leg selection is a bit placement. Undriven legs are held low, not
    // floated: floating would make the Thevenin resistance code-dependent,
    // which is itself a nonlinearity.
    logic [3:0] pins;
    always_comb begin
        case (cfg_q)
            3'd0:    pins = {2'b00, code2};   // 549 / 1.1k
            3'd1:    pins = {1'b0,  code3};   // 270 / 549 / 1.1k
            3'd2:    pins = {code3, 1'b0};    // 130 / 270 / 549
            3'd3:    pins = code4;            // all four
            3'd4:    pins = {3'b000, code1};  // 1.1k alone
            3'd5:    pins = {code1, 3'b000};  // 130 alone
            3'd6:    pins = {1'b0, code2, 1'b0};  // 270 / 549
            default: pins = '0;
        endcase
    end

    // Registered at the pads so nextpnr can pack these into the IO
    // cells. The four legs must switch together: skew between them
    // presents the ladder a code that was never commanded, and that
    // glitch energy is code-correlated, which is indistinguishable from
    // nonlinearity. A cycle of latency here is 17 ns.
    logic [3:0] pins_q;

    always_ff @(posedge clk_audio) pins_q <= pins;

    assign audio_l = pins_q;
    assign audio_r = pins_q;

    // ── Status ─────────────────────────────────────────────────
    // A dark heartbeat with lock lit means the aux output failed, not
    // the PLL.
    logic [25:0] beat_q;
    always_ff @(posedge clk_audio) beat_q <= beat_q + 1'b1;

    assign led = {pll_lock, beat_q[25], sig_q, cfg_q};

endmodule
