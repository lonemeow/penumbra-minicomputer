// Verilator testbench for the CLASS_DISPLAY bus device
// (hw/rtl/io/video/video_display.sv).
//
// Tests the device against the class contract
// (doc/system/devices/display.md) at its two faces:
//   - bus side: CAP/INFO identity values, CTRL reset state
//     (ENABLE = 1, CURSOR_EN = 0), faithful CURSOR readback, cell
//     aperture write/readback with zero upper halves, absent slots
//     reading 0, and the busy contract — every read stalls exactly
//     one cycle, writes never stall;
//   - glass side: the TMDS receiver (same decode as the chain tbs)
//     confirms bus-visible state changes reach the screen — a cell
//     written over the bus renders its glyph, clearing CTRL.ENABLE
//     blacks the picture while blanking keeps its control-code
//     cadence, and CURSOR + CURSOR_EN paint the inverse-video cell.
// Run from the repo root so font8x16.hex resolves.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "Vvideo_display.h"

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

// ── Contract constants ─────────────────────────────────────────────
static constexpr uint32_t REG_CAP = 0x000, REG_INFO = 0x004;
static constexpr uint32_t REG_CTRL = 0x008, REG_CURSOR = 0x00C;
static constexpr uint32_t CELLS = 0x1000;
static constexpr uint32_t CAP_EXPECT  = (1u << 16) | (1u << 10) | (1u << 8) | 1u;
static constexpr uint32_t INFO_EXPECT = (30u << 16) | 80u;

static constexpr int H_ACTIVE = 640, H_FRONT = 16, H_SYNC = 96;
static constexpr int V_ACTIVE = 480, V_FRONT = 10, V_SYNC = 2, V_BACK = 33;
static constexpr int H_TOTAL = 800, V_TOTAL = 525;

