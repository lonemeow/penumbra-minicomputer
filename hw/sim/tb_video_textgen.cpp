// Verilator testbench for the display character generator
// (hw/rtl/io/video/video_textgen.sv).
//
// Frame-compare against an independent software oracle: the testbench
// fills the whole 80x30 grid through the CPU-side cell port (so the
// dual-clock write->scan path is exercised, not bypassed), then
// drives full VESA mode-0 rasters on the pixel clock and checks every
// output pixel against its own rendering computed from the same
// font8x16.hex image and the CGA palette. Pinned behaviors:
//   - LATENCY: outputs lag the raster input by exactly 4 pixel clocks
//     (de / hsync / vsync emerge delay-matched with their pixel);
//   - pixel select: font bit [7 - x%8] chooses FG (attr[3:0]) over
//     BG (attr[7:4]);
//   - cursor: inverse video (FG/BG swap) over the whole cursor cell,
//     visible frames 0..31 after reset, hidden frames 32..63 — the
//     blink counter advances on each vsync assertion;
//   - blanking is black regardless of cell contents.
// Frames 0 and 1 are checked pixel-for-pixel (cursor visible), the
// blink counter is then run to the hide phase, and frame 32 is
// checked pixel-for-pixel (cursor hidden). Run from the repo root so
// font8x16.hex resolves (make test-modules generates it).

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <string>
#include <vector>
#include "Vvideo_textgen.h"

// VESA mode-0 timing, restated as the oracle's raster.
static constexpr int H_ACTIVE = 640, H_FRONT = 16, H_SYNC = 96, H_BACK = 48;
static constexpr int V_ACTIVE = 480, V_FRONT = 10, V_SYNC = 2,  V_BACK = 33;
static constexpr int H_TOTAL = H_ACTIVE + H_FRONT + H_SYNC + H_BACK;  // 800
static constexpr int V_TOTAL = V_ACTIVE + V_FRONT + V_SYNC + V_BACK;  // 525
static constexpr int COLUMNS = 80, ROWS = 30;
static constexpr int LATENCY = 4;

static constexpr int CURSOR_COL = 10, CURSOR_ROW = 5;

