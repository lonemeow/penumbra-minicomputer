// Penumbra USB protocol constants
//
// Shared by the SIE line-layer cells (oversampling, line-state decode, SYNC/EOP
// framing) and the MAC cells above them (packet formation, the transaction
// FSM). These are USB-protocol-internal, distinct from the programmer-visible
// USB host-controller register contract, which lives in penumbra_pkg.sv
// alongside the other bus-device definitions.

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

    // Packet identifiers, the 4-bit PID codes of USB 2.0. On the wire a PID
    // travels as a byte with the complemented code in the upper nibble
    // ({~pid, pid}); a receiver rejects a byte whose halves disagree. The low
    // two bits encode the packet's group, and therefore its shape:
    //   01 token     — PID + 11-bit field + CRC5
    //   11 data      — PID + payload + CRC16
    //   10 handshake — bare PID
    //   00 special   — PRE/PING/…, outside the CLASS_USBHC minimum
    typedef enum logic [3:0] {
        USB_PID_OUT   = 4'b0001,
        USB_PID_IN    = 4'b1001,
        USB_PID_SOF   = 4'b0101,
        USB_PID_SETUP = 4'b1101,
        USB_PID_DATA0 = 4'b0011,
        USB_PID_DATA1 = 4'b1011,
        USB_PID_ACK   = 4'b0010,
        USB_PID_NAK   = 4'b1010,
        USB_PID_STALL = 4'b1110
    } usb_pid_e;

    // PID-group codes (pid[1:0]). Waived like the oversample factors: an
    // importing cell uses only the groups it decodes.
/* verilator lint_off UNUSEDPARAM */
    localparam logic [1:0] USB_PID_GROUP_TOKEN     = 2'b01;
    localparam logic [1:0] USB_PID_GROUP_DATA      = 2'b11;
    localparam logic [1:0] USB_PID_GROUP_HANDSHAKE = 2'b10;
/* verilator lint_on UNUSEDPARAM */
endpackage
