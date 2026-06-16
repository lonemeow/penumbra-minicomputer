// Verilator testbench for sdram_cdc — clock-domain-crossing bridge.
//
// Two-clock testbench: sys_clk @ 12.5 MHz (80 ns period), sd_clk @
// 100 MHz (10 ns period).  Matches the ULX3S operating point.  The
// C++ side advances both clocks at their respective rates and emulates
// a minimal SDRAM controller on the sd_clk side: ack, wait, done.
//
// Reads return a deterministic value derived from the address
// (addr ^ 0xCAFE0000) so the bridge's response routing can be verified.
//
// Test cases:
//   • Single read round-trip
//   • Single write round-trip (no rsp_data)
//   • Back-to-back sequential transactions
//   • Distinct-address reads (response routing integrity)
//   • Depth-2 in-flight: two requests issued before either completes;
//     fails on a single-outstanding bridge, passes on a depth-2 one
//
// Ends with non-zero exit code on any failure.
//
// Why a unit test for the CDC: the module is only exercised via
// machine_sim integration today, so any CDC bug surfaces as an
// integration failure with a long path to root-cause.  This test
// pins down the bridge's correctness at the unit level so future
// changes (e.g. depth-2 pipelining) can be validated cheaply.

#include <cstdio>
#include <cstdint>
#include "Vsdram_cdc.h"

static int errors = 0;
static int step_count = 0;

// One simulation "step" = 5 ns.  Within that:
//   sd_clk toggles every step (10 ns period → 100 MHz)
//   sys_clk toggles every 8 steps (80 ns period → 12.5 MHz)
static void step(Vsdram_cdc* d) {
    d->i_sd_clk = !d->i_sd_clk;
    if ((step_count % 8) == 0)
        d->i_sys_clk = !d->i_sys_clk;
    d->eval();
    step_count++;
}

// Minimal SDRAM-controller emulation on the sd_clk side.
// On observing o_sd_req_valid, ack after 1 cycle, then drive done
// (with rsp_valid for reads) after a fixed 3-cycle delay.
struct SdController {
    enum { IDLE, ACK, WAIT, DONE } state = IDLE;
    int     counter   = 0;
    bool    was_read  = false;
    uint32_t rsp_value = 0;
    int     last_clk  = 0;

    void tick(Vsdram_cdc* d) {
        // Sample on sd_clk rising edges only.
        if (d->i_sd_clk == 1 && last_clk == 0) {
            d->i_sd_req_ready = 0;
            d->i_sd_rsp_valid = 0;
            d->i_sd_done      = 0;

            switch (state) {
                case IDLE:
                    if (d->o_sd_req_valid) {
                        was_read  = !d->o_sd_req_we;
                        rsp_value = d->o_sd_req_addr ^ 0xCAFE0000u;
                        state = ACK;
                    }
                    break;
                case ACK:
                    d->i_sd_req_ready = 1;
                    counter = 3;
                    state = WAIT;
                    break;
                case WAIT:
                    if (--counter <= 0) state = DONE;
                    break;
                case DONE:
                    d->i_sd_done = 1;
                    if (was_read) {
                        d->i_sd_rsp_valid = 1;
                        d->i_sd_rsp_data  = rsp_value;
                    }
                    state = IDLE;
                    break;
            }
            d->eval();
        }
        last_clk = d->i_sd_clk;
    }
};

static SdController ctrl;

static void step_with_ctrl(Vsdram_cdc* d) {
    step(d);
    ctrl.tick(d);
}

