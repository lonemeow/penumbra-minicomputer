// Penumbra USB PHY-internal constants
//
// Shared by the SIE line-layer cells (oversampling, line-state decode, SYNC/EOP
// framing). These are PHY-internal, distinct from the programmer-visible USB
// host-controller register contract, which lives in penumbra_pkg.sv alongside
// the other bus-device definitions.

package usb_pkg;
    // Oversampling factors: the 60 MHz SIE clock per line bit. Full-speed
    // 12 Mbps -> 5x; low-speed 1.5 Mbps -> 40x. The receive sampler counts this
    // many clocks per bit and samples at the midpoint. Waived because line-layer
    // cells that import the package use only the parts they need.
/* verilator lint_off UNUSEDPARAM */
    localparam int USB_OS_FS = 5;
    localparam int USB_OS_LS = 40;
/* verilator lint_on UNUSEDPARAM */

    // Speed / transceiver select, the UTMI+ XcvrSelect encoding — one coding
    // from the seam down through the line-layer cells. HS is the ceiling code:
    // unused by the FS/LS PHYs except as the bus-reset drive state (with
    // TermSelect low, per the seam's signaling recipes).
    typedef enum logic [1:0] {
        USB_SPEED_HS = 2'b00,
        USB_SPEED_FS = 2'b01,
        USB_SPEED_LS = 2'b10
    } usb_speed_e;

    // Differential line states. J and K are the two NRZI symbols (their D+/D-
    // mapping swaps between full- and low-speed); SE0 is both lines low (EOP and
    // bus reset); SE1 (both high) is illegal. The receive sampler recovers bit
    // timing from J<->K transitions; SE0 is the framing layer's concern.
    typedef enum logic [1:0] {
        USB_LINE_SE0 = 2'b00,
        USB_LINE_J   = 2'b01,
        USB_LINE_K   = 2'b10,
        USB_LINE_SE1 = 2'b11
    } usb_line_e;
endpackage
