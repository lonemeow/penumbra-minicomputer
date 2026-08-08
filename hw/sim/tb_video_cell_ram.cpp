// Verilator testbench for the display character/attribute RAM
// (hw/rtl/io/video/video_cell_ram.sv).
//
// Checks the module contract: port A (CPU domain) writes and reads
// back with one-cycle registered latency; port B (pixel domain)
// returns the same contents with one-cycle registered latency on its
// own clock; both read ports hold their output across address changes
// until the next edge. The clocks are ticked independently — the
// cross-port path is plain dual-port RAM semantics, not a
// synchronized handshake.

#include <cstdio>
#include <cstdint>
#include "Vvideo_cell_ram.h"

static int errors = 0, checks = 0;

#define CHECK(cond, fmt, ...) do {                              \
    checks++;                                                   \
    if (!(cond)) {                                              \
        errors++;                                               \
        if (errors <= 20) printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
    }                                                           \
} while (0)

static void tick_cpu(Vvideo_cell_ram* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void tick_pix(Vvideo_cell_ram* d) {
    d->i_pclk = 0; d->eval();
    d->i_pclk = 1; d->eval();
}

// Distinct per-address pattern covering both bytes.
static uint16_t pat(int a) { return (uint16_t)((a * 2654435761u) >> 8); }

int main() {
    Vvideo_cell_ram* d = new Vvideo_cell_ram;
    d->i_clk = 0; d->i_pclk = 0; d->i_we = 0; d->eval();

    // Fill through port A.
    for (int a = 0; a < 4096; a++) {
        d->i_we = 1;
        d->i_addr = a;
        d->i_wdata = pat(a);
        tick_cpu(d);
    }
    d->i_we = 0;

    // Port A readback, one-cycle registered.
    for (int a = 0; a < 4096; a++) {
        d->i_addr = a;
        tick_cpu(d);
        CHECK(d->o_rdata == pat(a),
              "port A addr %03x: got %04x, want %04x", a, d->o_rdata, pat(a));
    }

    // Port B readout on the pixel clock.
    for (int a = 0; a < 4096; a++) {
        d->i_scan_addr = a;
        tick_pix(d);
        CHECK(d->o_scan_data == pat(a),
              "port B addr %03x: got %04x, want %04x", a, d->o_scan_data, pat(a));
    }

    // Registered-read pin on both ports: an address change without a
    // clock edge must not disturb the held outputs.
    d->i_addr = 0x123; tick_cpu(d);
    d->i_scan_addr = 0x456; tick_pix(d);
    uint16_t held_a = d->o_rdata, held_b = d->o_scan_data;
    d->i_addr = 0x321; d->i_scan_addr = 0x654; d->eval();
    CHECK(d->o_rdata == held_a, "port A output changed combinationally");
    CHECK(d->o_scan_data == held_b, "port B output changed combinationally");

    // A port-A write becomes visible to port B on B's next read.
    d->i_we = 1; d->i_addr = 0x200; d->i_wdata = 0xBEEF; tick_cpu(d);
    d->i_we = 0;
    d->i_scan_addr = 0x200; tick_pix(d);
    CHECK(d->o_scan_data == 0xBEEF, "cross-port write not visible: %04x",
          d->o_scan_data);

    printf("video_cell_ram: %d checks, %d errors\n", checks, errors);
    return errors != 0;
}
