/*
 * entry.c — bare-metal exercise for the display framebuffer.
 *
 * Checks the CLASS_DISPLAY framebuffer capability on real hardware,
 * where the paths simulation cannot reach are the interesting ones:
 * the CPU generating byte enables to a device (every other device on
 * this bus is word-strided, so nothing exercised that before), the
 * uncached MMIO store path at aperture depth, and the picture itself.
 *
 * Two halves. The first is machine-checkable and reports over the
 * UART: discovery, the declared geometry and format, and readback of
 * the pixel and palette apertures at word, halfword and byte width.
 * The second is a test pattern for a human — a wrong result should be
 * visibly wrong on the monitor rather than subtly off.
 *
 * The picture is left on screen when the test finishes, so there is
 * something to look at; a reset returns the character console.
 */

#include <stdint.h>
#include "bench.h"

/* ── Bootdata layout (mirror of hw/rom/bootdata.h) ────────────── */
#define BOOTDATA_BASE       0x00000040
#define BTAG_END            0
#define BTAG_DEVICE         2
#define ACFG_CLASS_DISPLAY  6     /* matches penumbra_pkg::ACFG_CLASS_DISPLAY */

struct btag_hdr {
    uint32_t type;
    uint32_t size;
};

struct btag_device {
    struct btag_hdr hdr;
    uint32_t cls;
    uint32_t base;
    uint32_t dev_size;
    uint32_t id;
    char     name[16];
};

/* ── Device registers (doc/system/devices/display.md) ─────────── */
#define PD_CAP          0x000
#define PD_INFO         0x004
#define PD_CTRL         0x008
#define PD_FB_GEOM      0x01C
#define PD_FB_FORMAT    0x020
#define PD_FB_PALETTE   0x10000
#define PD_FB           0x20000

#define PD_CAP_FRAMEBUFFER  (1u << 13)
#define PD_CTRL_ENABLE      (1u << 0)
#define PD_CTRL_FB_SEL      (1u << 4)

static uint32_t disp_base;
static uint32_t fb_width, fb_height;

#define REG(off)   (*(volatile uint32_t *)(disp_base + (off)))
#define FB_B(off)  (*(volatile uint8_t  *)(disp_base + PD_FB + (off)))
#define FB_H(off)  (*(volatile uint16_t *)(disp_base + PD_FB + (off)))
#define FB_W(off)  (*(volatile uint32_t *)(disp_base + PD_FB + (off)))
#define PAL(i)     (*(volatile uint32_t *)(disp_base + PD_FB_PALETTE + (i) * 4))

/* ── Reporting ────────────────────────────────────────────────── */
static int failures;

static void print_hex(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    bench_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        bench_putchar(digits[(v >> i) & 0xF]);
}

static void check(int ok, const char *what)
{
    bench_puts(ok ? "  PASS  " : "  FAIL  ");
    bench_puts(what);
    bench_puts("\r\n");
    if (!ok)
        failures++;
}

static void check_eq(uint32_t got, uint32_t want, const char *what)
{
    if (got == want) {
        check(1, what);
        return;
    }
    bench_puts("  FAIL  ");
    bench_puts(what);
    bench_puts(" — got ");
    print_hex(got);
    bench_puts(", want ");
    print_hex(want);
    bench_puts("\r\n");
    failures++;
}

/* ── Discovery ────────────────────────────────────────────────── */
static const struct btag_device *find_display(uint32_t bootdata)
{
    /* Past the bootdata header: magic, version, total_size. */
    uint32_t p = (bootdata ? bootdata : BOOTDATA_BASE) + 12;

    for (;;) {
        const struct btag_hdr *h = (const struct btag_hdr *)p;
        if (h->type == BTAG_END)
            return 0;
        /* An entry that does not advance the cursor would spin here
         * forever, so treat a malformed list as no device rather than
         * a hang with nothing on the console to say why. */
        if (h->size < sizeof(struct btag_hdr))
            return 0;
        if (h->type == BTAG_DEVICE) {
            const struct btag_device *d = (const struct btag_device *)p;
            if (d->cls == ACFG_CLASS_DISPLAY)
                return d;
        }
        p += h->size;
    }
}

/* ── Aperture checks ──────────────────────────────────────────── */
/*
 * The pixel aperture is the class's one departure from word-strided
 * access: it behaves as plain memory so the OS can map it into a
 * rendering process. That makes sub-word stores the path worth
 * proving — a byte store must move exactly one byte and leave its
 * neighbours in the same word alone.
 */
