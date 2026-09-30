// tb_nexysvideo_penumbra2_top — the Nexys Video machine top booting its
// boot ROM, driven and watched at the board pins.
//
// Runs the real boot ROM and the display's font, so build both images
// first:
//   make -C hw/rom && make font8x16.hex && make sim MOD=nexysvideo_penumbra2_top
// The MMCM lint stub passes the oscillator straight through, so one
// testbench clock is one CPU clock; the host end of the serial line runs
// at exactly 115200 baud (uart_host.h). The display's clocks are their
// simulation stand-in (hw/rtl/sim/nexysvideo/nexysvideo_video_clk.sv):
// the serial clock is the oscillator, the pixel clock a fifth of it.
//
// The DDR3 controller is its simulation stand-in
// (hw/rtl/sim/nexysvideo/nexysvideo_ddr3.sv): the line-memory model of
// its port, calibrating before it answers, behind the real stack.
//
// Covers the board wiring under the machine, not the core:
//  - the reset hold, through the memory's calibration, then the ROM's
//    early RAM check passing;
//  - the banner naming the board and its CPU clock;
//  - RAM detection finding exactly the 512 MiB of DDR3, which puts both
//    the RAM decode and the bus fault past its end on trial;
//  - the monitor storing words at both ends of RAM and reading them
//    back byte by byte;
//  - the LEDs: calibration reported, and the RAM's longest wait read
//    out against the bus watchdog's limit;
//  - the microSD slot: unpowered (sd_reset high) through the reset
//    hold and powered after it, found by autoconfig, and read over the
//    real SPI pins by `part sd:0,0` from an image with one partition.
//    The card is sd_card_sim.h behind a mode-0 SPI pin shim, which
//    answers each byte one byte late, as a card is free to;
//  - the display: found by autoconfig after the SD slot, and DVI on the
//    HDMI pins over a whole frame of mode 0 — the clock pair carrying
//    the pixel clock, every pair's N the inverse of its P, the syncs in
//    blue's control tokens and green and red idle in theirs, each token
//    exactly as often as the raster's blanking makes it. A swapped lane,
//    pair, bit pair or bit order each breaks one of those.
#include "Vnexysvideo_penumbra2_top.h"
#include "sd_card_sim.h"
#include "uart_host.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// The top's CPU_HZ, the console's baud rate, and its RAM size.
static const uint64_t CPU_HZ = 60000000;
static const uint64_t BAUD = 115200;
static const double BIT = double(CPU_HZ) / double(BAUD);

static Vnexysvideo_penumbra2_top* top;
static uint64_t cycle = 0;
static int checks = 0, fails = 0;
static UartHostRx host(BIT);
static UartHostTx host_tx(BIT);

// A card image: an MBR with one bootable FAT32-LBA partition of 32 MB
// at LBA 2048, the file sized to hold it (sparse).
static const char* SD_IMAGE = "build/nexysvideo_sd_test.img";
static void make_sd_image() {
    FILE* f = fopen(SD_IMAGE, "wb");
    uint8_t mbr[512] = {};
    uint8_t* e = mbr + 446;
    e[0] = 0x80;                             // bootable
    e[4] = 0x0C;                             // FAT32 LBA
    const uint32_t start = 2048, count = 65536;
    for (int i = 0; i < 4; i++) {
        e[8 + i]  = (start >> (8 * i)) & 0xFF;
        e[12 + i] = (count >> (8 * i)) & 0xFF;
    }
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    fwrite(mbr, 1, sizeof mbr, f);
    fseek(f, (long)(start + count) * 512 - 1, SEEK_SET);
    fputc(0, f);
    fclose(f);
}

// The card's SPI pins, mode 0: MOSI is sampled on SCK's rising edge and
// MISO changes only on its falling edge. A byte's reply is shifted out
// during the next byte. Unpowered, the card leaves MISO pulled high.
struct SdPins {
    SdCardSim* card = nullptr;
    uint8_t mosi_byte = 0;
    uint8_t miso_byte = 0xFF;
    uint8_t reply = 0xFF;       // the card's answer to the last full byte
    int bits = 0;
    bool boundary = false;      // a byte just completed: load reply next
    bool last_sck = false;
    bool last_cs_n = true;

    bool miso(bool powered) const { return !powered || ((miso_byte >> 7) & 1); }

