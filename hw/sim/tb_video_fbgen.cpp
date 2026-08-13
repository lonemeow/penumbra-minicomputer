// Verilator testbench for the framebuffer generator
// (hw/rtl/io/video/video_fbgen.sv).
//
// Plays the timing generator: drives a full mode-0 raster at the
// module's inputs and checks every pixel of a frame against an
// independent oracle that indexes the framebuffer directly. The oracle
// knows only the contract — raster pixel (x, y) shows framebuffer pixel
// (x >> 1, y >> 1), looked up through the palette, black outside the
// active region or with ENABLE clear — so it shares none of the
// module's addressing.
//
// A frame is checked end to end rather than sampled, because the
// addressing's interesting moments are all edges: the repeat of each
// framebuffer line, the wrap at the end of the array, and the hold
// through vertical blanking.

#include <cstdio>
#include <cstdint>
#include "Vvideo_fbgen.h"

// Mode 0, matching video_pkg.
static const int H_ACTIVE = 640, H_FRONT = 16, H_SYNC = 96, H_BACK = 48;
static const int V_ACTIVE = 480, V_FRONT = 10, V_SYNC = 2,  V_BACK = 33;
static const int H_TOTAL = H_ACTIVE + H_FRONT + H_SYNC + H_BACK;
static const int V_TOTAL = V_ACTIVE + V_FRONT + V_SYNC + V_BACK;
static const int H_SYNC_POL = 0, V_SYNC_POL = 0;

static const int FB_W = 320, FB_H = 240;
static const int FB_WORDS = FB_W / 4 * FB_H;

// Edges between a raster position being applied and its color
// appearing. The generator holds three register stages — word read,
// palette read, output register — but the scan counter already stands
// on the address for the current column, so the word read captures on
// the same edge that consumes the input rather than one after it.
// Syncs travel the same three stages, so they shift by the same two
// edges and the picture stays aligned to them on the wire.
static const int LATENCY = 2;

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

// Host-side copy of what was loaded, for the oracle.
static uint8_t  fb[FB_W * FB_H];
static uint32_t pal[256];

// Position-dependent pixel values: neighbouring addresses differ, and
// so do addresses one framebuffer line apart, so an addressing slip of
// either kind changes the color.
static uint8_t pixel_at(int col, int line) {
    return (uint8_t)((line * 7 + col) & 0xFF);
}
static uint32_t color_of(int idx) {
    return (uint32_t)(((idx * 3) & 0xFF) << 16 |
                      ((idx * 5) & 0xFF) << 8 |
                      ((idx * 11) & 0xFF));
}