static void check_apertures(void)
{
    uint32_t last = fb_width * fb_height - 4;   /* last whole word */

    FB_W(0) = 0x11223344u;
    check_eq(FB_W(0), 0x11223344u, "pixel aperture word readback");

    FB_W(last) = 0xAABBCCDDu;
    check_eq(FB_W(last), 0xAABBCCDDu, "pixel aperture last word readback");
    check_eq(FB_W(0), 0x11223344u, "first word survives a write to the last");

    FB_W(64) = 0x00000000u;
    FB_B(65) = 0x5Au;
    check_eq(FB_W(64), 0x00005A00u, "byte store lands in its own lane only");
    FB_B(67) = 0xA5u;
    check_eq(FB_W(64), 0xA5005A00u, "second byte store leaves the first");

    FB_W(72) = 0xFFFFFFFFu;
    FB_H(74) = 0x1234u;
    check_eq(FB_W(72), 0x1234FFFFu, "halfword store covers two lanes");

    /* Every byte of a word, one at a time, from a known background. */
    FB_W(80) = 0x00000000u;
    for (int i = 0; i < 4; i++)
        FB_B(80 + i) = (uint8_t)(0xE0 + i);
    check_eq(FB_W(80), 0xE3E2E1E0u, "all four lanes addressable");

    /* Readback at each width agrees with what the word holds. */
    check_eq(FB_B(81), 0xE1u, "byte read selects its lane");
    check_eq(FB_H(82), 0xE3E2u, "halfword read selects its pair");

    /* The palette keeps 24 bits and drops the aperture's unused byte. */
    PAL(7) = 0xFF884422u;
    check_eq(PAL(7), 0x00884422u, "palette slot keeps 24 bits");
    PAL(255) = 0x00FFFFFFu;
    check_eq(PAL(255), 0x00FFFFFFu, "last palette slot reachable");
}

/* ── Palette ──────────────────────────────────────────────────── */
/*
 * Fixed entries the pattern draws with, then a ramp filling the rest
 * so a gradient can show that every index reaches a distinct color.
 */
#define IDX_BLACK   0
#define IDX_WHITE   1
#define IDX_RED     2
#define IDX_GREEN   3
#define IDX_BLUE    4
#define IDX_GREY    5

/* The ramp's bounds are indices, like every IDX_ above, so a wrap
 * reads `idx > IDX_RAMP_LAST`. How many entries that spans is a
 * separate quantity and carries no IDX_ prefix, because using a count
 * where an index belongs is exactly the mistake the two names exist
 * to prevent. */
#define IDX_RAMP_FIRST  64
#define IDX_RAMP_LAST   255
#define RAMP_COUNT      (IDX_RAMP_LAST - IDX_RAMP_FIRST + 1)

static void load_palette(void)
{
    PAL(IDX_BLACK) = 0x00000000u;
    PAL(IDX_WHITE) = 0x00FFFFFFu;
    PAL(IDX_RED)   = 0x00FF0000u;
    PAL(IDX_GREEN) = 0x0000FF00u;
    PAL(IDX_BLUE)  = 0x000000FFu;
    PAL(IDX_GREY)  = 0x00404040u;

    for (int i = 6; i < IDX_RAMP_FIRST; i++)
        PAL(i) = 0x00000000u;

    /* A hue sweep, so neighbouring ramp entries are clearly distinct. */
    for (int i = IDX_RAMP_FIRST; i <= IDX_RAMP_LAST; i++) {
        uint32_t t = (uint32_t)(i - IDX_RAMP_FIRST) * 255u / (RAMP_COUNT - 1);
        PAL(i) = (t << 16) | ((255u - t) << 8) | ((t * 2u) & 0xFFu);
    }
}

/* ── Drawing primitives ───────────────────────────────────────── */
/*
 * Everything here works from a row pointer rather than addressing the
 * aperture per pixel. Per-pixel addressing costs a reload of the
 * device base (the volatile store is opaque, so the compiler cannot
 * keep a global in a register across it) plus a y * fb_width multiply
 * — and fb_width is a runtime value, so that multiply is a real MUL
 * into the divmul unit, tens of cycles for a single byte written.
 * Hoisting it to once per row is the difference between a pattern
 * that draws instantly and one that visibly crawls.
 */
static volatile uint8_t *fb_row(uint32_t y)
{
    return (volatile uint8_t *)(disp_base + PD_FB) + y * fb_width;
}

static void fb_pixel(uint32_t x, uint32_t y, uint8_t idx)
{
    if (x < fb_width && y < fb_height)
        fb_row(y)[x] = idx;
}

