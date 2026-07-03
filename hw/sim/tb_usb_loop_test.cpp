// Verilator testbench for the Penumbra USB transmit->receive loopback
//
// The capstone over the whole SIE: bytes fed into the composed transmit chain
// come back out of the composed receive chain over the shared D+/D- wires.
// The receive side was proven against reference waveforms, so recovering the
// bytes here validates the transmit side's on-wire format end to end. Checks
// payload equality, one EOP per packet, and no stuff errors, across both
// speeds, stuffing edge cases, and multi-packet runs.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_loop_test.h"

enum { SPEED_FS = 1, SPEED_LS = 2 };   // usb_pkg usb_speed_e (UTMI+ XcvrSelect)
static int div_of(int speed) { return speed == SPEED_LS ? 40 : 5; }

static void settle(Vusb_loop_test* d) { d->i_clk = 0; d->eval(); }
static void edge(Vusb_loop_test* d)   { d->i_clk = 1; d->eval(); }

struct Res { std::vector<uint8_t> bytes; int eop = 0; bool error = false; bool done = true; };

static Res run(Vusb_loop_test* dut, int speed,
               const std::vector<std::vector<uint8_t>>& packets) {
    dut->i_rst = 1; dut->i_speed = speed;
    dut->i_byte = 0; dut->i_byte_valid = 0;
    settle(dut); edge(dut); settle(dut); edge(dut);
    dut->i_rst = 0;

    Res r;
    for (const auto& pkt : packets) {
        size_t idx = 0;
        int eop_before = r.eop;
        long limit = (long)(pkt.size() * 8 + 48) * div_of(speed) * 4 + 400;
        long gap = 0;
        for (long c = 0; c < limit; c++) {
            bool offer = (idx < pkt.size());
            dut->i_byte       = offer ? pkt[idx] : 0;
            dut->i_byte_valid = offer;
            settle(dut);
            if (dut->o_byte_ready && offer) idx++;
            if (dut->o_byte_valid) r.bytes.push_back(dut->o_byte);
            if (dut->o_eop)        r.eop++;
            if (dut->o_error)      r.error = true;
            edge(dut);
            // After the packet's EOP is seen, idle a few bit times so the
            // next packet starts from a settled bus.
            if (r.eop > eop_before && ++gap >= 6 * div_of(speed)) break;
        }
        if (r.eop == eop_before) { r.done = false; break; }   // timed out
    }
    return r;
}

int main() {
    Vusb_loop_test* dut = new Vusb_loop_test;
    int pass = 0, fail = 0;

    struct Case {
        const char* name;
        int speed;
        std::vector<std::vector<uint8_t>> packets;
    };
    std::vector<Case> cases = {
        {"FS single byte",    SPEED_FS, {{0xC3}}},
        {"FS token-sized",    SPEED_FS, {{0x2D, 0x10, 0xE0}}},
        {"FS stuff-heavy",    SPEED_FS, {{0xFF, 0xFF, 0xFF}}},
        {"FS stuff at start", SPEED_FS, {{0x1F, 0xAA}}},
        {"FS trailing stuff", SPEED_FS, {{0xFC}}},
        {"FS b2b tail-ones",  SPEED_FS, {{0xF8}, {0x33, 0xCC}}},
        {"FS three packets",  SPEED_FS, {{0xE0, 0x07}, {0x81}, {0x7E, 0x55}}},
        {"LS single byte",    SPEED_LS, {{0xC3}}},
        {"LS stuff-heavy",    SPEED_LS, {{0xFF, 0x0F}}},
    };
    uint32_t lcg = 0x100ba11u;
    for (int n = 0; n < 12; n++) {
        Case c{"FS random", SPEED_FS, {}};
        lcg = lcg * 1664525u + 1013904223u;
        int npkt = 1 + ((lcg >> 28) & 1);
        for (int p = 0; p < npkt; p++) {
            lcg = lcg * 1664525u + 1013904223u;
            int len = 1 + (int)((lcg >> 28) & 7);
            std::vector<uint8_t> bytes;
            for (int i = 0; i < len; i++) {
                lcg = lcg * 1664525u + 1013904223u;
                bytes.push_back((uint8_t)(lcg >> 24));
            }
            c.packets.push_back(bytes);
        }
        cases.push_back(c);
    }

    for (const auto& c : cases) {
        std::vector<uint8_t> expected;
        for (const auto& p : c.packets)
            expected.insert(expected.end(), p.begin(), p.end());

        Res r = run(dut, c.speed, c.packets);
        if (r.done && r.bytes == expected && r.eop == (int)c.packets.size() && !r.error) {
            pass++;
        } else {
            printf("FAIL [%s]: done=%d bytes %zu/%zu eop %d/%zu error=%d\n",
                   c.name, r.done, r.bytes.size(), expected.size(),
                   r.eop, c.packets.size(), r.error);
            printf("  want:"); for (uint8_t b : expected) printf(" %02x", b);
            printf("\n  got: "); for (uint8_t b : r.bytes) printf(" %02x", b);
            printf("\n");
            fail++;
        }
    }

    printf("usb_loop_test: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}