static const uint32_t CGA[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

struct RasterIn { int x, y; bool de, hs, vs; };

static std::vector<uint8_t> font;
static std::vector<uint16_t> cells(COLUMNS * ROWS);
static int frames_seen = 0;   // vsync assertions driven so far (mirrors DUT)

// The oracle: what the screen must show for active pixel (x, y).
static uint32_t oracle_rgb(int x, int y) {
    int cc = x / 8, cr = y / 16;
    uint16_t cell = cells[cr * COLUMNS + cc];
    uint8_t glyph = cell & 0xFF, attr = cell >> 8;
    uint8_t row = font[glyph * 16 + (y % 16)];
    bool fgbit = (row >> (7 - (x % 8))) & 1;
    bool blink_show = !(frames_seen & 0x20);
    bool cursor = blink_show && cc == CURSOR_COL && cr == CURSOR_ROW;
    uint8_t fg = attr & 0xF, bg = attr >> 4;
    uint8_t idx = cursor ? (fgbit ? bg : fg) : (fgbit ? fg : bg);
    return CGA[idx];
}

static std::deque<RasterIn> raster_pipe;

static void tick_pix(Vvideo_textgen* d) {
    d->i_pclk = 0; d->eval();
    d->i_pclk = 1; d->eval();
}

// Drive one raster position, tick, and (optionally) check the output
// against the input that entered the pipeline LATENCY ticks ago.
static void step(Vvideo_textgen* d, int h, int v, bool check) {
    bool vs_now = (v >= V_ACTIVE + V_FRONT && v < V_ACTIVE + V_FRONT + V_SYNC);
    bool vs_prev = d->i_vsync == 0;   // level currently driven (active-low)
    d->i_x = h;
    d->i_y = v;
    d->i_de = (h < H_ACTIVE && v < V_ACTIVE);
    d->i_hsync = (h >= H_ACTIVE + H_FRONT && h < H_ACTIVE + H_FRONT + H_SYNC) ? 0 : 1;
    d->i_vsync = vs_now ? 0 : 1;
    if (vs_now && !vs_prev)
        frames_seen++;                // mirror the DUT's blink counter
    raster_pipe.push_back({h, v, (bool)d->i_de, (bool)!d->i_hsync, (bool)!d->i_vsync});
    tick_pix(d);
    if (raster_pipe.size() < LATENCY)
        return;
    RasterIn e = raster_pipe.front();
    raster_pipe.pop_front();
    if (!check)
        return;
    CHECK(d->o_de == e.de && (!d->o_hsync) == e.hs && (!d->o_vsync) == e.vs,
          "sync misaligned at (%d,%d): de %d hs %d vs %d",
          e.x, e.y, d->o_de, d->o_hsync, d->o_vsync);
    uint32_t got = ((uint32_t)d->o_r << 16) | ((uint32_t)d->o_g << 8) | d->o_b;
    uint32_t want = e.de ? oracle_rgb(e.x, e.y) : 0;
    CHECK(got == want, "pixel (%d,%d) frame %d: got %06x, want %06x",
          e.x, e.y, frames_seen, got, want);
}

int main() {
    // Reference font — the same image the DUT $readmemh-loads.
    std::ifstream f("font8x16.hex");
    if (!f) {
        printf("FAIL: font8x16.hex not found (run from the repo root)\n");
        return 1;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line.rfind("//", 0) == 0) continue;
        font.push_back((uint8_t)strtoul(line.c_str(), nullptr, 16));
    }
    if (font.size() != 4096) {
        printf("FAIL: font8x16.hex holds %zu bytes, want 4096\n", font.size());
        return 1;
    }

    Vvideo_textgen* d = new Vvideo_textgen;

    // Reset, testbench convention: two cycles on each clock.
    d->i_rst = 1; d->i_clk = 0; d->i_pclk = 0;
    d->i_cell_we = 0; d->i_vsync = 1; d->i_hsync = 1; d->eval();
    tick_pix(d); tick_pix(d);
    d->i_rst = 0; d->eval();

    // Fill the grid through the CPU-side port: every glyph code
    // appears, attributes vary (including FG == BG cells).
    for (int a = 0; a < COLUMNS * ROWS; a++) {
        cells[a] = (uint16_t)(((a * 7 + 3) & 0xFF) << 8 | (a & 0xFF));
        d->i_cell_we = 1;
        d->i_cell_addr = a;
        d->i_cell_wdata = cells[a];
        d->i_clk = 0; d->eval();
        d->i_clk = 1; d->eval();
    }
    d->i_cell_we = 0;

    // CPU-side readback spot checks (registered, port A).
    for (int a : {0, 137, COLUMNS * ROWS - 1}) {
        d->i_cell_addr = a;
        d->i_clk = 0; d->eval();
        d->i_clk = 1; d->eval();
        CHECK(d->o_cell_rdata == cells[a],
              "cell readback %d: got %04x, want %04x", a, d->o_cell_rdata, cells[a]);
    }

    d->i_cursor_en = 1;
    d->i_cursor_col = CURSOR_COL;
    d->i_cursor_row = CURSOR_ROW;

    // Frames 0 and 1: full pixel-for-pixel check, cursor visible.
    // Frames 2..31: rendered unchecked to advance the blink counter.
    // Frame 32: full check again, cursor now hidden.
    for (int frame = 0; frame <= 32; frame++) {
        bool check = (frame <= 1 || frame == 32);
        for (int v = 0; v < V_TOTAL; v++)
            for (int h = 0; h < H_TOTAL; h++)
                step(d, h, v, check);
    }

    printf("video_textgen: %d checks, %d errors\n", checks, errors);
    return errors != 0;
}
