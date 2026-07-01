// Penumbra USB differential line-state decode (SIE line layer)
//
// Resolves the raw D+/D- receiver pair into the four USB line states. Both
// lines low is SE0 (EOP and bus reset), both high is the illegal SE1, and a
// differential pair is J or K. Which differential polarity is J depends on
// speed -- full- and low-speed swap the D+/D- assignment -- so i_speed selects
// the mapping. This is the bottom of the receive chain: it feeds the
// oversampler its J/K symbol (o_state == USB_LINE_J) and the framing layer its
// SE0. Purely combinational.

module usb_line_state (
    input  logic [1:0] i_speed,   // usb_speed_e: FS/LS select the J/K polarity
    input  logic       i_dp,      // D+ receiver
    input  logic       i_dn,      // D- receiver
    output logic [1:0] o_state    // usb_line_e: SE0 / J / K / SE1
);
    import usb_pkg::*;

    // In a differential pair, the line level that means J. Full-speed J is D+
    // high; low-speed swaps the pair, so J is D- high. (Meaningful only when the
    // pair is differential; ignored in the single-ended branch below.)
    logic diff_is_j;
    assign diff_is_j = (i_speed == USB_SPEED_LS) ? i_dn : i_dp;

    always_comb begin
        if (i_dp == i_dn)
            // Both lines at the same level: single-ended, not a data symbol.
            o_state = i_dp ? USB_LINE_SE1 : USB_LINE_SE0;
        else
            o_state = diff_is_j ? USB_LINE_J : USB_LINE_K;
    end
endmodule