// Step until a sys posedge accepts the currently-driven request.
// o_sys_req_ready is combinational (!sys_full), so the transfer is the
// posedge where valid & ready are both high.  An accept that fills the
// FIFO clears ready on that same edge, so the ready value to test is the
// one present in the low phase *preceding* the posedge (what the flop
// samples) — not the post-edge value.  Returns true on accept, leaving
// the clock just past the accepting posedge; false on timeout.
static bool wait_sys_accept(Vsdram_cdc* d, int max_steps) {
    int  last_sys  = d->i_sys_clk;
    bool ready_pre = false;
    while (max_steps-- > 0) {
        if (d->i_sys_clk == 0) ready_pre = d->o_sys_req_ready;
        step_with_ctrl(d);
        if (d->i_sys_clk == 1 && last_sys == 0 && ready_pre) return true;
        last_sys = d->i_sys_clk;
    }
    return false;
}

static void reset(Vsdram_cdc* d) {
    d->i_sys_clk = 0;
    d->i_sd_clk  = 0;
    d->i_sys_rst = 1;
    d->i_sd_rst  = 1;
    d->i_sys_req_valid   = 0;
    d->i_sys_req_we      = 0;
    d->i_sys_req_addr    = 0;
    d->i_sys_req_wdata   = 0;
    d->i_sys_req_byte_en = 0;
    d->i_sys_rsp_ready   = 1;
    d->i_sd_req_ready    = 0;
    d->i_sd_rsp_valid    = 0;
    d->i_sd_rsp_data     = 0;
    d->i_sd_done         = 0;
    for (int i = 0; i < 64; i++) step_with_ctrl(d);
    d->i_sys_rst = 0;
    d->i_sd_rst  = 0;
    for (int i = 0; i < 16; i++) step_with_ctrl(d);
}

// Issue one transaction on sys side, wait for completion.
// Returns rsp_data for reads.
static uint32_t do_sys_req(Vsdram_cdc* d, uint32_t addr, bool we, uint32_t wdata,
                           uint8_t byte_en, const char* label) {
    d->i_sys_req_valid   = 1;
    d->i_sys_req_we      = we ? 1 : 0;
    d->i_sys_req_addr    = addr;
    d->i_sys_req_wdata   = wdata;
    d->i_sys_req_byte_en = byte_en;

    // Accept is one atomic posedge (combinational ready).  Drop valid
    // right after so the still-free sibling slot isn't fed a duplicate of
    // this same request on the next edge.
    if (!wait_sys_accept(d, 4000)) {
        printf("  FAIL: %s — req_ready never asserted\n", label);
        errors++;
        return 0;
    }
    d->i_sys_req_valid = 0;
    d->eval();

    // Wait for o_sys_done pulse.
    int safety = 8000;
    uint32_t got = 0;
    bool got_done = false;
    int last_sys_clk = d->i_sys_clk;
    while (safety-- > 0) {
        step_with_ctrl(d);
        if (d->i_sys_clk == 1 && last_sys_clk == 0) {
            if (d->o_sys_done) {
                got_done = true;
                if (!we) got = d->o_sys_rsp_data;
                break;
            }
        }
        last_sys_clk = d->i_sys_clk;
    }
    if (!got_done) {
        printf("  FAIL: %s — done never pulsed\n", label);
        errors++;
    }
    return got;
}

