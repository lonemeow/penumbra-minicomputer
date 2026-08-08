// Verilator testbench for the display text output chain
// (hw/rtl/io/video/video_text_chain.sv).
//
// Plays the monitor at the end of the GPDI cable, exactly like
// tb_video_chain_test (character-align on blanking control codes,
// TMDS-decode, locate a frame from the vsync edge, walk one full
// raster) — but the oracle renders the character screen: the same
// splash_cells.hex the DUT preloads, through the same font8x16.hex,
// the CGA palette, and the cursor at its parameter position. The
// recovered frame follows the first vsync edge, well inside the
// cursor's first 32 visible frames, so the oracle draws the cursor
// unconditionally. Run from the repo root so both hex images resolve
// (make test-modules generates them).

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include "Vvideo_text_chain.h"

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

// ── VESA 640x480 @ 60 geometry (the spec oracle, restated) ─────────
static constexpr int H_ACTIVE = 640, H_FRONT = 16, H_SYNC = 96, H_BACK = 48;
static constexpr int V_ACTIVE = 480, V_FRONT = 10, V_SYNC = 2,  V_BACK = 33;
static constexpr int H_TOTAL = 800, V_TOTAL = 525;
static constexpr int H_SYNC_START = H_ACTIVE + H_FRONT;   // 656
static constexpr int V_SYNC_START = V_ACTIVE + V_FRONT;   // 490
static constexpr int COLUMNS = 80;

// The chain's cursor parameters, restated.
static constexpr int CURSOR_COL = 0, CURSOR_ROW = 29;

static const uint32_t CGA[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static std::vector<uint8_t>  font;
static std::vector<uint16_t> cells;

// ── Text oracle: the character screen as the pipeline renders it ───
static void text_oracle(int x, int y, uint8_t* r, uint8_t* g, uint8_t* b) {
    int cc = x / 8, cr = y / 16;
    uint16_t cell = cells[cr * COLUMNS + cc];
    uint8_t glyph = cell & 0xFF, attr = cell >> 8;
    uint8_t row = font[glyph * 16 + (y % 16)];
    bool fgbit = (row >> (7 - (x % 8))) & 1;
    bool cursor = (cc == CURSOR_COL && cr == CURSOR_ROW);
    uint8_t fg = attr & 0xF, bg = attr >> 4;
    uint8_t idx = (fgbit ^ cursor) ? fg : bg;
    uint32_t rgb = CGA[idx];
    *r = (uint8_t)(rgb >> 16);
    *g = (uint8_t)(rgb >> 8);
    *b = (uint8_t)rgb;
}

// Hex image loader: one value per line, // comments skipped.
static bool load_hex(const char* path, std::vector<uint32_t>& out) {
    std::ifstream f(path);
    if (!f) { printf("FAIL: %s not found (run from the repo root)\n", path); return false; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line.rfind("//", 0) == 0) continue;
        out.push_back((uint32_t)strtoul(line.c_str(), nullptr, 16));
    }
    return true;
}

// ── TMDS receive rules ─────────────────────────────────────────────
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

// ── Stimulus ───────────────────────────────────────────────────────
static Vvideo_text_chain* dut;

static constexpr long N_PCLK = (long)H_TOTAL * V_TOTAL * 2 + 5000;
static constexpr int  OFFSET = 3;   // pixel-edge placement in the serial cycle

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

