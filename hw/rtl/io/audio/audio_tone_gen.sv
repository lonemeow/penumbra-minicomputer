// Test-signal source for the audio output stage.
//
// Emits one of four stimuli at the sample rate, selected by i_mode:
//
//   0  silence          — noise floor and idle behaviour
//   1  9000 + 10125 Hz  — two tones, each -12 dBFS
//   2  9000 Hz          — one tone, -6 dBFS
//   3  9000 Hz          — one tone, -42 dBFS
//
// Mode 1 is the measurement stimulus: two tones 1125 Hz apart place a
// difference product there that exists only through nonlinearity, which
// is far easier to see than harmonic distortion on one tone. Mode 2
// sets recording levels; mode 3 exercises the codes a converter idles
// around.
//
// Both increments are whole numbers of the 128-step phase, so the
// accumulators never truncate and the tones are spectrally clean.

module audio_tone_gen (
    input  logic               i_clk,
    input  logic               i_rst,
    input  logic               i_sample_en,  // one cycle at the sample rate
    input  logic        [1:0]  i_mode,
    output logic signed [15:0] o_sample
);

    // steps per sample: 24/128 and 27/128 of the sample rate
    localparam logic [6:0] INC_1 = 7'd24;
    localparam logic [6:0] INC_2 = 7'd27;

    logic [6:0] phase1_q, phase2_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            phase1_q <= '0;
            phase2_q <= '0;
        end else if (i_sample_en) begin
            phase1_q <= phase1_q + INC_1;
            phase2_q <= phase2_q + INC_2;
        end
    end

    logic signed [15:0] tone1, tone2;

    audio_sine_rom u_rom1 (
        .i_phase  (phase1_q),
        .o_sample (tone1)
    );

    audio_sine_rom u_rom2 (
        .i_phase  (phase2_q),
        .o_sample (tone2)
    );

    // Attenuations are shifts, so the levels are 6.02 dB steps: the
    // pair sums to -6.02 dBFS and mode 3 lands at -42.1.
    logic signed [15:0] mixed;
    always_comb begin
        case (i_mode)
            2'd1:    mixed = (tone1 >>> 2) + (tone2 >>> 2);
            2'd2:    mixed = tone1 >>> 1;
            2'd3:    mixed = tone1 >>> 7;
            default: mixed = '0;
        endcase
    end

    always_ff @(posedge i_clk) begin
        if (i_rst)            o_sample <= '0;
        else if (i_sample_en) o_sample <= mixed;
    end

endmodule
