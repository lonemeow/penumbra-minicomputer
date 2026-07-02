// Verilator testbench for the Penumbra USB receive framing FSM
//
// Drives scripted packets -- idle, SOP, a SYNC field (N zeros then a 1), a
// payload bit stream, then EOP -- and checks the control outputs: SYNC-done
// fires exactly once (on the terminating 1), the payload bits are routed out
// via o_payload_en in order, and EOP is seen. Includes a hub-stripped SYNC
// (few leading 0s) to confirm the end-marker detection survives it, and a
// back-to-back pair to confirm the FSM re-arms.

#include <cstdio>
#include <cstdint>
#include <vector>
#include "Vusb_rx_framing.h"

// Mirror usb_pkg::usb_line_e.
enum { LINE_SE0 = 0, LINE_J = 1, LINE_K = 2 };

struct Step { int line; int en; int data; };

static void settle(Vusb_rx_framing* d) { d->i_clk = 0; d->eval(); }
static void edge(Vusb_rx_framing* d)   { d->i_clk = 1; d->eval(); }

static void reset(Vusb_rx_framing* d) {
    d->i_rst = 1; d->i_line_state = LINE_J; d->i_bit_en = 0; d->i_data_bit = 0;
    settle(d); edge(d); settle(d); edge(d);
    d->i_rst = 0;
}

struct Res { int sync_done = 0; bool eop = false; bool active = false; std::vector<int> payload; };

// Build the cycle script for one packet: idle, SOP, SYNC (zeros then a 1, each
// bit as an i_bit_en pulse followed by an idle gap), payload (same pacing), EOP.
static std::vector<Step> packet_script(int sync_zeros, const std::vector<int>& payload) {
    std::vector<Step> s;
    for (int i = 0; i < 3; i++) s.push_back({LINE_J, 0, 0});    // idle
    s.push_back({LINE_K, 0, 0});                                // SOP: first K
    for (int i = 0; i < sync_zeros; i++) {                      // SYNC 0s
        s.push_back({LINE_K, 1, 0});
        s.push_back({LINE_K, 0, 0});
    }
    s.push_back({LINE_K, 1, 1});                                // SYNC terminating 1
    s.push_back({LINE_K, 0, 0});
    for (int b : payload) {                                     // payload bits
        s.push_back({LINE_K, 1, b});
        s.push_back({LINE_K, 0, 0});
    }
    s.push_back({LINE_SE0, 0, 0});                             // EOP
    s.push_back({LINE_SE0, 0, 0});
    for (int i = 0; i < 2; i++) s.push_back({LINE_J, 0, 0});    // back to idle
    return s;
}

static Res run(Vusb_rx_framing* d, const std::vector<Step>& script) {
    Res r;
    for (const Step& st : script) {
        d->i_line_state = st.line;
        d->i_bit_en     = st.en;
        d->i_data_bit   = st.data;
        settle(d);
        if (d->o_sync_done)  r.sync_done++;
        if (d->o_payload_en) r.payload.push_back(d->i_data_bit);
        if (d->o_eop)        r.eop = true;
        if (d->o_active)     r.active = true;
        edge(d);
    }
    return r;
}

static bool check(const char* name, const Res& r, const std::vector<int>& payload) {
    bool ok = (r.sync_done == 1) && r.eop && r.active && (r.payload == payload);
    if (!ok)
        printf("FAIL [%s]: sync_done=%d eop=%d active=%d payload %zu/%zu\n",
               name, r.sync_done, r.eop, r.active, r.payload.size(), payload.size());
    return ok;
}

int main() {
    Vusb_rx_framing* dut = new Vusb_rx_framing;
    int pass = 0, fail = 0;

    struct Case { const char* name; int zeros; std::vector<int> payload; };
    std::vector<Case> cases = {
        {"full SYNC",       7, {1, 0, 1, 1, 0, 0, 1, 0}},
        {"hub-stripped x1", 1, {0, 1, 1, 0, 1, 0, 0, 1}},
        {"hub-stripped x3", 3, {1, 1, 1, 1, 0, 0, 0, 0, 1, 0}},
        {"long payload",    7, std::vector<int>(32, 1)},
        {"min payload",     5, {1}},
    };

    for (const auto& c : cases) {
        reset(dut);
        Res r = run(dut, packet_script(c.zeros, c.payload));
        (check(c.name, r, c.payload) ? pass : fail)++;
    }

    // Back-to-back: two packets with no reset between them -- the FSM must
    // return to idle after EOP and frame the second cleanly.
    {
        reset(dut);
        std::vector<int> pa = {1, 0, 0, 1, 1, 0, 1, 0};
        std::vector<int> pb = {0, 1, 1, 0, 0, 1, 0, 1};
        Res ra = run(dut, packet_script(7, pa));
        Res rb = run(dut, packet_script(4, pb));
        (check("b2b first", ra, pa) ? pass : fail)++;
        (check("b2b second", rb, pb) ? pass : fail)++;
    }

    printf("usb_rx_framing: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0) printf("  *** %d FAILED ***\n", fail);
    delete dut;
    return (fail > 0) ? 1 : 0;
}
