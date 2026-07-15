// Penumbra USB receive oversampler / clock recovery (SIE line layer)
//
// USB carries no separate clock; the receiver recovers bit timing by
// oversampling the line at 60 MHz and locking to NRZI edges. This cell runs
// that recovery: it counts oversample clocks per bit (5x full-speed, 40x
// low-speed) and, because bit-stuffing guarantees a transition at least every
// seven bits, re-centers the counter on each line edge so sampling never drifts
// off the bit. At the bit midpoint it strobes o_bit_en with the sampled level
// on o_line, which drive usb_nrzi_decode's i_en / i_line directly.
//
// It is the bottom of the receive chain: line -> [oversample] -> nrzi_decode ->
// bit_unstuff_rx -> deserialize_rx. The input is the resolved J/K symbol level
// (J=1, K=0); resolving the differential pair and SE0 is upstream, and EOP/SE0
// framing is the layer above.

module usb_oversample_rx (
    input  logic       i_clk,
    input  logic       i_rst,
    input  logic [1:0] i_speed,    // usb_speed_e: selects the 5x / 40x divisor
    input  logic       i_line,     // resolved J/K symbol level, synced to i_clk (J=1)
    output logic       o_bit_en,   // one-cycle strobe at the bit midpoint
    output logic       o_line      // line level at the sample point (feeds nrzi_decode)
);
    import usb_pkg::*;

    // Bit period in oversample clocks and the midpoint sample phase, from the
    // port speed. Low-speed is 40x; everything else (full-speed, reserved) 5x.
    logic [5:0] div;
    logic [5:0] sample_phase;
    always_comb begin
        div          = (i_speed == USB_SPEED_LS) ? 6'(USB_OS_LS) : 6'(USB_OS_FS);
        sample_phase = div >> 1;
    end

    logic prev_line_q;   // line level last clock, for edge detection
    logic line_edge;
    assign line_edge = i_line ^ prev_line_q;

    // The port speed is quasi-static but does change live — connect-time
    // detection and the reset drive state's transceiver code both swap it
    // between packets. A change re-locks the counter from scratch so a
    // long low-speed phase can never run off a shrunken bit period.
    logic [1:0] speed_q;
    logic speed_change;
    assign speed_change = (i_speed != speed_q);

    logic [5:0] phase_q, phase_d;   // oversample-clock counter within the bit

    // Single mid-bit sample: the recovered symbol is the line at the strobe
    // cycle. (A majority-vote upgrade would register a vote here instead.)
    assign o_line = i_line;

    always_comb begin
        // Sample once at the bit midpoint. An edge landing on the midpoint means
        // the loop is momentarily a half-bit out of phase, so the sample would
        // read a transition and misalign everything after it -- suppress it and
        // let the re-center below resync. In lock, edges sit at bit boundaries,
        // never the midpoint, so this suppression never fires there.
        o_bit_en = (phase_q == sample_phase) && !line_edge;

        // Free-run: advance one oversample clock, wrapping at the end of the bit.
        if (phase_q == div - 6'd1)
            phase_d = 6'd0;
        else
            phase_d = phase_q + 6'd1;

        // Re-center on a line edge. The edge is detected on the boundary clock,
        // which is bit position 0, so the counter is 1 on the next clock and the
        // midpoint sample then lands sample_phase clocks in -- at the true bit
        // center. Loading 0 here would sample a clock late and, under jitter,
        // fall off the end of a squeezed bit.
        if (line_edge)
            phase_d = 6'd1;

        if (speed_change)
            phase_d = 6'd0;
    end

    always_ff @(posedge i_clk) begin
        if (i_rst) begin
            prev_line_q <= 1'b0;
            phase_q     <= 6'd0;
            speed_q     <= 2'd0;
        end else begin
            prev_line_q <= i_line;
            phase_q     <= phase_d;
            speed_q     <= i_speed;
        end
    end

    // The counter never runs past the current bit period, except on the
    // one cycle a live speed change shrinks the period under it — the
    // re-lock above clears it on the next clock.
    assert property (@(posedge i_clk) disable iff (i_rst)
        (speed_change || phase_q < div))
        else $error("usb_oversample_rx: phase counter exceeded the bit period");
endmodule
