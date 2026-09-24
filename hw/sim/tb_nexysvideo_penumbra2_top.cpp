// tb_nexysvideo_penumbra2_top — the Nexys Video machine top booting its
// boot ROM, driven and watched at the board pins.
//
// Runs the real boot ROM, so build its image first:
//   make -C hw/rom && make sim MOD=nexysvideo_penumbra2_top
// The MMCM lint stub passes the oscillator straight through, so one
// testbench clock is one CPU clock; the host end of the serial line runs
// at exactly 115200 baud (uart_host.h).
//
// Covers the board wiring under the machine, not the core:
//  - the reset hold, then the ROM's early RAM check passing;
//  - the banner naming the board and its CPU clock;
//  - RAM detection finding exactly the on-chip RAM, which puts both the
//    RAM decode and the bus fault past its end on trial;
//  - the monitor storing words at both ends of RAM and reading them
//    back byte by byte.
#include "Vnexysvideo_penumbra2_top.h"
#include "uart_host.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <string>

// The top's CPU_HZ, the console's baud rate, and its RAM size.
static const uint64_t CPU_HZ = 37500000;
static const uint64_t BAUD = 115200;
static const double BIT = double(CPU_HZ) / double(BAUD);

static Vnexysvideo_penumbra2_top* top;
static uint64_t cycle = 0;
static int checks = 0, fails = 0;
static UartHostRx host(BIT);
static UartHostTx host_tx(BIT);

static void chk(bool ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL at cycle %llu: %s\n", (unsigned long long)cycle, what);
    }
}

static void tick() {
    top->uart_tx_in = host_tx.line(cycle);
    top->clk = 0;
    top->eval();
    top->clk = 1;
    top->eval();
    cycle++;
    host.step(top->uart_rx_out, cycle);
}

// Run until the console output from mark on ends with the monitor's
// prompt; false on timeout.
static bool wait_prompt(size_t mark, uint64_t limit) {
    uint64_t t0 = cycle;
    for (;;) {
        const std::string& t = host.text;
        if (t.size() >= mark + 2 && t.compare(t.size() - 2, 2, "> ") == 0 && !host.in_frame)
            return true;
        if (cycle - t0 >= limit)
            return false;
        tick();
    }
}

// Type a command line and return what the monitor printed up to its
// next prompt.
static std::string command(const std::string& line) {
    size_t mark = host.text.size();
    host_tx.send(line + "\r");
    while (host_tx.busy())
        tick();
    bool ok = wait_prompt(mark + line.size(), 20000000);
    chk(ok, ("no prompt after '" + line + "'").c_str());
    return host.text.substr(mark);
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

static void expect(const std::string& out, const std::string& needle) {
    chk(contains(out, needle), ("console output lacks '" + needle + "'").c_str());
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    top = new Vnexysvideo_penumbra2_top;
    top->cpu_resetn = 1;
    top->uart_tx_in = 1;

    // ── Boot to the monitor prompt ─────────────────────────────────
    bool up = wait_prompt(0, 60000000);
    chk(up, "no monitor prompt within 60M cycles");
    printf("  boot to prompt: %llu cycles (%.1f ms at CPU_HZ)\n",
           (unsigned long long)cycle, cycle * 1000.0 / CPU_HZ);
    const std::string boot = host.text;
    expect(boot, "RAM: OK");
    expect(boot, "Hardware: Nexys Video");
    expect(boot, "@ 37.500 MHz");
    expect(boot, "Detecting base RAM... 128kB found");
    expect(boot, "0 device(s) found");

    // ── Words at both ends of RAM, read back a byte at a time ──────
    if (up) {
        command("w 2000 deadbeef");
        std::string out = command("x 2000 4");
        expect(out, "00002000    ef be ad de");
        command("w 1fffc 01234567");
        out = command("x 1fffc 4");
        expect(out, "0001fffc    67 45 23 01");
    }

    printf("  host: %d framing errors, worst edge %.3f%% off the grid\n",
           host.frame_errors, 100 * host.worst_edge_err);
    chk(host.frame_errors == 0, "the host saw a TX frame with a low stop bit");
    if (fails) {
        printf("---- console output ----\n%s\n------------------------\n",
               host.text.c_str());
    }

    top->final();
    delete top;
    printf("%s: %d/%d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    return fails ? 1 : 0;
}
