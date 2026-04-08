// Verilator testbench for Penumbra Programmable Interval Timer
//
// Tests the sysreg interface, countdown, underflow, auto-reload,
// one-shot mode, IRQ output, and write-1-to-clear status.

#include <cstdio>
#include <cstdint>
#include "Vtimer.h"

static int errors = 0, tests = 0;

// ── Helpers ────────────────────────────────────────────────

// Advance one CPU clock cycle (tick input held at current level)
static void clk(Vtimer* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

// Pulse the tick input: low→high→(sync through)→low
// After calling, the timer has seen exactly one tick edge.
// The two-FF synchroniser needs 2 clk edges to propagate,
// plus 1 more for edge detection (tick_prev), so we clock 4 times.
static void pulse_tick(Vtimer* d) {
    d->i_tick = 1;
    clk(d); clk(d); clk(d);
    d->i_tick = 0;
    clk(d);
}

static void reset(Vtimer* d) {
    d->i_rst = 1;
    d->i_tick = 0;
    d->i_sys_reg = 0;
    d->i_sys_wdata = 0;
    d->i_sys_we = 0;
    clk(d); clk(d);
    d->i_rst = 0;
}

static void sys_write(Vtimer* d, uint32_t reg, uint32_t data) {
    d->i_sys_reg = reg;
    d->i_sys_wdata = data;
    d->i_sys_we = 1;
    clk(d);
    d->i_sys_we = 0;
}

static uint32_t sys_read(Vtimer* d, uint32_t reg) {
    d->i_sys_reg = reg;
    d->eval();
    return d->o_sys_rdata;
}

// Sysreg register indices (must match penumbra_pkg)
enum {
    TM_FREQ   = 0,
    TM_CR     = 1,
    TM_COUNT  = 2,
    TM_RELOAD = 3,
    TM_STATUS = 4,
};

// TMCR bits
enum {
    CR_TICK_EN  = 1 << 0,
    CR_IRQ_EN   = 1 << 1,
    CR_AUTOLOAD = 1 << 2,
};

#define CHECK(name, cond) do { \
    tests++; \
    if (!(cond)) { errors++; printf("  FAIL: %s\n", name); } \
} while (0)

// ── Tests ──────────────────────────────────────────────────

static void test_reset_state(Vtimer* d) {
    printf("test_reset_state\n");
    reset(d);
    CHECK("TMCR is 0 after reset",     sys_read(d, TM_CR) == 0);
    CHECK("TMCOUNT is 0 after reset",   sys_read(d, TM_COUNT) == 0);
    CHECK("TMRELOAD is 0 after reset",  sys_read(d, TM_RELOAD) == 0);
    CHECK("TMSTATUS is 0 after reset",  sys_read(d, TM_STATUS) == 0);
    CHECK("o_irq deasserted after reset", d->o_irq == 0);
}

static void test_freq_register(Vtimer* d) {
    printf("test_freq_register\n");
    reset(d);
    // Default TICK_FREQ_HZ = 1000000
    CHECK("TMFREQ reads 1000000", sys_read(d, TM_FREQ) == 1000000);

    // Verify read-only: writing should have no effect
    sys_write(d, TM_FREQ, 0x12345678);
    CHECK("TMFREQ unchanged after write", sys_read(d, TM_FREQ) == 1000000);
}

static void test_cr_readback(Vtimer* d) {
    printf("test_cr_readback\n");
    reset(d);

    sys_write(d, TM_CR, CR_TICK_EN | CR_IRQ_EN | CR_AUTOLOAD);
    CHECK("TMCR reads back all bits", sys_read(d, TM_CR) == 0x7);

    sys_write(d, TM_CR, CR_IRQ_EN);
    CHECK("TMCR reads back IRQ_EN only", sys_read(d, TM_CR) == 0x2);

    sys_write(d, TM_CR, 0);
    CHECK("TMCR reads back 0", sys_read(d, TM_CR) == 0);
}

static void test_count_reload_readback(Vtimer* d) {
    printf("test_count_reload_readback\n");
    reset(d);

    sys_write(d, TM_COUNT, 0xABCD);
    CHECK("TMCOUNT reads back",  sys_read(d, TM_COUNT) == 0xABCD);

    sys_write(d, TM_RELOAD, 0x1234);
    CHECK("TMRELOAD reads back", sys_read(d, TM_RELOAD) == 0x1234);

    // Upper 16 bits should be zero
    sys_write(d, TM_COUNT, 0xFFFFFFFF);
    CHECK("TMCOUNT masks to 16 bits", sys_read(d, TM_COUNT) == 0xFFFF);
}

