// Verilator testbench for bram_mem — the streaming registered-read BRAM model
// the gen2 front end fetches from.
//
// Checks the three properties the IF path and the future D-side rely on:
//   - a 1-cycle registered read (word appears the cycle after the address),
//   - byte-enabled writes with no read/write-through,
//   - the read clock-enable holding the output while a consumer stalls (the
//     property that keeps a fetched word aligned with its PC under back-pressure).

#include <cstdio>
#include <cstdint>
#include "Vbram_mem.h"

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vbram_mem* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

int main() {
    Vbram_mem* dut = new Vbram_mem;
    dut->i_en = 1; dut->i_we = 0; dut->i_byte_en = 0; dut->i_wdata = 0; dut->i_addr = 0;

    // ── Full-word write, then registered read-back ───────────────
    dut->i_addr = 0; dut->i_wdata = 0xDEADBEEF; dut->i_byte_en = 0xF; dut->i_we = 1;
    tick(dut);
    dut->i_we = 0; dut->i_addr = 0; dut->i_en = 1;   // drive read address
    tick(dut);                                        // o_rdata latches mem[0]
    check("read_word", dut->o_rdata, 0xDEADBEEF);

    // ── Byte-enabled write: overwrite only byte 0 ────────────────
    dut->i_addr = 4; dut->i_wdata = 0x11223344; dut->i_byte_en = 0xF; dut->i_we = 1; tick(dut);
    dut->i_addr = 4; dut->i_wdata = 0x000000AA; dut->i_byte_en = 0x1; dut->i_we = 1; tick(dut);
    dut->i_we = 0; dut->i_addr = 4; tick(dut);
    check("byte_en_write", dut->o_rdata, 0x112233AA);

    // ── Read clock-enable holds the output under a stall ─────────
    dut->i_addr = 0; dut->i_en = 1; dut->i_we = 0; tick(dut);
    check("ce_pre", dut->o_rdata, 0xDEADBEEF);
    dut->i_addr = 4; dut->i_en = 0; tick(dut);        // address moves, read disabled
    check("ce_hold", dut->o_rdata, 0xDEADBEEF);        // output unchanged
    dut->i_addr = 4; dut->i_en = 1; tick(dut);         // re-enable picks up mem[4]
    check("ce_resume", dut->o_rdata, 0x112233AA);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}
