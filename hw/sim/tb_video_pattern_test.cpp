// Frame-dump testbench for the Penumbra display generator front end
// (video_timing + video_pattern_gen, wired up in video_pattern_test.sv).
//
// Drives the pixel clock for one full frame, captures the active region
// into a framebuffer, and writes it out as a binary PPM
// (build/video_pattern.ppm) for visual inspection — the "screenshot
// from simulation" that catches addressing / channel-order bugs by eye,
// with no TMDS or PLL hardware involved.
//
// The automatic checks are deliberately pattern-agnostic, so they hold
// for whatever test pattern the generator produces:
//   - every blanking pixel (de=0) must be black (a DVI requirement)
//   - the active region must contain at least one non-black pixel
//   - the active pixel count must be exactly 640x480
// The look of the pattern itself is verified by opening the PPM.

#include <cstdio>
#include <cstdint>
#include "Vvideo_pattern_test.h"

static constexpr int H_ACTIVE = 640, H_FRONT = 16, H_SYNC = 96, H_BACK = 48;
static constexpr int V_ACTIVE = 480, V_FRONT = 10, V_SYNC = 2,  V_BACK = 33;
static constexpr int H_TOTAL = H_ACTIVE + H_FRONT + H_SYNC + H_BACK;
static constexpr int V_TOTAL = V_ACTIVE + V_FRONT + V_SYNC + V_BACK;

static uint8_t fb[V_ACTIVE][H_ACTIVE][3];   // zero-initialized (static)

static void tick(Vvideo_pattern_test* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

int main() {
    Vvideo_pattern_test* d = new Vvideo_pattern_test;

    // Synchronous reset, held two cycles.
    d->i_rst = 1; d->i_clk = 0; d->eval();
    tick(d);
    tick(d);
    d->i_rst = 0;
    d->eval();

    long active_pixels = 0, nonblack = 0, blank_leak = 0;

    for (int n = 0; n < H_TOTAL * V_TOTAL; n++) {
        int x = d->o_x, y = d->o_y, de = d->o_de;
        int r = d->o_r, g = d->o_g, b = d->o_b;

        if (de) {
            active_pixels++;
            if (r || g || b) nonblack++;
            if (x < H_ACTIVE && y < V_ACTIVE) {
                fb[y][x][0] = (uint8_t)r;
                fb[y][x][1] = (uint8_t)g;
                fb[y][x][2] = (uint8_t)b;
            }
        } else if (r || g || b) {
            blank_leak++;   // pixel data leaked into blanking
        }

        tick(d);
    }

    // Write the active image as a binary PPM for visual inspection.
    const char* path = "build/video_pattern.ppm";
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", H_ACTIVE, V_ACTIVE);
        fwrite(fb, 1, sizeof(fb), f);
        fclose(f);
    } else {
        printf("  WARN: could not open %s for writing\n", path);
    }

    int errors = 0;
    if (blank_leak != 0) {
        errors++;
        printf("  FAIL: %ld blanking pixels are non-black (must be black)\n", blank_leak);
    }
    if (active_pixels != (long)H_ACTIVE * V_ACTIVE) {
        errors++;
        printf("  FAIL: active pixel count %ld, expected %d\n",
               active_pixels, H_ACTIVE * V_ACTIVE);
    }
    if (nonblack == 0) {
        errors++;
        printf("  FAIL: active region is entirely black (no test pattern)\n");
    }

    printf("video_pattern: %ld active px, %ld non-black, %ld blanking-leak; wrote %s\n",
           active_pixels, nonblack, blank_leak, path);
    if (errors == 0)
        printf("  PASS: pattern renders, blanking is clean\n");

    delete d;
    return errors ? 1 : 0;
}
