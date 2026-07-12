// Verilator testbench for the Penumbra USB host port controller
//
// Drives the raw {D-, D+} line state through attach/bounce/detach/illegal
// sequences and the PORT_CTRL bits through the reset and resume recipes,
// checking the debounced connect/speed view, the transceiver-control
// outputs against the seam's signaling table, ENABLED semantics, and the
// PORT_CHANGE event discipline (exactly one strobe per real transition).
//
// Uses the module's default 600-cycle debounce window: stability shorter
// than the window must never commit, stability past it must.

#include <cstdio>
#include <cstdint>
#include "Vusbhc_port.h"

static const int DB = 600;              // matches DEBOUNCE_CLKS
// Wire states in the FS polarity frame (usb_line_e's convention):
// J = D+ high — an FS device's idle; K = D- high — an LS device's idle.
static const int LINE_SE0 = 0, LINE_J = 1, LINE_K = 2, LINE_SE1 = 3;
static const int SPEED_FS = 1, SPEED_LS = 2;
static const int OPMODE_NORMAL = 0, OPMODE_RAW = 2;

static int g_pass = 0, g_fail = 0;

static void check(const char* name, bool ok) {
    if (ok) {
        g_pass++;
    } else {
        printf("MISMATCH %s\n", name);
        g_fail++;
    }
}

static void check_eq(const char* name, long got, long want) {
    if (got == want) {
        g_pass++;
    } else {
        printf("MISMATCH %s: got %ld want %ld\n", name, got, want);
        g_fail++;
    }
}

static Vusbhc_port* dut;
static int g_changes = 0;               // o_change_evt strobes seen

static void step(int cycles, int line) {
    dut->i_line_state = line;
    for (int i = 0; i < cycles; i++) {
        dut->i_clk = 0;
        dut->eval();
        dut->i_clk = 1;
        dut->eval();
        if (dut->o_change_evt)
            g_changes++;
    }
}

