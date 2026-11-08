// Verilator testbench for penumbra3_perfctr.
//
// Drives a run of cycles, each either retiring an instruction or carrying
// one stall cause, and reads the counters back through the sysreg side
// against counts kept here: CYCLES every cycle, INSNS_RETIRED each retire,
// each stall counter its own cause's non-retiring cycles, a retiring cycle
// charged to no stall whatever cause it carries, and CYCLES = INSNS_RETIRED
// + the sum of the stalls. Registers outside 5-12 read 0, and reset clears
// every counter.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_perfctr.h"
#include "verilated.h"

typedef Vpenumbra3_perfctr Dut;
static Dut* d;

// bcause_e (penumbra3_pkg) and the CPU device's registers (penumbra_pkg)
enum { NONE = 0, FLUSH, IFETCH, LOAD, STORE, FUNIT, HAZARD, NCAUSE };
enum { REG_CYCLES = 5, REG_INSNS = 6, REG_FUNIT = 7, REG_IFETCH = 8, REG_LOAD = 9,
       REG_STORE = 10, REG_HAZARD = 11, REG_FLUSH = 12 };

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got %u, expected %u\n", n, g, e); errors++; }
}

static void tick() { d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval(); }

static uint32_t rd(int reg) { d->i_sys_reg = reg; d->eval(); return d->o_sys_rdata; }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    d = new Dut;
    d->i_insn_retired = 1; d->i_bcause = NONE; d->i_sys_reg = 0;
    d->i_rst = 1; tick(); tick(); d->i_rst = 0; d->eval();
    check("rst_cycles", rd(REG_CYCLES), 0);
    check("rst_insns",  rd(REG_INSNS), 0);
    check("rst_flush",  rd(REG_FLUSH), 0);

    // A run: every cycle retires or carries one cause. A retiring cycle
    // sometimes carries a stale cause too, which must not count.
    uint32_t cycles = 0, insns = 0, stall[NCAUSE] = {0};
    uint32_t lfsr = 0xACE1u;
    for (int c = 0; c < 2000; c++) {
        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
        bool retire = (lfsr & 3) == 0;
        int  cause  = 1 + (int)((lfsr >> 2) % (NCAUSE - 1));    // FLUSH..HAZARD
        d->i_insn_retired = retire;
        d->i_bcause = (retire && (lfsr & 0x10)) ? NONE : cause;
        d->eval();
        tick();
        cycles++;
        if (retire) insns++;
        else        stall[cause]++;
    }
    d->i_insn_retired = 1; d->i_bcause = NONE; d->eval();

    check("cycles", rd(REG_CYCLES), cycles);
    check("insns",  rd(REG_INSNS),  insns);
    check("funit",  rd(REG_FUNIT),  stall[FUNIT]);
    check("ifetch", rd(REG_IFETCH), stall[IFETCH]);
    check("load",   rd(REG_LOAD),   stall[LOAD]);
    check("store",  rd(REG_STORE),  stall[STORE]);
    check("hazard", rd(REG_HAZARD), stall[HAZARD]);
    check("flush",  rd(REG_FLUSH),  stall[FLUSH]);
    uint32_t sum = rd(REG_FUNIT) + rd(REG_IFETCH) + rd(REG_LOAD) + rd(REG_STORE)
                 + rd(REG_HAZARD) + rd(REG_FLUSH);
    check("partition", rd(REG_CYCLES), rd(REG_INSNS) + sum);
    for (int k = 1; k < NCAUSE; k++) {
        char n[24]; snprintf(n, sizeof(n), "cause%d_seen", k);
        check(n, stall[k] > 0, 1);                // the run reached every cause
    }

    // The CPU identity's registers and the unused ones read 0 here.
    for (int r : {0, 1, 2, 3, 4, 13, 14, 15}) {
        char n[24]; snprintf(n, sizeof(n), "reg%d_zero", r);
        check(n, rd(r), 0);
    }

    // Reset clears every counter.
    d->i_rst = 1; tick(); d->i_rst = 0; d->eval();
    for (int r = REG_CYCLES; r <= REG_FLUSH; r++) {
        char n[24]; snprintf(n, sizeof(n), "rst2_reg%d", r);
        check(n, rd(r), 0);
    }

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