static void test_countdown(Vtimer* d) {
    printf("test_countdown\n");
    reset(d);

    sys_write(d, TM_COUNT, 5);
    sys_write(d, TM_CR, CR_TICK_EN);  // Enable, no IRQ, no autoload

    pulse_tick(d);
    CHECK("count 5→4", sys_read(d, TM_COUNT) == 4);

    pulse_tick(d);
    CHECK("count 4→3", sys_read(d, TM_COUNT) == 3);

    pulse_tick(d);
    CHECK("count 3→2", sys_read(d, TM_COUNT) == 2);

    pulse_tick(d);
    CHECK("count 2→1", sys_read(d, TM_COUNT) == 1);

    pulse_tick(d);
    CHECK("count 1→0", sys_read(d, TM_COUNT) == 0);

    // No UDF yet — counter just reached 0
    CHECK("no underflow yet at 0", sys_read(d, TM_STATUS) == 0);
}

static void test_underflow_oneshot(Vtimer* d) {
    printf("test_underflow_oneshot\n");
    reset(d);

    sys_write(d, TM_COUNT, 1);
    sys_write(d, TM_CR, CR_TICK_EN);  // One-shot (no AUTOLOAD)

    // Tick: 1→0
    pulse_tick(d);
    CHECK("count at 0", sys_read(d, TM_COUNT) == 0);
    CHECK("no UDF yet",  sys_read(d, TM_STATUS) == 0);

    // Tick at 0: underflow fires, one-shot clears TICK_EN
    pulse_tick(d);
    CHECK("UDF set",            sys_read(d, TM_STATUS) == 1);
    CHECK("TICK_EN cleared",    (sys_read(d, TM_CR) & CR_TICK_EN) == 0);

    // Further ticks should not change anything (stopped)
    pulse_tick(d);
    CHECK("count still 0",     sys_read(d, TM_COUNT) == 0);
    CHECK("UDF still set",     sys_read(d, TM_STATUS) == 1);
}

static void test_underflow_autoload(Vtimer* d) {
    printf("test_underflow_autoload\n");
    reset(d);

    sys_write(d, TM_RELOAD, 100);
    sys_write(d, TM_COUNT, 1);
    sys_write(d, TM_CR, CR_TICK_EN | CR_AUTOLOAD);

    // Tick: 1→0
    pulse_tick(d);
    CHECK("count at 0", sys_read(d, TM_COUNT) == 0);

    // Tick at 0: underflow, auto-reload from TMRELOAD
    pulse_tick(d);
    CHECK("UDF set",             sys_read(d, TM_STATUS) == 1);
    CHECK("count reloaded to 100", sys_read(d, TM_COUNT) == 100);
    CHECK("TICK_EN still set",   (sys_read(d, TM_CR) & CR_TICK_EN) != 0);

    // Continues counting
    pulse_tick(d);
    CHECK("count 100→99", sys_read(d, TM_COUNT) == 99);
}

static void test_irq_output(Vtimer* d) {
    printf("test_irq_output\n");
    reset(d);

    sys_write(d, TM_COUNT, 1);
    sys_write(d, TM_CR, CR_TICK_EN | CR_IRQ_EN);  // No autoload

    // Tick: 1→0 (no underflow yet)
    pulse_tick(d);
    CHECK("no IRQ before underflow", d->o_irq == 0);

    // Tick at 0: underflow
    pulse_tick(d);
    CHECK("IRQ asserted",   d->o_irq == 1);
    CHECK("UDF set",        sys_read(d, TM_STATUS) == 1);

    // Clear UDF via W1C → IRQ deasserts
    sys_write(d, TM_STATUS, 1);
    d->eval();
    CHECK("UDF cleared",    sys_read(d, TM_STATUS) == 0);
    CHECK("IRQ deasserted", d->o_irq == 0);
}

static void test_irq_gating(Vtimer* d) {
    printf("test_irq_gating\n");
    reset(d);

    sys_write(d, TM_COUNT, 1);
    sys_write(d, TM_CR, CR_TICK_EN);  // IRQ_EN=0

    pulse_tick(d);  // 1→0
    pulse_tick(d);  // underflow

    CHECK("UDF set",            sys_read(d, TM_STATUS) == 1);
    CHECK("IRQ not asserted (IRQ_EN=0)", d->o_irq == 0);

    // Enable IRQ → should assert immediately (level-triggered)
    sys_write(d, TM_CR, CR_IRQ_EN);
    d->eval();
    CHECK("IRQ asserts when IRQ_EN set", d->o_irq == 1);

    // Disable IRQ → deasserts
    sys_write(d, TM_CR, 0);
    d->eval();
    CHECK("IRQ deasserts when IRQ_EN cleared", d->o_irq == 0);
}