int main() {
    dut = new Vusbhc_port;

    dut->i_rst = 1;
    dut->i_clk = 0;
    dut->i_power = 0;
    dut->i_reset = 0;
    dut->i_suspend = 0;
    dut->i_resume = 0;
    dut->i_line_state = LINE_SE0;
    dut->i_caps = 0x3;                  // {hs=0, fs, ls}
    step(2, LINE_SE0);
    dut->i_rst = 0;

    // Unpowered: even a rock-stable pull-up must not report a device.
    step(3 * DB, LINE_J);
    check("unpowered: no connect", dut->o_connect == 0);
    check_eq("unpowered: no events", g_changes, 0);

    // Powered but nothing attached.
    dut->i_power = 1;
    step(3 * DB, LINE_SE0);
    check("idle: no connect", dut->o_connect == 0);
    check("idle: power reaches the seam", dut->o_port_power == 1);
    check_eq("idle: xcvr parks at FS", dut->o_xcvr_sel, SPEED_FS);
    check_eq("idle: opmode normal", dut->o_opmode, OPMODE_NORMAL);

    // Attach bounce: alternating shorter than the window never commits.
    for (int i = 0; i < 6; i++) {
        step(DB / 3, LINE_J);
        step(DB / 3, LINE_SE0);
    }
    check("bounce: still no connect", dut->o_connect == 0);
    check_eq("bounce: no events", g_changes, 0);

    // A full-speed device settles: D+ pull-up, stable past the window.
    step(DB / 2, LINE_J);
    check("FS attach: not yet (half window)", dut->o_connect == 0);
    step(DB, LINE_J);
    check("FS attach: connected", dut->o_connect == 1);
    check_eq("FS attach: speed", dut->o_speed, SPEED_FS);
    check_eq("FS attach: one event", g_changes, 1);
    check_eq("FS attach: xcvr", dut->o_xcvr_sel, SPEED_FS);
    check("FS attach: not enabled before reset", dut->o_enabled == 0);
    check_eq("FS attach: LINE reports raw", dut->o_line, LINE_J);

    // Packet-shaped traffic: J/K flips and EOP SE0s, every burst far
    // shorter than the window — the debounced view must not move.
    for (int i = 0; i < 40; i++) {
        step(13, LINE_K);
        step(7, LINE_J);
        step(2, LINE_SE0);              // EOP-ish
        step(11, LINE_J);
    }
    check("traffic: connect held", dut->o_connect == 1);
    check_eq("traffic: no events", g_changes, 1);

    // Bus reset: the UTMI+ HS-termination drive state, and 10 ms of SE0
    // on the line must not read as a detach while we drive it.
    dut->i_reset = 1;
    step(10, LINE_SE0);
    check("reset: active", dut->o_reset_active == 1);
    check_eq("reset: xcvr HS code", dut->o_xcvr_sel, 0);
    check("reset: term off", dut->o_term_sel == 0);
    check_eq("reset: opmode raw", dut->o_opmode, OPMODE_RAW);
    check("reset: enabled cleared", dut->o_enabled == 0);
    step(20 * DB, LINE_SE0);            // long SE0, host-driven
    check("reset: connect held through SE0", dut->o_connect == 1);
    dut->i_reset = 0;
    step(5, LINE_J);                    // device idles J again
    check("reset done: enabled", dut->o_enabled == 1);
    check_eq("reset done: one event", g_changes, 2);
    check_eq("reset done: speed retained", dut->o_speed, SPEED_FS);
    step(2 * DB, LINE_J);
    check_eq("reset done: idle J commits nothing", g_changes, 2);

    // Resume: raw K at the port speed, transmit channel held at 00h.
    dut->i_resume = 1;
    step(3 * DB, LINE_K);               // we drive K; detection paused
    check_eq("resume: opmode raw", dut->o_opmode, OPMODE_RAW);
    check_eq("resume: xcvr at port speed", dut->o_xcvr_sel, SPEED_FS);
    check("resume: term on", dut->o_term_sel == 1);
    check("resume: tx override", dut->o_tx_override == 1);
    check_eq("resume: held byte", dut->o_tx_override_data, 0x00);
    check("resume: connect held", dut->o_connect == 1);
    dut->i_resume = 0;
    step(2 * DB, LINE_J);
    check("resume done: still connected", dut->o_connect == 1);
    check_eq("resume done: no stray events", g_changes, 2);

    // Detach: stable SE0 past the window.
    step(DB + DB / 2, LINE_SE0);
    check("detach: disconnected", dut->o_connect == 0);
    check("detach: disabled", dut->o_enabled == 0);
    check_eq("detach: one event", g_changes, 3);

    // A low-speed device: the D- pull-up identifies it.
    step(2 * DB, LINE_K);
    check("LS attach: connected", dut->o_connect == 1);
    check_eq("LS attach: speed", dut->o_speed, SPEED_LS);
    check_eq("LS attach: xcvr", dut->o_xcvr_sel, SPEED_LS);
    check_eq("LS attach: one event", g_changes, 4);

    // A direct polarity swap with no SE0 between (fault-shaped; a real
    // replug always passes through a debounce-length SE0): the view
    // commits as a still-connected speed change, one event.
    step(2 * DB, LINE_J);
    check("polarity swap: still connected", dut->o_connect == 1);
    check_eq("polarity swap: new speed", dut->o_speed, SPEED_FS);
    check_eq("polarity swap: one event", g_changes, 5);

    // Illegal SE1, stable: not a device. From connected it reads as a
    // fault, and the port treats it as a detach.
    step(2 * DB, LINE_SE1);
    check("SE1: disconnects", dut->o_connect == 0);
    check_eq("SE1: one event", g_changes, 6);
    step(2 * DB, LINE_SE1);
    check("SE1: never connects", dut->o_connect == 0);
    check_eq("SE1: no further events", g_changes, 6);

    // Power drop erases the port state.
    step(2 * DB, LINE_K);
    check("repower: LS device back", dut->o_connect == 1);
    dut->i_power = 0;
    step(2 * DB, LINE_K);
    check("unpowered: disconnects despite pull-up", dut->o_connect == 0);

    printf("usbhc_port: %d/%d tests passed\n", g_pass, g_pass + g_fail);
    if (g_fail > 0)
        printf("  *** %d FAILED ***\n", g_fail);

    delete dut;
    return (g_fail > 0) ? 1 : 0;
}
