// tb_nexysvideo_ddr3_selftest_test — the DDR3 self-test sequencer against
// a scripted controller, read back from its serial line.
//
// At simulation scale (nexysvideo_ddr3_selftest_test.sv): a millisecond
// is 1000 cycles and a serial bit exactly ten. The controller model takes
// the sequencer's reset, raises calibration-complete when a run's script
// says, and answers the debug port as UberDDR3 does (a request latched on
// one edge is acked, with its data, on the next). Its self-test counters
// grow by each run's scripted amounts GROW_MS after release, as the real
// self-test runs late in calibration, and never reset. GROW_MS leaves room
// for a lost start-of-run read to be issued again first: the sequencer
// waits 1024 cycles for an ack, a millisecond at this scale, where on the
// board it is microseconds against seconds.
//
// Covers:
//  - the banner, once after reset, with the controller held in reset;
//  - one line per run, whose verdict, counts and calibration time match
//    the run: clean (pass), completed after mismatches (RTRY), never
//    completed within the timeout (FAIL);
//  - counts as what the counters grew by, which carry over between runs;
//  - a debug read the controller loses, issued again;
//  - the status outputs after each run;
//  - a reset in the middle of a character: the line idles for two
//    frames, then the banner arrives whole;
//  - a reset in the middle of a run: the next report counts only its own
//    reads, though the controller's counters ran on through both.
#include "Vnexysvideo_ddr3_selftest_test.h"
#include "uart_host.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const uint64_t MS = 1000;           // cycles
static const double BIT = 10.0;            // cycles
static const int TIMEOUT_MS = 20;
static const int GROW_MS = 2;          // below every scripted calib_ms
static const std::string BANNER =
    "\r\nNexys Video DDR3 self-test: MT41K256M16 (x16), BIST mode 2\r\n";

struct Run {
    int calib_ms;          // -1: never calibrates
    uint32_t matched;      // counter growth over the run
    uint32_t mismatched;
    bool lose_first_read;
    const char* verdict;
    bool pass_led;
    bool mismatch_led;
};

static const std::vector<Run> RUNS = {
    {5,  1000, 0, false, "pass", true,  false},
    {8,  2000, 3, false, "RTRY", false, true},
    {-1, 500,  7, false, "FAIL", false, true},
    {3,  1000, 0, true,  "pass", true,  true},
    {10, 777,  5, false, "",     false, false},   // cut short by reset
};

static Vnexysvideo_ddr3_selftest_test* top;
static uint64_t cycle = 0;
static int checks = 0, fails = 0;
static UartHostRx host(BIT);

// ── Controller model ───────────────────────────────────────────────
static uint32_t matched = 0, mismatched = 0;
static bool latched = false;
static uint8_t latched_addr = 0;
static bool ack_out = false;
static uint32_t data_out = 0;
static bool lose_next_read = false;
static size_t run_idx = 0;
static bool running = false;           // released from reset
static bool grown = false;             // this run's counts added
static uint64_t released_at = 0;
static bool last_rst_n = false;

static void chk(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL at cycle %llu: %s\n", (unsigned long long)cycle, what.c_str());
    }
}

static void tick() {
    // The controller's view of this cycle.
    bool rst_n = top->o_ctrl_rst_n;
    if (rst_n && !last_rst_n) {
        running = true;
        grown = false;
        released_at = cycle;
        if (run_idx < RUNS.size())
            lose_next_read = RUNS[run_idx].lose_first_read;
    } else if (!rst_n && last_rst_n) {
        running = false;
        run_idx++;
    }
    last_rst_n = rst_n;
    if (running && !grown && cycle - released_at >= GROW_MS * MS && run_idx < RUNS.size()) {
        matched += RUNS[run_idx].matched;
        mismatched += RUNS[run_idx].mismatched;
        grown = true;
    }

    bool calib = false;
    if (running && run_idx < RUNS.size() && RUNS[run_idx].calib_ms >= 0)
        calib = cycle - released_at >= (uint64_t)RUNS[run_idx].calib_ms * MS;
    top->i_calib_complete = calib;
    top->i_dbg_stall = 0;
    top->i_dbg_ack = ack_out;
    top->i_dbg_data = data_out;

    top->i_clk = 0;
    top->eval();
    bool stb = top->o_dbg_stb;
    uint8_t addr = top->o_dbg_addr;
    top->i_clk = 1;
    top->eval();
    cycle++;

    ack_out = latched;
    data_out = latched_addr == 15 ? matched : latched_addr == 16 ? mismatched : 0xAAAAAAAAu;
    latched = stb;
    latched_addr = addr;
    if (stb && lose_next_read) {
        latched = false;
        lose_next_read = false;
    }

    host.step(top->o_tx, cycle);
}