static void tick_cpu(Vvideo_fbgen* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void tick_pix(Vvideo_fbgen* d) {
    d->i_pclk = 0; d->eval();
    d->i_pclk = 1; d->eval();
}

int main() {
    Vvideo_fbgen* d = new Vvideo_fbgen;
    d->i_clk = 0; d->i_pclk = 0; d->i_rst = 1;
    d->i_fb_we = 0; d->i_fb_byte_en = 0; d->i_pal_we = 0;
    d->i_enable = 0; d->i_de = 0;
    d->i_raster_x = 0; d->i_raster_y = 0;
    d->i_hsync = !H_SYNC_POL; d->i_vsync = !V_SYNC_POL;
    d->eval();

    // Load the picture through the CPU-side aperture ports, four
    // pixels per word.
    for (int line = 0; line < FB_H; line++)
        for (int col = 0; col < FB_W; col++)
            fb[line * FB_W + col] = pixel_at(col, line);

    for (int w = 0; w < FB_WORDS; w++) {
        uint32_t word = 0;
        for (int b = 0; b < 4; b++)
            word |= (uint32_t)fb[w * 4 + b] << (8 * b);
        d->i_fb_we = 1;
        d->i_fb_byte_en = 0xF;
        d->i_fb_addr = w;
        d->i_fb_wdata = word;
        tick_cpu(d);
    }
    d->i_fb_we = 0;

    for (int i = 0; i < 256; i++) {
        pal[i] = color_of(i);
        d->i_pal_we = 1;
        d->i_pal_addr = i;
        d->i_pal_wdata = pal[i];
        tick_cpu(d);
    }
    d->i_pal_we = 0;

    // Release reset with the raster at the top of a frame.
    for (int i = 0; i < 2; i++) tick_pix(d);
    d->i_rst = 0;
    d->i_enable = 1;

    // Expected outputs, delayed by the pipeline.
    struct Expect { uint32_t rgb; bool de; bool hs; bool vs; };
    Expect pipe[LATENCY + 1];
    for (int i = 0; i <= LATENCY; i++) pipe[i] = {0, false, (bool)!H_SYNC_POL, (bool)!V_SYNC_POL};

    // Two frames: the first settles the address counter from reset,
    // the second is checked.
    for (int frame = 0; frame < 2; frame++) {
        for (int y = 0; y < V_TOTAL; y++) {
            for (int x = 0; x < H_TOTAL; x++) {
                bool de = (x < H_ACTIVE) && (y < V_ACTIVE);
                bool hs = (x >= H_ACTIVE + H_FRONT &&
                           x <  H_ACTIVE + H_FRONT + H_SYNC) ? H_SYNC_POL : !H_SYNC_POL;
                bool vs = (y >= V_ACTIVE + V_FRONT &&
                           y <  V_ACTIVE + V_FRONT + V_SYNC) ? V_SYNC_POL : !V_SYNC_POL;

                d->i_raster_x = x;
                d->i_raster_y = y;
                d->i_de = de;
                d->i_hsync = hs;
                d->i_vsync = vs;

                // What this raster position should eventually produce.
                Expect want;
                want.de = de; want.hs = hs; want.vs = vs;
                want.rgb = de ? pal[fb[(y >> 1) * FB_W + (x >> 1)]] : 0;

                tick_pix(d);

                for (int i = LATENCY; i > 0; i--) pipe[i] = pipe[i - 1];
                pipe[0] = want;

                if (frame == 1) {
                    const Expect& e = pipe[LATENCY];
                    uint32_t got = ((uint32_t)d->o_r << 16) |
                                   ((uint32_t)d->o_g << 8) | d->o_b;
                    CHECK(got == e.rgb,
                          "raster (%d,%d): rgb %06x, want %06x", x, y, got, e.rgb);
                    CHECK(d->o_de == e.de,
                          "raster (%d,%d): de %d, want %d", x, y, d->o_de, e.de);
                    CHECK(d->o_hsync == e.hs,
                          "raster (%d,%d): hsync %d, want %d", x, y, d->o_hsync, e.hs);
                    CHECK(d->o_vsync == e.vs,
                          "raster (%d,%d): vsync %d, want %d", x, y, d->o_vsync, e.vs);
                }
                if (errors > 20) goto done;
            }
        }
    }
done:

    // ENABLE clear blanks the picture while the syncs keep running —
    // checked on one active line.
    if (!errors) {
        d->i_enable = 0;
        for (int x = 0; x < H_ACTIVE; x++) {
            d->i_raster_x = x; d->i_raster_y = 100;
            d->i_de = 1;
            d->i_hsync = !H_SYNC_POL; d->i_vsync = !V_SYNC_POL;
            tick_pix(d);
        }
        uint32_t got = ((uint32_t)d->o_r << 16) | ((uint32_t)d->o_g << 8) | d->o_b;
        CHECK(got == 0, "ENABLE clear: rgb %06x, want 000000", got);
        CHECK(d->o_de == 1, "ENABLE clear: de %d, want 1", d->o_de);
    }

    printf("video_fbgen: %d checks, %d errors\n", checks, errors);
    delete d;
    return errors ? 1 : 0;
}