static const uint32_t CGA[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static Vvideo_display* dut;
static std::vector<uint8_t> font;

// ── Bus access, modeling the CPU's STALL behavior ──────────────────
static void tick_cpu() {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static uint32_t bus_read(uint32_t off) {
    dut->i_addr = off; dut->i_re = 1; dut->eval();
    CHECK(dut->o_busy, "read 0x%03x: busy not asserted on launch", off);
    tick_cpu();
    dut->eval();
    CHECK(!dut->o_busy, "read 0x%03x: busy still high on the data cycle", off);
    uint32_t v = dut->o_rdata;
    dut->i_re = 0;
    tick_cpu();
    return v;
}

static void bus_write(uint32_t off, uint32_t val) {
    dut->i_addr = off; dut->i_wdata = val; dut->i_we = 1; dut->eval();
    CHECK(!dut->o_busy, "write 0x%03x: writes must not stall", off);
    tick_cpu();
    dut->i_we = 0;
    tick_cpu();
}

// ── TMDS receiver (as in the chain testbenches) ────────────────────
static const uint16_t CTRL_WORD[4] = { 0b1101010100, 0b0010101011,
                                       0b0101010100, 0b1010101011 };

static int ctrl_index(uint16_t q) {
    for (int i = 0; i < 4; i++)
        if (q == CTRL_WORD[i]) return i;
    return -1;
}

static uint8_t tmds_decode(uint16_t q) {
    uint8_t v = q & 0xff;
    if (q & (1 << 9)) v = (uint8_t)~v;
    uint8_t d = v & 1;
    for (int i = 1; i < 8; i++) {
        int bit = ((v >> i) & 1) ^ ((v >> (i - 1)) & 1);
        if (!((q >> 8) & 1)) bit = !bit;
        d |= (uint8_t)bit << i;
    }
    return d;
}

static constexpr long N_PCLK = (long)H_TOTAL * V_TOTAL * 2 + 5000;
static constexpr int  OFFSET = 3;

static void capture(std::vector<uint8_t> bits[4]) {
    for (long cyc = 0; cyc < N_PCLK * 5; cyc++) {
        int ph = (int)(cyc % 5);
        dut->i_sclk = 0;
        if (ph == (OFFSET + 3) % 5) dut->i_pclk = 0;
        dut->eval();
        dut->i_sclk = 1;
        if (ph == OFFSET) dut->i_pclk = 1;
        dut->eval();
        for (int l = 0; l < 4; l++) {
            bits[l].push_back((uint8_t)((dut->o_d0 >> l) & 1));
            bits[l].push_back((uint8_t)((dut->o_d1 >> l) & 1));
        }
    }
}

static uint16_t symbol_at(const std::vector<uint8_t>& bits, size_t p) {
    uint16_t q = 0;
    for (int i = 0; i < 10; i++) q |= (uint16_t)bits[p + i] << i;
    return q;
}

// Capture two frames and decode one aligned complete frame; returns
// the symbol index of its (0,0), or -1.
struct Decoded {
    std::vector<int8_t>  ctrl[3];
    std::vector<uint8_t> data[3];
    long frame0 = -1;
};

static void decode_frame(Decoded& out) {
    static std::vector<uint8_t> bits[4];
    for (auto& b : bits) b.clear();
    capture(bits);

    const long VOTE_SYMS = 5000;
    int phase = -1; long best = -1;
    for (int p = 0; p < 10; p++) {
        long hits = 0;
        for (long s = 0; s < VOTE_SYMS; s++)
            if (ctrl_index(symbol_at(bits[0], (size_t)(10 * s + p))) >= 0) hits++;
        if (hits > best) { best = hits; phase = p; }
    }
    long n_syms = (long)(bits[0].size() - phase) / 10;
    for (int l = 0; l < 3; l++) {
        out.ctrl[l].assign(n_syms, -1);
        out.data[l].assign(n_syms, 0);
        for (long s = 0; s < n_syms; s++) {
            uint16_t q = symbol_at(bits[l], (size_t)(10 * s + phase));
            out.ctrl[l][s] = (int8_t)ctrl_index(q);
            out.data[l][s] = tmds_decode(q);
        }
    }
    long vs_edge = -1;
    for (long s = H_TOTAL; s < n_syms; s++) {
        if (out.ctrl[0][s - 1] >= 0 && out.ctrl[0][s] >= 0 &&
            !(out.ctrl[0][s - 1] & 2) && (out.ctrl[0][s] & 2)) { vs_edge = s; break; }
    }
    if (vs_edge >= 0 &&
        vs_edge + (long)(V_BACK + V_TOTAL) * H_TOTAL <= n_syms)
        out.frame0 = vs_edge + (long)V_BACK * H_TOTAL;
    CHECK(out.frame0 >= 0, "no complete frame recovered");
}

// Check one active pixel of a decoded frame against a 24-bit color.
static void check_pixel(const Decoded& d, int x, int y, uint32_t rgb) {
    long s = d.frame0 + (long)y * H_TOTAL + x;
    uint8_t er = (uint8_t)(rgb >> 16), eg = (uint8_t)(rgb >> 8), eb = (uint8_t)rgb;
    CHECK(d.ctrl[0][s] < 0 && d.ctrl[1][s] < 0 && d.ctrl[2][s] < 0 &&
          d.data[0][s] == eb && d.data[1][s] == eg && d.data[2][s] == er,
          "pixel (%d,%d): got B/G/R %02x/%02x/%02x (ctrl %d/%d/%d), want %06x",
          x, y, d.data[0][s], d.data[1][s], d.data[2][s],
          d.ctrl[0][s], d.ctrl[1][s], d.ctrl[2][s], rgb);
}

int main() {
    std::ifstream f("font8x16.hex");
    if (!f) { printf("FAIL: font8x16.hex not found (run from the repo root)\n"); return 1; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line.rfind("//", 0) == 0) continue;
        font.push_back((uint8_t)strtoul(line.c_str(), nullptr, 16));
    }
    if (font.size() != 4096) { printf("FAIL: bad font image\n"); return 1; }

    dut = new Vvideo_display;

    // Reset both domains (two cycles each, testbench convention).
    dut->i_rst = 1; dut->i_vrst = 1;
    dut->i_clk = 0; dut->i_pclk = 0; dut->i_sclk = 0;
    dut->i_we = 0; dut->i_re = 0; dut->eval();
    tick_cpu(); tick_cpu();
    for (int c = 0; c < 12; c++) {
        int ph = c % 5;
        dut->i_sclk = 0; if (ph == (OFFSET + 3) % 5) dut->i_pclk = 0; dut->eval();
        dut->i_sclk = 1; if (ph == OFFSET) dut->i_pclk = 1; dut->eval();
    }
    dut->i_rst = 0; dut->i_vrst = 0; dut->eval();

    // ── Bus contract ────────────────────────────────────────────
    CHECK(bus_read(REG_CAP) == CAP_EXPECT, "CAP: got %08x, want %08x",
          bus_read(REG_CAP), CAP_EXPECT);
    CHECK(bus_read(REG_INFO) == INFO_EXPECT, "INFO: got %08x, want %08x",
          bus_read(REG_INFO), INFO_EXPECT);
    CHECK(bus_read(REG_CTRL) == 0x1, "CTRL reset: got %08x, want 1 (ENABLE)",
          bus_read(REG_CTRL));
    CHECK(bus_read(0x010) == 0 && bus_read(0x01C) == 0 && bus_read(0x020) == 0,
          "absent option slots must read 0");

    bus_write(REG_CURSOR, 0xDEADBEEF);
    CHECK(bus_read(REG_CURSOR) == 0xDEADBEEF, "CURSOR readback unfaithful");
    bus_write(REG_CURSOR, 0);

    bus_write(REG_CTRL, 0xFFFFFFFF);
    CHECK(bus_read(REG_CTRL) == 0x3, "CTRL: unimplemented bits must read 0");
    bus_write(REG_CTRL, 0x1);   // back to reset state

    // Cells: write/readback through the aperture, zero upper half.
    bus_write(CELLS + 0 * 4, 0x00070741);          // 'A', grey on black
    bus_write(CELLS + 137 * 4, 0xFFFF0F42);        // upper half must drop
    CHECK(bus_read(CELLS + 0 * 4) == 0x0741, "cell 0 readback");
    CHECK(bus_read(CELLS + 137 * 4) == 0x0F42, "cell 137 readback: got %08x",
          bus_read(CELLS + 137 * 4));
    bus_write(CELLS + 137 * 4, 0x0020);            // clear again (space, attr 0)

    // ── Glass side 1: the written glyph renders ─────────────────
    Decoded d1;
    decode_frame(d1);
    if (d1.frame0 >= 0) {
        long bad = 0;
        for (int y = 0; y < 16 && errors < 20; y++) {
            uint8_t row = font[0x41 * 16 + y];
            for (int x = 0; x < 8; x++) {
                bool fg = (row >> (7 - x)) & 1;
                check_pixel(d1, x, y, fg ? CGA[7] : CGA[0]);
                if (errors) bad++;
            }
        }
        // A neighboring untouched cell stays black (glyph 0, attr 0).
        check_pixel(d1, 8 + 3, 5, 0x000000);
    }

    // ── Glass side 2: ENABLE off blacks the picture ─────────────
    bus_write(REG_CTRL, 0x0);
    Decoded d2;
    decode_frame(d2);
    if (d2.frame0 >= 0) {
        for (int i = 0; i < 64; i++)
            check_pixel(d2, (i * 37) % H_ACTIVE, (i * 91) % V_ACTIVE, 0x000000);
    }

    // ── Glass side 3: the cursor paints inverse video ───────────
    bus_write(CELLS + 2 * 4, 0x0720);              // space, grey on black
    bus_write(REG_CURSOR, (0u << 16) | 2u);        // row 0, col 2
    bus_write(REG_CTRL, 0x3);                      // ENABLE | CURSOR_EN
    Decoded d3;
    decode_frame(d3);
    if (d3.frame0 >= 0) {
        // Inverse of a blank grey-on-black cell is a solid grey block.
        for (int y = 0; y < 16; y += 5)
            for (int x = 0; x < 8; x += 3)
                check_pixel(d3, 16 + x, y, CGA[7]);
        // The 'A' cell is not the cursor cell and renders normally.
        uint8_t row = font[0x41 * 16 + 8];
        check_pixel(d3, 0, 8, ((row >> 7) & 1) ? CGA[7] : CGA[0]);
    }

    printf("video_display: %d checks, %d errors\n", checks, errors);
    return errors != 0;
}