    void step(bool powered, bool cs_n, bool sck, bool mosi) {
        bool selected = powered && !cs_n;
        card->select(selected);
        if (selected && last_cs_n) {
            // A newly selected card has nothing queued (select() clears
            // the model's queue on deselect), so it starts on 0xFF.
            bits = 0;
            boundary = false;
            reply = 0xFF;
            miso_byte = 0xFF;
        }
        if (selected && sck && !last_sck) {
            mosi_byte = (uint8_t)((mosi_byte << 1) | mosi);
            if (++bits == 8) {
                reply = card->exchange(mosi_byte);
                bits = 0;
                boundary = true;
            }
        } else if (selected && !sck && last_sck) {
            if (boundary) {
                miso_byte = reply;
                boundary = false;
            } else {
                miso_byte = (uint8_t)((miso_byte << 1) | 1);
            }
        }
        last_sck = sck;
        last_cs_n = cs_n || !powered;
    }
};

static SdPins sd;

// The HDMI pins, sampled once per half of the serial clock: one TMDS
// bit on every pair per sample. Bit i of a sample is pair i's P in the
// display's lane order — hdmi_tx_p[0] blue, [1] green, [2] red, then
// the clock pair.
static const int CLOCK_PAIR = 3;

struct HdmiPins {
    bool capturing = false;
    bool complementary = true;   // every N the inverse of its P, so far
    std::vector<uint8_t> samples;

    void sample() {
        uint8_t p = (top->hdmi_tx_p & 7) | (top->hdmi_tx_clk_p << CLOCK_PAIR);
        uint8_t n = (top->hdmi_tx_n & 7) | (top->hdmi_tx_clk_n << CLOCK_PAIR);
        complementary &= (p ^ n) == 0xF;
        if (capturing)
            samples.push_back(p);
    }
};

static HdmiPins hdmi;

static void chk(bool ok, const char* what) {
    checks++;
    if (!ok) {
        fails++;
        printf("  FAIL at cycle %llu: %s\n", (unsigned long long)cycle, what);
    }
}

