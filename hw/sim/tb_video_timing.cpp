// Verilator testbench for the Penumbra video timing generator
// (hw/rtl/io/video/video_timing.sv).
//
// Drives the pixel clock for two full frames and checks the output
// stream cycle-by-cycle against VESA 640x480 @ 60 Hz timing, which is
// recomputed here independently as the oracle:
//   - the raster counters o_x/o_y enumerate 0..799 x 0..524 and wrap
//   - o_de is high exactly over the 640x480 active region
//   - o_hsync is asserted (active-low) for 96 cycles starting at col 656
//   - o_vsync is asserted (active-low) for 2 lines starting at line 490
//
// The testbench tracks its own expected raster position rather than
// trusting o_x/o_y, so the counters are checked against an independent
// source. No PHY, no PLL — this is the parallel-RGB seam, fully
// verifiable before any TMDS or clocking hardware exists.

#include <cstdio>
#include <cstdint>
#include "Vvideo_timing.h"

// VESA 640x480 @ 60 Hz — the module's defaults, restated as the spec
// oracle (kept separate from the RTL on purpose).
static constexpr int H_ACTIVE = 640, H_FRONT = 16, H_SYNC = 96, H_BACK = 48;
static constexpr int V_ACTIVE = 480, V_FRONT = 10, V_SYNC = 2,  V_BACK = 33;
static constexpr int H_TOTAL = H_ACTIVE + H_FRONT + H_SYNC + H_BACK;  // 800
static constexpr int V_TOTAL = V_ACTIVE + V_FRONT + V_SYNC + V_BACK;  // 525
static constexpr int H_SYNC_START = H_ACTIVE + H_FRONT;               // 656
static constexpr int V_SYNC_START = V_ACTIVE + V_FRONT;               // 490
// Active-low sync (POL = 0): asserted level is 0, idle level is 1.
static constexpr int H_SYNC_POL = 0, V_SYNC_POL = 0;

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

static void tick(Vvideo_timing* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();   // posedge: counters advance, then comb resettles
}

int main() {
    Vvideo_timing* d = new Vvideo_timing;

    // Synchronous reset, held two cycles (testbench convention).
    d->i_rst = 1; d->i_clk = 0; d->eval();
    tick(d);
    tick(d);
    d->i_rst = 0;
    d->eval();                 // registers now (0,0); settle outputs for it

    const int FRAMES = 2;
    long de_count = 0;
    long hsync_assert_cycles = 0;   // cycles hsync sits at its asserted level
    long vsync_assert_cycles = 0;
    int  ex = 0, ey = 0;            // independently-tracked raster position

    for (int f = 0; f < FRAMES; f++) {
        for (int n = 0; n < H_TOTAL * V_TOTAL; n++) {
            // Expected outputs as a pure function of raster position (ex, ey).
            int exp_de    = (ex < H_ACTIVE) && (ey < V_ACTIVE);
            int in_hsync  = (ex >= H_SYNC_START) && (ex < H_SYNC_START + H_SYNC);
            int in_vsync  = (ey >= V_SYNC_START) && (ey < V_SYNC_START + V_SYNC);
            int exp_hsync = in_hsync ? H_SYNC_POL : !H_SYNC_POL;
            int exp_vsync = in_vsync ? V_SYNC_POL : !V_SYNC_POL;

            // Sample BEFORE advancing — outputs are valid for (ex, ey) now.
            CHECK((int)d->o_x == ex && (int)d->o_y == ey,
                  "raster pos: got (%d,%d), expected (%d,%d)",
                  (int)d->o_x, (int)d->o_y, ex, ey);
            CHECK((int)d->o_de == exp_de,
                  "de at (%d,%d): got %d, expected %d", ex, ey, (int)d->o_de, exp_de);
            CHECK((int)d->o_hsync == exp_hsync,
                  "hsync at col %d (line %d): got %d, expected %d",
                  ex, ey, (int)d->o_hsync, exp_hsync);
            CHECK((int)d->o_vsync == exp_vsync,
                  "vsync at line %d (col %d): got %d, expected %d",
                  ey, ex, (int)d->o_vsync, exp_vsync);

            if (d->o_de) de_count++;
            if ((int)d->o_hsync == H_SYNC_POL) hsync_assert_cycles++;
            if ((int)d->o_vsync == V_SYNC_POL) vsync_assert_cycles++;

            tick(d);
            if (++ex == H_TOTAL) { ex = 0; if (++ey == V_TOTAL) ey = 0; }
        }
    }

    // Headline VESA aggregates over the whole run.
    long exp_de    = (long)FRAMES * H_ACTIVE * V_ACTIVE;   // active pixels
    long exp_hsync = (long)FRAMES * V_TOTAL * H_SYNC;      // pulse every line
    long exp_vsync = (long)FRAMES * V_SYNC * H_TOTAL;      // 2 full lines/frame
    CHECK(de_count == exp_de,
          "active pixels: got %ld, expected %ld", de_count, exp_de);
    CHECK(hsync_assert_cycles == exp_hsync,
          "hsync asserted cycles: got %ld, expected %ld", hsync_assert_cycles, exp_hsync);
    CHECK(vsync_assert_cycles == exp_vsync,
          "vsync asserted cycles: got %ld, expected %ld", vsync_assert_cycles, exp_vsync);

    printf("video_timing: %d checks, %d failures "
           "(de=%ld hsync=%ld vsync=%ld over %d frames)\n",
           checks, errors, de_count, hsync_assert_cycles, vsync_assert_cycles, FRAMES);
    if (errors == 0)
        printf("  PASS: 640x480@60 timing matches VESA\n");

    delete d;
    return errors ? 1 : 0;
}