/* A run of one color along a row, clipped to the picture. */
static void fb_span(uint32_t x, uint32_t y, uint32_t len, uint8_t idx)
{
    if (y >= fb_height || x >= fb_width)
        return;
    if (len > fb_width - x)
        len = fb_width - x;

    volatile uint8_t *p = fb_row(y) + x;
    while (len--)
        *p++ = idx;
}

/*
 * Explicitly unrolled, against a hoisted pointer. Written the obvious
 * way — a rolled loop over FB_W(off) — the compiler reloads the device
 * base from memory and rematerializes the aperture offset on every
 * iteration, spending twelve instructions per four-byte store. A fill
 * built that way measures the loop rather than the aperture, and no
 * real rendering code would write a frame that way either.
 */
static void fb_fill(uint8_t idx)
{
    uint32_t word = (uint32_t)idx * 0x01010101u;
    uint32_t words = fb_width * fb_height / 4;
    volatile uint32_t *p = (volatile uint32_t *)(disp_base + PD_FB);

    while (words >= 8) {
        p[0] = word; p[1] = word; p[2] = word; p[3] = word;
        p[4] = word; p[5] = word; p[6] = word; p[7] = word;
        p += 8;
        words -= 8;
    }
    while (words--)
        *p++ = word;
}

static void fb_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                    uint8_t idx)
{
    for (uint32_t y = 0; y < h; y++)
        fb_span(x0, y0 + y, w, idx);
}

/*
 * The half a human judges. check_apertures() has already proven the
 * device at the bus; what is left is whether the picture maps onto the
 * screen the way the contract says, and the pattern's job is to make a
 * wrong answer obviously wrong rather than subtly off.
 *
 * Each element is aimed at a failure that would otherwise pass
 * unnoticed:
 *   - the ring on the outermost pixels shows the full extent reaching
 *     the glass, with nothing cropped or wrapped at an edge;
 *   - the checkerboard's quadrant identifies the origin: a flipped or
 *     transposed picture puts it somewhere else, and a byte order
 *     reversed within each word breaks its lattice;
 *   - its one-pixel features show the doubling is exactly 2x, which
 *     nothing larger can — a coarser feature looks the same whichever
 *     scale it is drawn at;
 *   - a wrong line stride shears that lattice into diagonals and
 *     disturbs the ramp's banding;
 *   - the ramp covers every palette entry, so a dead or aliased index
 *     shows as a break in the sweep.
 */
static void draw_test_pattern(void)
{
    uint32_t half_width = fb_width / 2;
    uint32_t half_height = fb_height / 2;

    fb_fill(IDX_WHITE);

    /* Top left: a one-pixel checkerboard. Correct scan-out renders it
     * as 2x2 blocks; any other scale is obvious, and a wrong line
     * stride shears the lattice into diagonal streaks. */
    for (uint32_t y = 1; y < half_height; y++) {
        volatile uint8_t *row = fb_row(y);
        for (uint32_t x = 1; x < half_width; x++)
            if ((x & 1) ^ (y & 1))
                row[x] = IDX_GREY;
    }

    /* Top right: the ramp, walked down each column in turn, so every
     * palette entry appears and the banding runs diagonally — a
     * stride error breaks the diagonal's regularity. This is also the
     * range animate_palette() rotates, so this quadrant is what
     * sweeps. */
    /* Walked row by row rather than column by column, which is the
     * same picture: stepping one column advances the entry by a whole
     * column's worth, so each row starts where its own column walk
     * would have and adds that stride across. Iterating columns
     * directly would put a row multiply — a real divmul MUL, since
     * fb_width is a runtime value — on every single pixel. */
    uint32_t stride = half_height % RAMP_COUNT;
    for (uint32_t y = 0; y < half_height; y++) {
        volatile uint8_t *row = fb_row(y);
        uint32_t idx = IDX_RAMP_FIRST + y % RAMP_COUNT;
        for (uint32_t x = half_width; x < fb_width; x++) {
            row[x] = (uint8_t)idx;
            idx += stride;
            if (idx > IDX_RAMP_LAST)
                idx -= RAMP_COUNT;
        }
    }

    /* A one-pixel ring on the outermost pixels, drawn last so the
     * quadrants above cannot eat an edge of it: if any side is cropped
     * or wrapped away, that side of the ring is missing. It contrasts
     * with the field rather than matching it — a white ring on a white
     * field is the one border that cannot be seen. */
    fb_span(0, 0, fb_width, IDX_RED);
    fb_span(0, fb_height - 1, fb_width, IDX_RED);
    for (uint32_t y = 0; y < fb_height; y++) {
        volatile uint8_t *row = fb_row(y);
        row[0] = IDX_RED;
        row[fb_width - 1] = IDX_RED;
    }
}

