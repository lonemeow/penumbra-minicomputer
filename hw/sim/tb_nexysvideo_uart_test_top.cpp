// tb_nexysvideo_uart_test_top — the Nexys Video UART test top, driven
// and watched at its board pins.
//
// The MMCM lint stub passes the oscillator straight through, so one
// testbench clock is one CPU clock. The host side of the serial line
// runs at exactly 115200 baud, as the FT232R does, so a top whose bit
// timing drifts fails here the way it would fail on the board.
//
// Covers:
//  - power-on reset: held while CPU RESET is pressed and for at least
//    10 ms of CPU clock after release (doc/hardware/bus-protocol.md,
//    Reset Timing); a press mid-run reasserts it within the synchronizer
//    delay, and a bounce during the hold restarts the full hold;
//  - TX: high from configuration, before any clock edge; the banner
//    after every reset, 8N1, every line transition within 1% of where a
//    115200-baud receiver expects it;
//  - RX: a byte sent to the board is echoed and toggles led[3]; a frame
//    with a low stop bit sets led[4], is not echoed, and reset clears it;
//  - heartbeat: led[2] toggles every CPU_HZ / 2 cycles, through reset.
#include "Vnexysvideo_uart_test_top.h"
#include "uart_host.h"
#include "verilated.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The top's CPU_HZ and BAUD, and the reset minimum from the bus spec.
static const uint64_t CPU_HZ = 37500000;
static const uint64_t BAUD = 115200;
static const uint64_t RESET_MIN_CYCLES = CPU_HZ / 100;          // 10 ms
static const double BIT = double(CPU_HZ) / double(BAUD);        // cycles

static const std::string BANNER = "\r\nPenumbra Nexys Video UART test\r\n";

enum { LED_LOCKED = 0, LED_RUNNING = 1, LED_BEAT = 2, LED_RX = 3,
       LED_FRAME_ERR = 4 };

static Vnexysvideo_uart_test_top* top;
static uint64_t cycle = 0;
static int checks = 0, fails = 0;

static void chk(bool ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL at cycle %llu: %s\n", (unsigned long long)cycle, what);
    }
}

static bool led(int n) { return (top->led >> n) & 1; }

static UartHostRx host(BIT);
static UartHostTx host_tx(BIT);
static std::vector<uint64_t> beat_toggles;
static bool last_beat = false;

static void tick() {
    top->uart_tx_in = host_tx.line(cycle);
    top->clk = 0;
    top->eval();
    top->clk = 1;
    top->eval();
    cycle++;
    host.step(top->uart_rx_out, cycle);
    bool beat = led(LED_BEAT);
    if (beat != last_beat)
        beat_toggles.push_back(cycle);
    last_beat = beat;
}

static void run(uint64_t n) {
    for (uint64_t i = 0; i < n; i++)
        tick();
}

// Run until led[1] shows reset released; returns the cycles taken, or
// ~0 on timeout.
static uint64_t cycles_to_release(uint64_t limit) {
    uint64_t t0 = cycle;
    while (!led(LED_RUNNING)) {
        if (cycle - t0 >= limit)
            return ~0ULL;
        tick();
    }
    return cycle - t0;
}

// Run until the TX line has been idle for two frame times, counting from
// the call at the earliest, so output that is about to start is waited
// for rather than missed.
static void wait_tx_quiet() {
    uint64_t quiet = (uint64_t)(20 * BIT);
    uint64_t t0 = cycle;
    while (host.in_frame || cycle - std::max(t0, host.last_activity) < quiet)
        tick();
}

// Send one frame into uart_tx_in and run until it is out.
static void host_send(uint8_t byte, bool stop_bit = true) {
    host_tx.send(byte, stop_bit);
    while (host_tx.busy())
        tick();
}

static void check_release(const char* after) {
    uint64_t n = cycles_to_release(10 * RESET_MIN_CYCLES);
    char msg[160];
    if (n == ~0ULL) {
        snprintf(msg, sizeof msg,
                 "reset still held 100 ms after %s", after);
        chk(false, msg);
        return;
    }
    printf("  reset released %llu cycles (%.2f ms) after %s\n",
           (unsigned long long)n, n * 1000.0 / CPU_HZ, after);
    snprintf(msg, sizeof msg,
             "reset released %llu cycles after %s; the minimum is %llu (10 ms)",
             (unsigned long long)n, after, (unsigned long long)RESET_MIN_CYCLES);
    chk(n >= RESET_MIN_CYCLES, msg);
}