static bool wait_text(size_t from, const std::string& needle, uint64_t limit) {
    uint64_t t0 = cycle;
    while (host.text.find(needle, from) == std::string::npos) {
        if (cycle - t0 >= limit)
            return false;
        tick();
    }
    return true;
}

static void reset() {
    top->i_rst = 1;
    for (int i = 0; i < 4; i++)
        tick();
    top->i_rst = 0;
}

// Cycles the line stays high from now, up to limit.
static uint64_t idle_run(uint64_t limit) {
    uint64_t t0 = cycle;
    while (top->o_tx && cycle - t0 < limit)
        tick();
    return cycle - t0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    top = new Vnexysvideo_ddr3_selftest_test;
    reset();

    // ── A reset in the middle of a banner character ───────────────
    // The line idles for two frames first, and the controller stays in
    // reset while the banner goes out.
    uint64_t idle = idle_run(10 * MS);
    chk(idle >= 20 * BIT, "line idle only " + std::to_string(idle) + " cycles after reset");
    while (host.text.size() < 3)
        tick();
    for (int i = 0; i < 43; i++)           // partway into the next frame
        tick();
    chk(!top->o_tx || host.in_frame, "not mid-frame when resetting");
    reset();
    idle = idle_run(10 * MS);
    chk(idle >= 20 * BIT, "line idle only " + std::to_string(idle) + " cycles after a reset mid-frame");
    size_t mark = host.text.size();
    int frame_errors_before = host.frame_errors;
    bool held = true;
    while (host.text.size() < mark + BANNER.size() && cycle < 40 * MS) {
        tick();
        held &= !top->o_ctrl_rst_n;
    }
    chk(host.text.substr(mark) == BANNER, "banner after reset: got '" + host.text.substr(mark) + "'");
    chk(held, "controller released while the banner was going out");

    // ── The runs ──────────────────────────────────────────────────
    for (size_t r = 0; r < 4; r++) {
        const Run& run = RUNS[r];
        size_t from = host.text.size();
        bool saw_testing = false;
        uint64_t t0 = cycle;
        while (host.text.find("\r\n", from) == std::string::npos && cycle - t0 < 60 * MS) {
            tick();
            saw_testing |= top->o_testing;
        }
        std::string line = host.text.substr(from);
        char tag[16];
        snprintf(tag, sizeof tag, "run %02zu", r + 1);
        chk(saw_testing, std::string(tag) + ": o_testing never rose");

        char num[3], verdict[5];
        unsigned reads = 0, mism = 0, ms = 0;
        int n = sscanf(line.c_str(), "run %2s: %4s reads %8x mismatches %8x calibrated %8x ms",
                       num, verdict, &reads, &mism, &ms);
        chk(n == 5, std::string(tag) + ": unreadable line '" + line + "'");
        if (n != 5)
            continue;
        char want_num[3];
        snprintf(want_num, sizeof want_num, "%02zX", r + 1);
        chk(std::string(num) == want_num, std::string(tag) + ": numbered " + num);
        chk(std::string(verdict) == run.verdict,
            std::string(tag) + ": verdict " + verdict + ", want " + run.verdict);
        chk(reads == run.matched, std::string(tag) + ": reads " + std::to_string(reads));
        chk(mism == run.mismatched, std::string(tag) + ": mismatches " + std::to_string(mism));
        int want_ms = run.calib_ms >= 0 ? run.calib_ms : TIMEOUT_MS;
        chk((int)ms >= want_ms - 1 && (int)ms <= want_ms + 1,
            std::string(tag) + ": calibrated after " + std::to_string(ms) + " ms, want about " +
            std::to_string(want_ms));
        chk(top->o_last_pass == run.pass_led, std::string(tag) + ": o_last_pass");
        chk(top->o_mismatch_seen == run.mismatch_led, std::string(tag) + ": o_mismatch_seen");
        printf("  %s", line.c_str());
    }
    chk(host.frame_errors == frame_errors_before, "framing errors on the serial line");

    // ── A reset in the middle of a run ────────────────────────────
    // Run 05 has grown the counters when the reset lands; the first run
    // after it must not count those reads.
    uint64_t t0 = cycle;
    while (!(running && grown) && cycle - t0 < 60 * MS)
        tick();
    chk(running && grown, "run 05 never got going");
    mark = host.text.size();
    reset();
    run_idx = 0;
    chk(wait_text(mark, BANNER, 20 * MS), "no banner after reset");
    chk(!top->o_mismatch_seen, "o_mismatch_seen survived reset");
    size_t line_at = host.text.size();
    chk(wait_text(line_at, " ms\r\n", 60 * MS), "no run 01 after reset");
    std::string line = host.text.substr(line_at);
    chk(line.rfind("run 01: pass reads 000003E8 mismatches 00000000", 0) == 0,
        "run 01 after reset: '" + line + "'");

    printf("%s: %d/%d checks passed\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    delete top;
    return fails ? 1 : 0;
}