static void test_w1c_status(Vtimer* d) {
    printf("test_w1c_status\n");
    reset(d);

    // Trigger underflow
    sys_write(d, TM_COUNT, 0);
    sys_write(d, TM_CR, CR_TICK_EN);
    pulse_tick(d);
    CHECK("UDF set", sys_read(d, TM_STATUS) == 1);

    // Writing 0 to status should NOT clear UDF
    sys_write(d, TM_STATUS, 0);
    CHECK("UDF not cleared by writing 0", sys_read(d, TM_STATUS) == 1);

    // Writing 1 clears it
    sys_write(d, TM_STATUS, 1);
    CHECK("UDF cleared by writing 1", sys_read(d, TM_STATUS) == 0);
}

static void test_write_count_while_running(Vtimer* d) {
    printf("test_write_count_while_running\n");
    reset(d);

    sys_write(d, TM_COUNT, 1000);
    sys_write(d, TM_CR, CR_TICK_EN | CR_AUTOLOAD);

    pulse_tick(d);
    CHECK("count decremented", sys_read(d, TM_COUNT) == 999);

    // Overwrite count mid-run
    sys_write(d, TM_COUNT, 500);
    CHECK("count updated to 500", sys_read(d, TM_COUNT) == 500);

    pulse_tick(d);
    CHECK("continues from 500→499", sys_read(d, TM_COUNT) == 499);
}

static void test_disabled_ignores_ticks(Vtimer* d) {
    printf("test_disabled_ignores_ticks\n");
    reset(d);

    sys_write(d, TM_COUNT, 100);
    // Don't enable TICK_EN

    pulse_tick(d);
    pulse_tick(d);
    pulse_tick(d);
    CHECK("count unchanged when disabled", sys_read(d, TM_COUNT) == 100);
}

static void test_reserved_regs_read_zero(Vtimer* d) {
    printf("test_reserved_regs_read_zero\n");
    reset(d);

    for (int reg = 5; reg <= 15; reg++) {
        uint32_t val = sys_read(d, reg);
        if (val != 0) {
            errors++;
            printf("  FAIL: reg %d reads 0x%08x (expected 0)\n", reg, val);
        }
        tests++;
    }
}

static void test_periodic_underflow_cycle(Vtimer* d) {
    printf("test_periodic_underflow_cycle\n");
    reset(d);

    // Set up periodic timer with reload=2, so period is 3 ticks (2,1,0,underflow)
    sys_write(d, TM_RELOAD, 2);
    sys_write(d, TM_COUNT, 2);
    sys_write(d, TM_CR, CR_TICK_EN | CR_IRQ_EN | CR_AUTOLOAD);

    // Tick 1: 2→1
    pulse_tick(d);
    CHECK("cycle1: count=1", sys_read(d, TM_COUNT) == 1);
    CHECK("cycle1: no UDF",  sys_read(d, TM_STATUS) == 0);

    // Tick 2: 1→0
    pulse_tick(d);
    CHECK("cycle2: count=0", sys_read(d, TM_COUNT) == 0);
    CHECK("cycle2: no UDF",  sys_read(d, TM_STATUS) == 0);

    // Tick 3: underflow at 0, reload to 2
    pulse_tick(d);
    CHECK("cycle3: count=2 (reloaded)", sys_read(d, TM_COUNT) == 2);
    CHECK("cycle3: UDF set",            sys_read(d, TM_STATUS) == 1);
    CHECK("cycle3: IRQ asserted",       d->o_irq == 1);

    // Clear UDF
    sys_write(d, TM_STATUS, 1);
    CHECK("UDF cleared", sys_read(d, TM_STATUS) == 0);

    // Tick 4: 2→1 (second period)
    pulse_tick(d);
    CHECK("cycle4: count=1", sys_read(d, TM_COUNT) == 1);
    CHECK("cycle4: no UDF",  sys_read(d, TM_STATUS) == 0);

    // Tick 5: 1→0
    pulse_tick(d);
    CHECK("cycle5: count=0", sys_read(d, TM_COUNT) == 0);

    // Tick 6: underflow again
    pulse_tick(d);
    CHECK("cycle6: count=2 (reloaded)", sys_read(d, TM_COUNT) == 2);
    CHECK("cycle6: UDF set again",      sys_read(d, TM_STATUS) == 1);
}

int main() {
    Vtimer* d = new Vtimer;

    test_reset_state(d);
    test_freq_register(d);
    test_cr_readback(d);
    test_count_reload_readback(d);
    test_countdown(d);
    test_underflow_oneshot(d);
    test_underflow_autoload(d);
    test_irq_output(d);
    test_irq_gating(d);
    test_w1c_status(d);
    test_write_count_while_running(d);
    test_disabled_ignores_ticks(d);
    test_reserved_regs_read_zero(d);
    test_periodic_underflow_cycle(d);

    printf("\ntimer: %d/%d passed\n", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
