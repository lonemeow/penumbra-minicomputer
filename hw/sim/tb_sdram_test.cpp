// Verilator testbench for SDRAM controller + behavioral model.
//
// Exercises sdram_test (sdram_ctrl + sdram_phy_sim + sdram_model):
//   • Init: confirm o_init_done asserts within bounded cycles.
//   • Round-trip: write a value, read it back.
//   • Multi-address: 4 sequential addresses with distinct values.
//   • Byte-enables: STB-style sub-word writes leave neighbours intact.
//   • Refresh: run idle long enough for >1 refresh interval; ensure
//     no protocol violations and that data survives.
//
// Ends with non-zero exit code on any failure.

#include <cstdio>
#include <cstdint>
#include "Vsdram_test.h"

static int errors = 0;

static void tick(Vsdram_test* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vsdram_test* d) {
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

static void wait_init(Vsdram_test* d) {
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

// Issue one transaction and (for reads) capture the response.
// Returns rdata for reads; ignored for writes.
static uint32_t do_req(Vsdram_test* d, uint32_t addr, bool we,
                       uint32_t wdata, uint8_t byte_en, const char* label) {
    d->i_req_valid   = 1;
    d->i_req_we      = we ? 1 : 0;
    d->i_req_addr    = addr;
    d->i_req_wdata   = wdata;
    d->i_req_byte_en = byte_en;

    // Wait for o_req_ready to pulse (controller accepted it).
    int cyc = 0;
    while (!d->o_req_ready) {
        tick(d);
        if (++cyc > 200) {
            printf("  FAIL: %s addr=0x%08X — no req_ready within 200 cyc\n", label, addr);
            errors++;
            d->i_req_valid = 0;
            return 0;
        }
    }
    tick(d);                      // consume the accept cycle
    d->i_req_valid = 0;

    if (we) {
        // Wait for the FSM to finish RECOVER and return to IDLE.
        for (int i = 0; i < 30; i++) tick(d);
        return 0;
    }

    // Read: poll for o_rsp_valid pulse, capture data.
    cyc = 0;
    while (!d->o_rsp_valid) {
        tick(d);
        if (++cyc > 200) {
            printf("  FAIL: %s addr=0x%08X — no rsp_valid within 200 cyc\n", label, addr);
            errors++;
            return 0;
        }
    }
    uint32_t rd = d->o_rsp_data;
    tick(d);                      // let recovery proceed
    return rd;
}

static void check(uint32_t got, uint32_t want, const char* label) {
    if (got != want) {
        printf("  FAIL: %s — got 0x%08X want 0x%08X\n", label, got, want);
        errors++;
    } else {
        printf("  PASS: %s — 0x%08X\n", label, got);
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Vsdram_test* d = new Vsdram_test;

    printf("── SDRAM controller unit test ──\n");

    reset(d);
    wait_init(d);
    if (!d->o_init_done) goto done;
    printf("  init complete\n");

    // ── Test 1: round-trip ───────────────────────────────────
    do_req(d, 0x00001000, true,  0xDEADBEEF, 0xF, "WR  0x1000 = 0xDEADBEEF");
    {
        uint32_t r = do_req(d, 0x00001000, false, 0, 0xF, "RD  0x1000");
        check(r, 0xDEADBEEF, "round-trip 0x1000");
    }

    // ── Test 2: overwrite (different value, same address) ────
    do_req(d, 0x00001000, true,  0x55AA55AA, 0xF, "WR  0x1000 = 0x55AA55AA");
    {
        uint32_t r = do_req(d, 0x00001000, false, 0, 0xF, "RD  0x1000");
        check(r, 0x55AA55AA, "overwrite 0x1000");
    }

    // ── Test 3: 4 distinct addresses, distinct values ────────
    do_req(d, 0x00002000, true, 0xC0DE0000, 0xF, "WR  0x2000");
    do_req(d, 0x00002004, true, 0xC0DE0001, 0xF, "WR  0x2004");
    do_req(d, 0x00002008, true, 0xC0DE0002, 0xF, "WR  0x2008");
    do_req(d, 0x0000200C, true, 0xC0DE0003, 0xF, "WR  0x200C");
    check(do_req(d, 0x00002000, false, 0, 0xF, "RD  0x2000"), 0xC0DE0000, "seq[0]");
    check(do_req(d, 0x00002004, false, 0, 0xF, "RD  0x2004"), 0xC0DE0001, "seq[1]");
    check(do_req(d, 0x00002008, false, 0, 0xF, "RD  0x2008"), 0xC0DE0002, "seq[2]");
    check(do_req(d, 0x0000200C, false, 0, 0xF, "RD  0x200C"), 0xC0DE0003, "seq[3]");

    // ── Test 4: byte enables (sub-word integrity) ────────────
    do_req(d, 0x00003000, true, 0xCAFEFACE, 0xF, "WR  0x3000 = 0xCAFEFACE (full)");
    do_req(d, 0x00003000, true, 0xFFFF22FF, 0x2, "WR  0x3000 byte[1]=0x22");
    check(do_req(d, 0x00003000, false, 0, 0xF, "RD  0x3000"), 0xCAFE22CE, "byte_en single byte");

    do_req(d, 0x00003000, true, 0xFFFF1111, 0x3, "WR  0x3000 halfword[0]=0x1111");
    check(do_req(d, 0x00003000, false, 0, 0xF, "RD  0x3000"), 0xCAFE1111, "byte_en low halfword");

    // Sub-word access at non-zero address[1:0] — STB-style write at
    // byte offset 2 of the 32-bit word (address bit 1 set).  This
    // catches the column-alignment bug where addr[1] would otherwise
    // shift the BL=2 burst into the next word.  Pre-fill the next
    // word with a sentinel so any spillover is visible.
    do_req(d, 0x00004000, true, 0xFFFFFFFF, 0xF, "WR  0x4000 = 0xFFFFFFFF (full)");
    do_req(d, 0x00004004, true, 0xCAFEBABE, 0xF, "WR  0x4004 = 0xCAFEBABE (sentinel next word)");
    do_req(d, 0x00004002, true, 0xABABABAB, 0x4, "WR  0x4002 byte[2]=0xAB (sub-word, addr[1]=1)");
    check(do_req(d, 0x00004000, false, 0, 0xF, "RD  0x4000"), 0xFFABFFFF, "sub-word offset 2");
    check(do_req(d, 0x00004004, false, 0, 0xF, "RD  0x4004"), 0xCAFEBABE, "next word untouched");

    // ── Test 5: refresh tolerance ────────────────────────────
    // Run idle long enough that several refresh intervals elapse.
    // T_REFI=750 cycles in the preset; do 3000 cycles of idle.
    for (int i = 0; i < 3000; i++) tick(d);

    // Re-read prior addresses to ensure refresh did not corrupt state.
    check(do_req(d, 0x00001000, false, 0, 0xF, "RD  0x1000 after refresh"), 0x55AA55AA, "post-refresh 0x1000");
    check(do_req(d, 0x00002000, false, 0, 0xF, "RD  0x2000 after refresh"), 0xC0DE0000, "post-refresh 0x2000");
    check(do_req(d, 0x00003000, false, 0, 0xF, "RD  0x3000 after refresh"), 0xCAFE1111, "post-refresh 0x3000");

    // ── Protocol violations from the model ───────────────────
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