static void tick() {
    bool powered = !top->sd_reset;
    top->uart_tx_in = host_tx.line(cycle);
    top->sd_d = (top->sd_d & ~1u) | (sd.miso(powered) ? 1u : 0u);
    top->clk = 0;
    top->eval();
    hdmi.sample();
    top->clk = 1;
    top->eval();
    hdmi.sample();
    cycle++;
    host.step(top->uart_rx_out, cycle);
    sd.step(!top->sd_reset, (top->sd_d >> 3) & 1, top->sd_cclk, top->sd_cmd);
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

// Mode 0's raster (video_pkg): words per line and lines per frame, the
// active area, and the sync pulses' lengths.
static const long H_TOTAL = 800, V_TOTAL = 525;
static const long H_ACTIVE = 640, V_ACTIVE = 480;
static const long H_SYNC = 96, V_SYNC = 2;
static const int WORD_BITS = 10;

// DVI's four control tokens, as 10-bit words, indexed by {c1, c0}.
// Blue carries {vsync, hsync}, both active-low in mode 0; green and red
// tie theirs to 00.
static const uint16_t CTRL_TOKEN[4] = {0x354, 0x0AB, 0x154, 0x2AB};
// The clock pair's word: five 1s then five 0s, LSB first.
static const uint16_t CLOCK_WORD = 0x01F;

// Capture one frame's worth of words from the HDMI pins and check the
// link carries mode 0. Counting over any window one frame long gives
// the per-frame counts, so the capture need not start on a frame.
static void check_hdmi_frame() {
    const long words = H_TOTAL * V_TOTAL;
    hdmi.samples.clear();
    hdmi.samples.reserve((words + 2) * WORD_BITS);
    hdmi.capturing = true;
    while (hdmi.samples.size() < (size_t)(words + 2) * WORD_BITS)
        tick();
    hdmi.capturing = false;
    const std::vector<uint8_t>& s = hdmi.samples;

    // A word starts where the clock pair rises.
    auto clock_bit = [&](size_t i) { return (s[i] >> CLOCK_PAIR) & 1; };
    size_t start = 1;
    while (start <= (size_t)WORD_BITS && !(clock_bit(start) && !clock_bit(start - 1)))
        start++;
    chk(start <= (size_t)WORD_BITS, "the HDMI clock pair never rises");
    if (start > (size_t)WORD_BITS)
        return;

    long clock_bad = 0;
    long tokens[3][4] = {};
    for (long w = 0; w < words; w++) {
        uint16_t word[4] = {};
        for (int b = 0; b < WORD_BITS; b++) {
            uint8_t x = s[start + w * WORD_BITS + b];
            for (int pair = 0; pair < 4; pair++)
                word[pair] |= ((x >> pair) & 1) << b;
        }
        clock_bad += word[CLOCK_PAIR] != CLOCK_WORD;
        for (int lane = 0; lane < 3; lane++)
            for (int t = 0; t < 4; t++)
                tokens[lane][t] += word[lane] == CTRL_TOKEN[t];
    }

    // Blanking is every word outside the active area, and the sync
    // pulses lie within it: hsync every line, vsync over whole lines.
    const long blank = words - H_ACTIVE * V_ACTIVE;
    const long both = H_SYNC * V_SYNC;
    const long hsync_only = H_SYNC * V_TOTAL - both;
    const long vsync_only = H_TOTAL * V_SYNC - both;
    const long blue[4] = {both, vsync_only, hsync_only,
                          blank - both - hsync_only - vsync_only};
    const long idle[4] = {blank, 0, 0, 0};

    printf("  hdmi: blue tokens {vs,hs} 00/01/10/11: %ld/%ld/%ld/%ld; "
           "green/red idle: %ld/%ld\n",
           tokens[0][0], tokens[0][1], tokens[0][2], tokens[0][3],
           tokens[1][0], tokens[2][0]);
    chk(clock_bad == 0, "the HDMI clock pair does not carry the pixel clock");
    chk(hdmi.complementary, "an HDMI pair's N is not the inverse of its P");
    for (int t = 0; t < 4; t++) {
        chk(tokens[0][t] == blue[t], "blue's control tokens do not carry mode 0's syncs");
        chk(tokens[1][t] == idle[t], "green's control tokens are not idle blanking");
        chk(tokens[2][t] == idle[t], "red's control tokens are not idle blanking");
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    make_sd_image();
    SdCardSim card(SD_IMAGE);
    sd.card = &card;

    top = new Vnexysvideo_penumbra2_top;
    top->cpu_resetn = 1;
    top->uart_tx_in = 1;

    // ── The card stays unpowered through the reset hold ────────────
    bool off_in_reset = true;
    for (int i = 0; i < 1000; i++) {
        tick();
        off_in_reset &= top->sd_reset;
    }
    chk(off_in_reset, "sd_reset low (card powered) during the reset hold");

    // ── Boot to the monitor prompt ─────────────────────────────────
    // RAM detection probes each of the 131072 pages in turn.
    bool up = wait_prompt(0, 150000000);
    chk(up, "no monitor prompt within 150M cycles");
    printf("  boot to prompt: %llu cycles (%.1f ms at CPU_HZ)\n",
           (unsigned long long)cycle, cycle * 1000.0 / CPU_HZ);
    const std::string boot = host.text;
    expect(boot, "RAM: OK");
    expect(boot, "Hardware: Nexys Video");
    // The banner's format: kHz digits only when the clock has any.
    char banner_mhz[32];
    unsigned long long mhz = CPU_HZ / 1000000, khz = CPU_HZ / 1000 % 1000;
    if (khz)
        snprintf(banner_mhz, sizeof banner_mhz, "@ %llu.%03llu MHz", mhz, khz);
    else
        snprintf(banner_mhz, sizeof banner_mhz, "@ %llu MHz", mhz);
    expect(boot, banner_mhz);
    expect(boot, "Detecting base RAM... 512MB found");
    expect(boot, "  SD SD @ 0x");
    expect(boot, "  DISPLAY Display @ 0x");
    expect(boot, "2 device(s) found");
    expect(boot, "sd:0,0 ... card present");
    chk(!top->sd_reset, "sd_reset still high (card unpowered) after the reset hold");

    // ── Words at both ends of RAM, read back a byte at a time ──────
    if (up) {
        command("w 2000 deadbeef");
        std::string out = command("x 2000 4");
        expect(out, "00002000    ef be ad de");
        command("w 1ffffffc 01234567");
        out = command("x 1ffffffc 4");
        expect(out, "1ffffffc    67 45 23 01");

        // Sector 0 over the SD pins: the MBR's one partition.
        out = command("part sd:0,0");
        expect(out, "MBR partition table on device");
        expect(out, "FAT32-LBA 2048 32MB (bootable)");

        // The ROM has drawn its splash and enabled the display by now.
        check_hdmi_frame();
    }

    // led[3] the DDR3 calibrated; led[6:4] the RAM's longest wait
    // against the watchdog's limit, from an eighth up to a half. The wait
    // is reported, not judged: here it is the stand-in's random latency
    // with both clock-manager stubs passing the oscillator through, so
    // controller cycles cost whole CPU cycles, where the board's
    // controller clock runs faster than its CPU clock. The margin is the
    // board's to measure.
    printf("  leds: %02x (RAM wait at least %s of the watchdog's limit)\n", top->led,
           (top->led >> 6) & 1 ? "a half" : (top->led >> 5) & 1 ? "a quarter"
           : (top->led >> 4) & 1 ? "an eighth" : "nothing");
    chk((top->led >> 3) & 1, "led[3] (DDR3 calibrated) dark after boot");

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
