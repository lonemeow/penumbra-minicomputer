// Verilator testbench for the SDRAM controller at an eight-beat burst.
//
// Exercises sdram_bl8_test (sdram_ctrl at BURST_LEN=8 + sdram_phy_sim
// + sdram_model).  Covers what the longer burst introduces rather than
// repeating tb_sdram_test's single-word suite:
//   • Ordering: one read returns the group's four words, word 0 in the
//     low bits.
//   • Alignment: a read from any word in the group returns the same
//     group, since the controller aligns the burst down to its base.
//   • Writes: still one word wide, and a write to one group leaves its
//     neighbour untouched.
//   • Byte-enables: sub-word writes reach the right lanes of the right
//     word of the group.
//
// Ends with non-zero exit code on any failure.

#include <cstdio>
#include <cstdint>
#include "Vsdram_bl8_test.h"

// Bytes covered by one burst: 8 beats x 2 bytes.
static const uint32_t GROUP_BYTES = 16;
static const int      GROUP_WORDS = 4;

static int errors = 0;

static void tick(Vsdram_bl8_test* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vsdram_bl8_test* d) {
    d->i_rst = 1;
    d->i_req_valid = 0;
    d->i_req_we = 0;
    d->i_req_addr = 0;
    d->i_req_wdata = 0;
    d->i_req_byte_en = 0xF;
    d->i_rsp_ready = 1;
    tick(d); tick(d);
    d->i_rst = 0;
}

static void wait_init(Vsdram_bl8_test* d) {
    int cycles = 0;
    while (!d->o_init_done && cycles < 1000) {
        tick(d);
        cycles++;
    }
    if (!d->o_init_done) {
        printf("  FAIL: init did not complete within 1000 cycles\n");
        errors++;
    }
}

// Drive one request to acceptance, then run to completion.  Reads leave
// the group in o_rsp_data; writes just complete.
static bool issue(Vsdram_bl8_test* d, uint32_t addr, bool we,
                  uint32_t wdata, uint8_t byte_en, const char* label) {
    d->i_req_valid   = 1;
    d->i_req_we      = we ? 1 : 0;
    d->i_req_addr    = addr;
    d->i_req_wdata   = wdata;
    d->i_req_byte_en = byte_en;

    int cyc = 0;
    while (!d->o_req_ready) {
        tick(d);
        if (++cyc > 200) {
            printf("  FAIL: %s addr=0x%08X — no req_ready within 200 cyc\n", label, addr);
            errors++;
            d->i_req_valid = 0;
            return false;
        }
    }
    tick(d);                 // consume the accepting edge
    d->i_req_valid = 0;

    cyc = 0;
    while (!d->o_done) {
        tick(d);
        if (++cyc > 200) {
            printf("  FAIL: %s addr=0x%08X — no done within 200 cyc\n", label, addr);
            errors++;
            return false;
        }
    }
    if (!we && !d->o_rsp_valid) {
        printf("  FAIL: %s addr=0x%08X — done without rsp_valid\n", label, addr);
        errors++;
        return false;
    }
    return true;
}

static void write_word(Vsdram_bl8_test* d, uint32_t addr, uint32_t val,
                       uint8_t byte_en = 0xF) {
    issue(d, addr, true, val, byte_en, "WR");
    tick(d);
}

// Reads the group containing `addr` into `out[GROUP_WORDS]`.
static void read_group(Vsdram_bl8_test* d, uint32_t addr, uint32_t* out) {
    for (int w = 0; w < GROUP_WORDS; w++) out[w] = 0;
    if (!issue(d, addr, false, 0, 0xF, "RD")) return;
    for (int w = 0; w < GROUP_WORDS; w++) out[w] = d->o_rsp_data[w];
    tick(d);
}

static void check(uint32_t got, uint32_t want, const char* label) {
    if (got != want) {
        printf("  FAIL: %s — got 0x%08X want 0x%08X\n", label, got, want);
        errors++;
    } else {
        printf("  PASS: %s\n", label);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Vsdram_bl8_test* d = new Vsdram_bl8_test;

    printf("── SDRAM controller, eight-beat burst ──\n");

    reset(d);
    wait_init(d);
    if (errors) goto done;
    printf("  PASS: init completed\n");

    // ── Ordering: the group reads back in address order ──────
    {
        const uint32_t base = 0x00001000;
        for (int w = 0; w < GROUP_WORDS; w++)
            write_word(d, base + 4 * w, 0xA5A50000u + w);

        uint32_t got[GROUP_WORDS];
        read_group(d, base, got);
        for (int w = 0; w < GROUP_WORDS; w++) {
            char label[64];
            snprintf(label, sizeof(label), "group word %d in order", w);
            check(got[w], 0xA5A50000u + w, label);
        }
    }

    // ── Alignment: any word in the group returns the group ───
    {
        const uint32_t base = 0x00001000;
        for (uint32_t off = 4; off < GROUP_BYTES; off += 4) {
            uint32_t got[GROUP_WORDS];
            read_group(d, base + off, got);
            char label[64];
            snprintf(label, sizeof(label), "read at +%u aligns to group base", off);
            check(got[0], 0xA5A50000u, label);
        }
    }

    // ── A write reaches one word, not the group ──────────────
    {
        const uint32_t base = 0x00001000;
        write_word(d, base + 8, 0xDEADBEEF);

        uint32_t got[GROUP_WORDS];
        read_group(d, base, got);
        check(got[0], 0xA5A50000u, "word 0 untouched by word 2 write");
        check(got[1], 0xA5A50001u, "word 1 untouched by word 2 write");
        check(got[2], 0xDEADBEEF,  "word 2 written");
        check(got[3], 0xA5A50003u, "word 3 untouched by word 2 write");
    }

    // ── Byte-enables land in the right lane of the right word ─
    {
        const uint32_t base = 0x00002000;
        for (int w = 0; w < GROUP_WORDS; w++)
            write_word(d, base + 4 * w, 0x11111111u * (w + 1));

        write_word(d, base + 4, 0x000000CCu, 0x1);   // low byte of word 1
        write_word(d, base + 12, 0x00DD0000u, 0x4);  // byte 2 of word 3

        uint32_t got[GROUP_WORDS];
        read_group(d, base, got);
        check(got[0], 0x11111111u, "byte_en: word 0 intact");
        check(got[1], 0x222222CCu, "byte_en: word 1 low byte only");
        check(got[2], 0x33333333u, "byte_en: word 2 intact");
        check(got[3], 0x44DD4444u, "byte_en: word 3 byte 2 only");
    }

    // ── A neighbouring group is a separate burst ─────────────
    {
        uint32_t got[GROUP_WORDS];
        read_group(d, 0x00001000, got);
        check(got[0], 0xA5A50000u, "first group survives the second");
        read_group(d, 0x00002000, got);
        check(got[0], 0x11111111u, "second group reads independently");
    }

    if (d->o_protocol_errors != 0) {
        printf("  FAIL: model reported %u protocol violation(s)\n", d->o_protocol_errors);
        errors++;
    } else {
        printf("  PASS: no protocol violations\n");
    }

done:
    delete d;
    if (errors == 0) {
        printf("── ALL PASS ──\n");
        return 0;
    } else {
        printf("── %d FAILURE(S) ──\n", errors);
        return 1;
    }
}