int main() {
    std::vector<uint32_t> font_w, cells_w;
    if (!load_hex("font8x16.hex", font_w) ||
        !load_hex("splash_cells.hex", cells_w))
        return 1;
    if (font_w.size() != 4096 || cells_w.size() != 4096) {
        printf("FAIL: image sizes %zu/%zu, want 4096/4096\n",
               font_w.size(), cells_w.size());
        return 1;
    }
    for (uint32_t v : font_w) font.push_back((uint8_t)v);
    for (uint32_t v : cells_w) cells.push_back((uint16_t)v);

    dut = new Vvideo_text_chain;
    dut->i_cell_we = 0;

    // Synchronous reset, held two pixel periods (testbench convention).
    dut->i_rst = 1; dut->i_pclk = 0; dut->i_sclk = 0; dut->i_clk = 0;
    for (int c = 0; c < 12; c++) {
        int ph = c % 5;
        dut->i_sclk = 0; if (ph == (OFFSET + 3) % 5) dut->i_pclk = 0; dut->eval();
        dut->i_sclk = 1; if (ph == OFFSET) dut->i_pclk = 1; dut->eval();
    }
    dut->i_rst = 0;

    static std::vector<uint8_t> bits[4];
    capture(bits);

    // 1. Character alignment: vote over early symbols on the blue lane.
    const long VOTE_SYMS = 5000;
    int phase = -1; long best = -1;
    for (int p = 0; p < 10; p++) {
        long hits = 0;
        for (long s = 0; s < VOTE_SYMS; s++)
            if (ctrl_index(symbol_at(bits[0], (size_t)(10 * s + p))) >= 0) hits++;
        if (hits > best) { best = hits; phase = p; }
    }
    CHECK(best > VOTE_SYMS / 10,
          "alignment: best phase %d matched only %ld/%ld control symbols",
          phase, best, VOTE_SYMS);

    // 2. Decode symbol streams on the shared phase.
    long n_syms = (long)(bits[0].size() - phase) / 10;
    std::vector<int8_t>  ctrl[3];
    std::vector<uint8_t> data[3];
    for (int l = 0; l < 3; l++) {
        ctrl[l].reserve(n_syms);
        data[l].reserve(n_syms);
        for (long s = 0; s < n_syms; s++) {
            uint16_t q = symbol_at(bits[l], (size_t)(10 * s + phase));
            ctrl[l].push_back((int8_t)ctrl_index(q));
            data[l].push_back(tmds_decode(q));
        }
    }

    // 3. Frame boundary: first vsync deassert edge in blue's control
    // stream; the frame's (0,0) follows V_BACK lines later. The
    // search starts one line in: the symbols before serializer word
    // lock are arbitrary bit windows, and a misframed pair there can
    // fabricate a control-code edge (the generator's pipeline emits
    // blanking codes from reset, so the junk sits right next to
    // legitimate control symbols).
    long vs_edge = -1;
    for (long s = H_TOTAL; s < n_syms; s++) {
        if (ctrl[0][s - 1] >= 0 && ctrl[0][s] >= 0 &&
            !(ctrl[0][s - 1] & 2) && (ctrl[0][s] & 2)) { vs_edge = s; break; }
    }
    CHECK(vs_edge >= 0, "no vsync deassert edge found in %ld symbols", n_syms);

    if (vs_edge >= 0) {
        long frame0 = vs_edge + (long)V_BACK * H_TOTAL;
        CHECK(frame0 + (long)H_TOTAL * V_TOTAL <= n_syms,
              "stream too short for a full frame after the vsync edge");

        // 4. Cycle-exact raster walk over one complete frame.
        long bad_pixels = 0, bad_blank = 0;
        for (int y = 0; y < V_TOTAL; y++) {
            for (int x = 0; x < H_TOTAL; x++) {
                long s = frame0 + (long)y * H_TOTAL + x;
                bool active = (x < H_ACTIVE) && (y < V_ACTIVE);
                int  exp_hs = (x >= H_SYNC_START && x < H_SYNC_START + H_SYNC) ? 0 : 1;
                int  exp_vs = (y >= V_SYNC_START && y < V_SYNC_START + V_SYNC) ? 0 : 1;
                if (active) {
                    uint8_t er, eg, eb;
                    text_oracle(x, y, &er, &eg, &eb);
                    bool ok = ctrl[0][s] < 0 && ctrl[1][s] < 0 && ctrl[2][s] < 0
                           && data[0][s] == eb && data[1][s] == eg && data[2][s] == er;
                    if (!ok) bad_pixels++;
                    CHECK(ok, "pixel (%d,%d): got B/G/R %02x/%02x/%02x "
                              "(ctrl %d/%d/%d), expected %02x/%02x/%02x",
                          x, y, data[0][s], data[1][s], data[2][s],
                          ctrl[0][s], ctrl[1][s], ctrl[2][s], eb, eg, er);
                } else {
                    int exp_idx = (exp_vs << 1) | exp_hs;
                    bool ok = ctrl[0][s] == exp_idx
                           && ctrl[1][s] == 0 && ctrl[2][s] == 0;
                    if (!ok) bad_blank++;
                    CHECK(ok, "blank (%d,%d): got ctrl %d/%d/%d, expected %d/0/0",
                          x, y, ctrl[0][s], ctrl[1][s], ctrl[2][s], exp_idx);
                }
            }
        }
        printf("  frame at symbol %ld: %ld bad active, %ld bad blanking of %d\n",
               frame0, bad_pixels, bad_blank, H_TOTAL * V_TOTAL);
    }

    printf("video_text_chain: %d checks, %d failures\n", checks, errors);
    if (errors == 0)
        printf("  PASS: character screen recovered bit-exact through the TMDS link\n");

    delete dut;
    return errors ? 1 : 0;
}