static void check_banner(size_t from, const char* when) {
    std::string got = host.text.substr(from);
    char msg[160];
    snprintf(msg, sizeof msg, "banner %s: got %zu bytes, expected %zu",
             when, got.size(), BANNER.size());
    chk(got == BANNER, msg);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    top = new Vnexysvideo_uart_test_top;
    top->cpu_resetn = 0;
    top->uart_tx_in = 1;

    // ── Configuration: the line idles before any clock edge ────────
    top->clk = 0;
    top->eval();
    chk(top->uart_rx_out, "TX line low at configuration, before the clock runs");

    // ── Power-on with CPU RESET pressed ─────────────────────────────
    bool held = true, tx_idle = true;
    for (int i = 0; i < 1000; i++) {
        tick();
        held &= !led(LED_RUNNING);
        tx_idle &= top->uart_rx_out;
    }
    chk(led(LED_LOCKED), "led[0] (MMCM locked) is off");
    chk(held, "reset released while CPU RESET is pressed");
    chk(tx_idle, "TX line left idle while in reset");

    // ── Release: the 10 ms hold, then the banner ────────────────────
    top->cpu_resetn = 1;
    check_release("CPU RESET was released");
    chk(host.text.empty() && host.frame_errors == 0,
        "TX line active during the reset hold");
    wait_tx_quiet();
    check_banner(0, "after power-on");

    // ── Echo ───────────────────────────────────────────────────────
    size_t mark = host.text.size();
    bool rx_led = led(LED_RX);
    host_send('A');
    wait_tx_quiet();
    chk(host.text.substr(mark) == "A", "'A' was not echoed");
    chk(led(LED_RX) != rx_led, "led[3] did not toggle on a received byte");

    mark = host.text.size();
    for (char c : std::string("hello"))
        host_send((uint8_t)c);
    wait_tx_quiet();
    chk(host.text.substr(mark) == "hello",
        "back-to-back bytes were not all echoed");
    chk(!led(LED_FRAME_ERR), "led[4] (framing error) set by good frames");

    // ── A low stop bit ─────────────────────────────────────────────
    mark = host.text.size();
    host_send('x', false);
    run((uint64_t)(2 * BIT));      // line back high, as after a break
    wait_tx_quiet();
    chk(led(LED_FRAME_ERR), "led[4] not set by a frame with a low stop bit");
    chk(host.text.substr(mark).empty(), "a frame with a low stop bit was echoed");

    // ── A press mid-run ────────────────────────────────────────────
    top->cpu_resetn = 0;
    uint64_t t0 = cycle;
    while (led(LED_RUNNING) && cycle - t0 < 100)
        tick();
    char msg[160];
    snprintf(msg, sizeof msg,
             "reset took %llu cycles to assert after a press; at most 4 expected",
             (unsigned long long)(cycle - t0));
    chk(cycle - t0 <= 4, msg);
    run(1000);
    chk(!led(LED_FRAME_ERR), "reset did not clear led[4]");

    // ── A bounce during the hold restarts it ────────────────────────
    top->cpu_resetn = 1;
    run(RESET_MIN_CYCLES / 2);
    chk(!led(LED_RUNNING), "reset released halfway through the hold");
    top->cpu_resetn = 0;
    run(4);
    top->cpu_resetn = 1;
    mark = host.text.size();
    check_release("a 4-cycle bounce halfway through the hold");
    wait_tx_quiet();
    check_banner(mark, "after a press");

    // ── Heartbeat ──────────────────────────────────────────────────
    while (beat_toggles.size() < 2)
        tick();
    for (size_t i = 1; i < beat_toggles.size(); i++) {
        uint64_t period = beat_toggles[i] - beat_toggles[i - 1];
        snprintf(msg, sizeof msg,
                 "heartbeat toggled after %llu cycles; expected CPU_HZ / 2 = %llu",
                 (unsigned long long)period, (unsigned long long)(CPU_HZ / 2));
        chk(period == CPU_HZ / 2, msg);
    }

    printf("  TX: worst edge %.3f%% off the 115200-baud grid, %d framing errors\n",
           100 * host.worst_edge_err, host.frame_errors);
    chk(host.frame_errors == 0, "the host saw a TX frame with a low stop bit");
    chk(host.worst_edge_err <= 0.01, "a TX edge is more than 1% off the bit grid");

    top->final();
    delete top;
    printf("%s: %d/%d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    return fails ? 1 : 0;
}
