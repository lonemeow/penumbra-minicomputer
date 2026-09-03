// Audio noise-shaping modulator — wide PCM in, narrow DAC code out.
//
// Second-order, Candy form: two integrators, each with the previous
// output code subtracted from its input, quantizer on the second. STF
// is unity, NTF is (1 - z^-1)^2; removing the out-of-band error is left
// to the analog side.
//
// Integrators rather than an error-feedback network, which assumes the
// quantization error is small against the signal. At OUT_W = 1 one code
// is full scale, that assumption fails outright, and an error-fed loop
// collapses into a two-code limit cycle.
//
// No enable: silence is an input of zero, leaving the loop dithering
// around midscale. Freezing the output would park the DAC on a code.

module audio_sigma_delta #(
    parameter int SAMPLE_W = 16,  // input sample width, signed
    parameter int OUT_W    = 4    // output code width, unsigned
) (
    input  logic                       i_clk,
    input  logic                       i_rst,
    input  logic signed [SAMPLE_W-1:0] i_sample,  // held between sample ticks
    output logic        [OUT_W-1:0]    o_code     // offset binary, to the DAC
);

    // One code spans 2^STEP_SHIFT input units, so quantizing is a shift.
    localparam int STEP_SHIFT = SAMPLE_W - OUT_W;
    localparam int CODE_MAX   = (1 << OUT_W) - 1;
    localparam int HALF_FS    = (CODE_MAX << STEP_SHIFT) / 2;

    // Integrator state, and the wider sum it is computed in. The state
    // clamp is a bit-boundary saturation of that sum, so ACC_W sets the
    // limit — about four times full scale, which the loop does not
    // reach in normal operation.
    localparam int ACC_W  = SAMPLE_W + 2;
    localparam int SUM_W  = SAMPLE_W + 4;
    localparam int QUOT_W = SUM_W - STEP_SHIFT;

    localparam logic signed [SUM_W-1:0] HALF_FS_S = SUM_W'(HALF_FS);

    // Saturation rails. A concatenation is unsigned whatever it holds,
    // so the cast is what makes these signed rather than the bit
    // pattern happening to read correctly.
    localparam logic signed [ACC_W-1:0] ACC_MIN =
        signed'({1'b1, {(ACC_W-1){1'b0}}});
    localparam logic signed [ACC_W-1:0] ACC_MAX =
        signed'({1'b0, {(ACC_W-1){1'b1}}});

    // In range exactly when the bits above the state's sign are copies
    // of it.
    function automatic logic signed [ACC_W-1:0] sat_acc(
        input logic signed [SUM_W-1:0] v);
        logic [SUM_W-ACC_W:0] top;
        top = v[SUM_W-1:ACC_W-1];
        if (&top || ~|top) return ACC_W'(v);
        else return v[SUM_W-1] ? ACC_MIN : ACC_MAX;
    endfunction

    // Reconstructing a code as code*STEP puts the top code one step
    // short of the input's range; 1 - 2^-OUT_W closes that gap exactly.
    // Skipping it costs 0.6 dB at OUT_W = 4 and half the positive swing
    // at OUT_W = 1.
    //
    // Registered because it feeds the loop forward rather than closing
    // it: a cycle of delay here shifts the signal, not the feedback.
    logic signed [SAMPLE_W-1:0] scaled_q;

    always_ff @(posedge i_clk) begin
        if (i_rst) scaled_q <= '0;
        else       scaled_q <= i_sample - (i_sample >>> OUT_W);
    end

    // The loop's y[n-1], as a table rather than a shift and subtract.
    // There are at most sixteen codes, so this is one level of logic
    // where the arithmetic would be a full-width adder inside the loop.
    logic signed [SUM_W-1:0] dac_tab [0:CODE_MAX];
    for (genvar c = 0; c <= CODE_MAX; c++) begin : g_dac
        assign dac_tab[c] = SUM_W'(c * (1 << STEP_SHIFT) - HALF_FS);
    end

    logic signed [SUM_W-1:0] dac;
    assign dac = dac_tab[o_code];

    logic signed [ACC_W-1:0] v1_q, v2_q;
    logic signed [SUM_W-1:0] v1_sum, v2_sum;
    logic signed [ACC_W-1:0] v1_d, v2_d;

    // v2's own terms are both registered, so subtracting the feedback
    // there happens alongside v1's sum rather than behind it, leaving
    // one add between v1_d and v2_sum instead of two.
    logic signed [SUM_W-1:0] v2_base;

    assign v1_sum  = SUM_W'(v1_q) + SUM_W'(scaled_q) - dac;
    assign v1_d    = sat_acc(v1_sum);
    assign v2_base = SUM_W'(v2_q) - dac;
    assign v2_sum  = v2_base + SUM_W'(v1_d);
    assign v2_d    = sat_acc(v2_sum);

    // v2 carries the quantizer's offset in its state rather than having
    // it added here: the offset cancels between v2[n] and v2[n-1], so
    // biasing the reset value alone puts the loop in the DAC's offset
    // binary and takes a full-width adder out of the path.
    logic signed [QUOT_W-1:0] quot;
    assign quot = QUOT_W'(v2_d >>> STEP_SHIFT);

    logic [OUT_W-1:0] code;
    always_comb begin
        if      (quot[QUOT_W-1])        code = '0;
        else if (|quot[QUOT_W-2:OUT_W]) code = '1;
        else                            code = quot[OUT_W-1:0];
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            v1_q   <= '0;
            v2_q   <= ACC_W'(HALF_FS_S);
            // The rest level, exact for every width but OUT_W = 1, where
            // the two codes are the rails and the loop dithers instead.
            o_code <= {1'b1, {(OUT_W-1){1'b0}}};
        end else begin
            v1_q   <= v1_d;
            v2_q   <= v2_d;
            o_code <= code;
        end
    end

endmodule
