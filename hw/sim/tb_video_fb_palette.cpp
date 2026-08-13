// Verilator testbench for the framebuffer palette
// (hw/rtl/io/video/video_fb_palette.sv).
//
// Checks the module contract: every entry stores a 24-bit color and
// reads back what it was given on port A with one-cycle registered
// latency; port B returns the same color for the same index on the
// pixel clock; a write is visible to the pixel side on its next read,
// which is what makes palette animation under a live picture work.

#include <cstdio>
#include <cstdint>
#include "Vvideo_fb_palette.h"

static const int ENTRIES = 256;

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

static void tick_cpu(Vvideo_fb_palette* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void tick_pix(Vvideo_fb_palette* d) {
    d->i_pclk = 0; d->eval();
    d->i_pclk = 1; d->eval();
}

// A distinct color per slot, spanning all three channels.
static uint32_t color(int i) {
    return (uint32_t)(((i * 7) & 0xFF) << 16 |
                      ((i * 13) & 0xFF) << 8 |
                      ((i * 29) & 0xFF));
}

static void wr(Vvideo_fb_palette* d, int i, uint32_t c) {
    d->i_we = 1;
    d->i_addr = i;
    d->i_wdata = c;
    tick_cpu(d);
    d->i_we = 0;
}

int main() {
    Vvideo_fb_palette* d = new Vvideo_fb_palette;
    d->i_clk = 0; d->i_pclk = 0; d->i_we = 0; d->eval();

    for (int i = 0; i < ENTRIES; i++)
        wr(d, i, color(i));

    for (int i = 0; i < ENTRIES; i++) {
        d->i_addr = i;
        tick_cpu(d);
        CHECK(d->o_rdata == color(i),
              "port A slot %3d: got %06x, want %06x",
              i, d->o_rdata, color(i));
    }

    for (int i = 0; i < ENTRIES; i++) {
        d->i_index = i;
        tick_pix(d);
        CHECK(d->o_color == color(i),
              "port B slot %3d: got %06x, want %06x",
              i, d->o_color, color(i));
    }

    // Only the low 24 bits are stored; the aperture's unused top byte
    // must not reach the color.
    {
        wr(d, 5, 0x00FF8040u);
        d->i_index = 5;
        tick_pix(d);
        CHECK(d->o_color == 0x00FF8040u,
              "24-bit slot: got %06x, want %06x", d->o_color, 0x00FF8040u);
    }

    // Rewriting a live entry reaches the pixel side on its next read —
    // the palette-animation path.
    {
        d->i_index = 200;
        tick_pix(d);
        uint32_t before = d->o_color;
        wr(d, 200, 0x00123456u);
        d->i_index = 200;
        tick_pix(d);
        CHECK(before == color(200),
              "pre-animation slot 200: got %06x, want %06x",
              before, color(200));
        CHECK(d->o_color == 0x00123456u,
              "animated slot 200: got %06x, want %06x",
              d->o_color, 0x00123456u);
    }

    // Read output holds across an index change until the next edge.
    {
        d->i_index = 9;
        tick_pix(d);
        uint32_t held = d->o_color;
        d->i_index = 10;
        d->eval();
        CHECK(d->o_color == held,
              "port B output moved without a clock edge: %06x != %06x",
              d->o_color, held);
    }

    printf("video_fb_palette: %d checks, %d errors\n", checks, errors);
    delete d;
    return errors ? 1 : 0;
}
