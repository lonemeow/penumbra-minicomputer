// tb_ecp5_pll_test — verify ecp5_pll_compute's divider search.
//
// The 25 MHz config must reproduce the historical hand-picked literals
// {CLKI 1, CLKFB 1, CLKOP 24, CLKOS 6} (CPHASE 23). The 30 MHz config must be
// REJECTED (valid=0): its 6/5 ratio forces fPFD = 5 MHz, below PFD_MIN — the
// solver refuses it rather than emit an unlockable bitstream. The 37.5 MHz
// config is the next clock clearing the floor on the 600 MHz VCO:
// {2, 3, 16, 6} (CPHASE 15) at fPFD 12.5 MHz.
#include "Vecp5_pll_test.h"
#include "verilated.h"
#include <cstdio>

static int checks = 0, fails = 0;
static void chk(const char* name, long long got, long long exp) {
    checks++;
    if (got != exp) {
        fails++;
        printf("  FAIL %s: got %lld, expected %lld\n", name, got, exp);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vecp5_pll_test* d = new Vecp5_pll_test;
    d->eval();

    // 25 MHz CPU / 100 MHz SDRAM — reproduces the prior literals exactly.
    chk("25.valid",  d->o25_valid,  1);
    chk("25.clki",   d->o25_clki,   1);
    chk("25.clkfb",  d->o25_clkfb,  1);
    chk("25.clkop",  d->o25_clkop,  24);
    chk("25.clkos",  d->o25_clkos,  6);
    chk("25.cphase", d->o25_cphase, 23);

    // 30 MHz CPU — unlockable (fPFD 5 MHz < floor), must be refused.
    chk("30.rejected", d->o30_valid, 0);

    // 37.5 MHz CPU / 100 MHz SDRAM — next lockable step, VCO stays 600.
    chk("37.valid",  d->o37_valid,  1);
    chk("37.clki",   d->o37_clki,   2);
    chk("37.clkfb",  d->o37_clkfb,  3);
    chk("37.clkop",  d->o37_clkop,  16);
    chk("37.clkos",  d->o37_clkos,  6);
    chk("37.cphase", d->o37_cphase, 15);
    chk("37.pfd",    (long long)d->o37_pfd,    12500000LL);
    chk("37.clk",    (long long)d->o37_clk,    37500000LL);
    chk("37.sdram",  (long long)d->o37_sdram,  100000000LL);

    delete d;
    if (fails) { printf("FAIL: %d/%d checks failed\n", fails, checks); return 1; }
    printf("PASS: %d/%d checks passed\n", checks, checks);
    return 0;
}
