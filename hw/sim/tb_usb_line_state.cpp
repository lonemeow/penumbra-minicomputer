// Verilator testbench for the Penumbra USB differential line-state decode
//
// Exhaustive: all 2 speeds x 4 (D+,D-) combinations against a reference derived
// straight from the USB spec, so the full- vs low-speed J/K swap is checked
// both ways.

#include <cstdio>
#include <cstdint>
#include "Vusb_line_state.h"

// Mirror usb_pkg::usb_line_e and usb_speed_e.
enum { LINE_SE0 = 0, LINE_J = 1, LINE_K = 2, LINE_SE1 = 3 };
enum { SPEED_FS = 1, SPEED_LS = 2 };   // usb_pkg usb_speed_e (UTMI+ XcvrSelect)

// Reference decode: SE0/SE1 when single-ended; otherwise J/K by speed. At
// full-speed J is D+ high; low-speed swaps so J is D- high.
static int expect(int speed, int dp, int dn) {
    if (dp == dn) return dp ? LINE_SE1 : LINE_SE0;
    bool is_j = (speed == SPEED_LS) ? (dn == 1) : (dp == 1);
    return is_j ? LINE_J : LINE_K;
}

int main() {
    Vusb_line_state* dut = new Vusb_line_state;
    int pass = 0, fail = 0;

    for (int speed : {SPEED_FS, SPEED_LS}) {
        for (int dp = 0; dp <= 1; dp++) {
            for (int dn = 0; dn <= 1; dn++) {
                dut->i_speed = speed;
                dut->i_dp = dp;
                dut->i_dn = dn;
                dut->eval();
                int want = expect(speed, dp, dn);
                if ((int)dut->o_state == want) {
                    pass++;
                } else {
                    printf("FAIL speed=%d dp=%d dn=%d: got %d want %d\n",
                           speed, dp, dn, (int)dut->o_state, want);
                    fail++;
                }
            }
        }
    }

    printf("usb_line_state: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}