/* ── Throughput ───────────────────────────────────────────────── */
/*
 * Full-frame fill rate through the uncached aperture — the ceiling on
 * anything that redraws the whole picture, and the number that decides
 * whether a demo can animate at all.
 */
static void measure_fill_rate(void)
{
    const int frames = 8;
    uint32_t bytes = fb_width * fb_height;

    bench_timer_start();
    for (int i = 0; i < frames; i++)
        fb_fill((uint8_t)(IDX_RAMP_FIRST + i));
    uint32_t us = bench_timer_elapsed_us();

    /* Scale to KB before the rate so the arithmetic stays 32-bit —
     * bytes-per-second across a frame overflows, and a 64-bit divide
     * would pull in a libcall this runtime does not carry. */
    uint32_t kb_moved = bytes / 1024u * (uint32_t)frames;

    bench_puts("  full-frame fill: ");
    bench_print_uint(bytes);
    bench_puts(" bytes in ");
    bench_print_uint(us / (uint32_t)frames);
    bench_puts(" us/frame (");
    if (us > 0)
        bench_print_uint(kb_moved * 1000000u / us);
    bench_puts(" KB/s)\r\n");
}

/* ── Palette animation ────────────────────────────────────────── */
/*
 * Rotating the ramp under a still picture: writes reach scan-out on
 * the next read whether the framebuffer is selected or not, so the
 * gradient should sweep without the pixels being touched again.
 */
/* Long enough to read as motion on glass, short enough that the whole
 * test stays practical under simulation, where this loop is the only
 * part that costs real time. */
#define FB_ANIM_STEP_US  40000u

static void animate_palette(int rounds)
{
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < RAMP_COUNT; i++) {
            uint32_t t = (uint32_t)((i + r * 8) % RAMP_COUNT) * 255u
                         / (RAMP_COUNT - 1);
            PAL(IDX_RAMP_FIRST + i) =
                (t << 16) | ((255u - t) << 8) | ((t * 2u) & 0xFFu);
        }
        bench_timer_start();
        while (bench_timer_elapsed_us() < FB_ANIM_STEP_US)
            ;
    }
}

/* ── Entry ────────────────────────────────────────────────────── */
void bench_main(uint32_t bootdata)
{
    bench_init();
    bench_puts("\r\n=== Penumbra framebuffer test ===\r\n");

    const struct btag_device *d = find_display(bootdata);
    if (d == 0) {
        bench_puts("  no CLASS_DISPLAY device in the boot data\r\n");
        return;
    }
    disp_base = d->base;
    bench_puts("  display at ");
    print_hex(disp_base);
    bench_puts(", window ");
    bench_print_uint(d->dev_size);
    bench_puts(" bytes\r\n");

    uint32_t cap = REG(PD_CAP);
    uint32_t geom = REG(PD_FB_GEOM);
    uint32_t fmt = REG(PD_FB_FORMAT);

    bench_puts("  CAP ");
    print_hex(cap);
    bench_puts("  FB_GEOM ");
    print_hex(geom);
    bench_puts("  FB_FORMAT ");
    print_hex(fmt);
    bench_puts("\r\n");

    if (!(cap & PD_CAP_FRAMEBUFFER)) {
        bench_puts("  device declares no framebuffer — nothing to test\r\n");
        return;
    }
    fb_width  = geom & 0xFFFF;
    fb_height = (geom >> 16) & 0xFFFF;
    check((fmt & 0xFF) == 8, "declared format is 8bpp indexed");
    check(fb_width > 0 && fb_height > 0, "declared geometry is non-zero");
    if (failures)
        return;

    bench_puts("  geometry ");
    bench_print_uint(fb_width);
    bench_puts("x");
    bench_print_uint(fb_height);
    bench_puts("\r\n");

    check_apertures();
    measure_fill_rate();

    load_palette();
    draw_test_pattern();
    REG(PD_CTRL) = PD_CTRL_ENABLE | PD_CTRL_FB_SEL;

    bench_puts(failures ? "\r\n  APERTURE CHECKS FAILED\r\n"
                        : "\r\n  aperture checks passed\r\n");
    bench_puts("  picture is on the monitor; the gradient sweeps for a"
               " few seconds\r\n");

    animate_palette(12);

    bench_puts("  done — reset to return the character console\r\n");
}
