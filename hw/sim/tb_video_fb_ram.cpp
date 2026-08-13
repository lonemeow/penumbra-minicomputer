// Verilator testbench for the framebuffer RAM
// (hw/rtl/io/video/video_fb_ram.sv).
//
// Checks the module contract: port A (CPU domain) commits exactly the
// byte lanes its byte enables select and leaves the others standing,
// and reads back with one-cycle registered latency; port B (pixel
// domain) returns the same words with one-cycle registered latency on
// its own clock. The clocks are ticked independently — the cross-port
// path is plain dual-port RAM semantics, not a synchronized handshake.

#include <cstdio>
#include <cstdint>
#include "Vvideo_fb_ram.h"

// Matches the module's default depth: 320x240 pixels packed four per
// word.
static const int WORDS = 19200;

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

static void tick_cpu(Vvideo_fb_ram* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void tick_pix(Vvideo_fb_ram* d) {
    d->i_pclk = 0; d->eval();
    d->i_pclk = 1; d->eval();
}

// Distinct per-address pattern covering all four lanes.
static uint32_t pat(int a) { return (uint32_t)(a * 2654435761u) ^ 0xA5A5A5A5u; }

// Host-side model of what the RAM should hold.
static uint32_t model[WORDS];

static void wr(Vvideo_fb_ram* d, int a, uint32_t data, int be) {
    d->i_we = 1;
    d->i_byte_en = be;
    d->i_addr = a;
    d->i_wdata = data;
    tick_cpu(d);
    d->i_we = 0;
    for (int b = 0; b < 4; b++)
        if (be & (1 << b)) {
            uint32_t lane = 0xFFu << (8 * b);
            model[a] = (model[a] & ~lane) | (data & lane);
        }
}

int main() {
    Vvideo_fb_ram* d = new Vvideo_fb_ram;
    d->i_clk = 0; d->i_pclk = 0; d->i_we = 0; d->i_byte_en = 0; d->eval();

    // Full-word fill, then port A readback.
    for (int a = 0; a < WORDS; a++)
        wr(d, a, pat(a), 0xF);

    for (int a = 0; a < WORDS; a++) {
        d->i_addr = a;
        tick_cpu(d);
        CHECK(d->o_rdata == model[a],
              "port A addr %05x: got %08x, want %08x",
              a, d->o_rdata, model[a]);
    }

    // Port B sees the same contents on its own clock.
    for (int a = 0; a < WORDS; a++) {
        d->i_scan_addr = a;
        tick_pix(d);
        CHECK(d->o_scan_data == model[a],
              "port B addr %05x: got %08x, want %08x",
              a, d->o_scan_data, model[a]);
    }

    // Sub-word writes: every byte-enable combination against a known
    // background, checking both that enabled lanes take the new data
    // and that disabled lanes keep the old.
    for (int be = 0; be < 16; be++) {
        int a = be * 97;                      // spread across the array
        wr(d, a, 0x00000000u, 0xF);           // known background
        wr(d, a, 0xDEADBEEFu, be);

        d->i_addr = a;
        tick_cpu(d);
        CHECK(d->o_rdata == model[a],
              "byte_en %x at %05x: got %08x, want %08x",
              be, a, d->o_rdata, model[a]);

        d->i_scan_addr = a;
        tick_pix(d);
        CHECK(d->o_scan_data == model[a],
              "byte_en %x at %05x via port B: got %08x, want %08x",
              be, a, d->o_scan_data, model[a]);
    }

    // A write with no lanes enabled must not disturb the word, even
    // though i_we is asserted — the aperture sees these whenever a
    // masked store reaches it.
    {
        int a = 1234;
        wr(d, a, 0x11223344u, 0xF);
        wr(d, a, 0xFFFFFFFFu, 0x0);
        d->i_addr = a;
        tick_cpu(d);
        CHECK(d->o_rdata == 0x11223344u,
              "empty byte_en at %05x: got %08x, want %08x",
              a, d->o_rdata, 0x11223344u);
    }

    // Read output holds across an address change until the next edge.
    {
        d->i_addr = 42;
        tick_cpu(d);
        uint32_t held = d->o_rdata;
        d->i_addr = 43;
        d->eval();
        CHECK(d->o_rdata == held,
              "port A output moved without a clock edge: %08x != %08x",
              d->o_rdata, held);
    }

    // Both ports at one address: the write lands, and the pixel side
    // reads the committed value on its next edge. No handshake, but
    // also no lost write.
    {
        int a = 777;
        wr(d, a, 0xCAFEBABEu, 0xF);
        d->i_scan_addr = a;
        tick_pix(d);
        CHECK(d->o_scan_data == 0xCAFEBABEu,
              "cross-port at %05x: got %08x, want %08x",
              a, d->o_scan_data, 0xCAFEBABEu);
    }

    printf("video_fb_ram: %d checks, %d errors\n", checks, errors);
    delete d;
    return errors ? 1 : 0;
}