static void check(uint32_t got, uint32_t want, const char* label) {
    if (got != want) {
        printf("  FAIL: %s — got 0x%08X want 0x%08X\n", label, got, want);
        errors++;
    } else {
        printf("  PASS: %s — 0x%08X\n", label, got);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Vsdram_cdc* d = new Vsdram_cdc;

    printf("── SDRAM CDC unit test ──\n");

    reset(d);
    printf("  reset complete\n");

    // ── Test 1: single read round-trip ───────────────────────
    {
        uint32_t r = do_sys_req(d, 0x00001000, false, 0, 0xF, "RD 0x1000");
        check(r, 0x00001000u ^ 0xCAFE0000u, "single read");
    }

    // ── Test 2: single write round-trip ──────────────────────
    {
        do_sys_req(d, 0x00002000, true, 0xDEADBEEFu, 0xF, "WR 0x2000");
        printf("  PASS: single write completed\n");
    }

    // ── Test 3: back-to-back sequential reads ────────────────
    {
        bool ok = true;
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t addr = 0x00003000u + (i * 4);
            uint32_t r = do_sys_req(d, addr, false, 0, 0xF, "seq RD");
            uint32_t exp = addr ^ 0xCAFE0000u;
            if (r != exp) {
                printf("  FAIL: seq[%u] addr=0x%08X — got 0x%08X want 0x%08X\n",
                       i, addr, r, exp);
                errors++;
                ok = false;
                break;
            }
        }
        if (ok) printf("  PASS: 8 back-to-back sequential reads\n");
    }

    // ── Test 4: distinct addresses (response-routing integrity) ──
    {
        uint32_t addrs[] = {
            0x00010000u, 0x00020004u, 0x00030008u, 0x0004000Cu,
            0x00050100u, 0x00060200u, 0x00070300u, 0x00080400u,
        };
        bool ok = true;
        for (uint32_t a : addrs) {
            uint32_t r = do_sys_req(d, a, false, 0, 0xF, "RD distinct");
            uint32_t exp = a ^ 0xCAFE0000u;
            if (r != exp) {
                printf("  FAIL: distinct addr=0x%08X — got 0x%08X want 0x%08X\n",
                       a, r, exp);
                errors++;
                ok = false;
                break;
            }
        }
        if (ok) printf("  PASS: 8 distinct-address reads\n");
    }

    // ── Test 5: depth-2 in-flight ────────────────────────────
    // Push two requests back-to-back without waiting for either to
    // complete: A is accepted, then — without dropping valid — the
    // address is switched to B and B is accepted on the very next sys
    // edge (the free second slot).  On a single-outstanding bridge ready
    // would stay low until A's done, so the second accept never appears.
    // Then verify both responses come back in order with correct data —
    // confirms response routing across the slot pointers.
    {
        uint32_t addr_a = 0x00009000u;
        uint32_t addr_b = 0x00009004u;

        // Submit A
        d->i_sys_req_valid   = 1;
        d->i_sys_req_we      = 0;
        d->i_sys_req_addr    = addr_a;
        d->i_sys_req_byte_en = 0xF;

        if (!wait_sys_accept(d, 200)) {
            printf("  FAIL: depth-2 — first request never accepted\n");
            errors++;
        }

        // Switch to B without dropping valid; the second slot accepts it
        // on the next edge while A is still in flight.
        d->i_sys_req_addr = addr_b;
        bool got_b = wait_sys_accept(d, 200);
        if (!got_b) {
            printf("  FAIL: depth-2 — second request not accepted within 25 sys cycles\n");
            printf("        (this is the failure mode of a single-outstanding bridge)\n");
            errors++;
        } else {
            printf("  PASS: depth-2 — both reqs accepted while in flight\n");
        }

        d->i_sys_req_valid = 0;
        d->eval();

        uint32_t r_a = 0, r_b = 0;
        // Wait for first done
        int safety = 8000;
        int last = d->i_sys_clk;
        while (safety-- > 0) {
            step_with_ctrl(d);
            if (d->i_sys_clk == 1 && last == 0 && d->o_sys_done) {
                r_a = d->o_sys_rsp_data;
                break;
            }
            last = d->i_sys_clk;
        }
        // Wait for second done
        safety = 8000;
        last = d->i_sys_clk;
        while (safety-- > 0) {
            step_with_ctrl(d);
            if (d->i_sys_clk == 1 && last == 0 && d->o_sys_done) {
                r_b = d->o_sys_rsp_data;
                break;
            }
            last = d->i_sys_clk;
        }
        check(r_a, addr_a ^ 0xCAFE0000u, "depth-2 A response");
        check(r_b, addr_b ^ 0xCAFE0000u, "depth-2 B response");
    }

    delete d;
    if (errors == 0) {
        printf("── ALL PASS ──\n");
        return 0;
    } else {
        printf("── %d FAILURE(S) ──\n", errors);
        return 1;
    }
}
