// Verilator testbench for Penumbra UART (hw/rtl/io/uart.sv).
//
// Covers RX BREAK detection (LSR.BI) — the path that lets serial-BREAK
// drop the kernel into DDB on real hardware via cn_check_magic.

#include <cstdio>
#include <cstdint>
#include "Vuart.h"

static int errors = 0, tests = 0;

static void tick(Vuart* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vuart* d) {
    d->i_rst  = 1;
    d->i_rx   = 1;        /* idle high */
    d->i_addr = 0;
    d->i_wdata = 0;
    d->i_we = 0;
    d->i_re = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
}

/*
 * Read one of the 8 UART registers.  The bus has a registered
 * read path: i_re=1 + access_pending=0 latches o_rdata on the
 * first posedge; on the next cycle the FSM clears access_pending.
 * read_reg() returns after both edges so o_rdata is settled.
 */
static uint8_t read_reg(Vuart* d, int reg) {
    d->i_addr = (uint32_t)reg << 2;
    d->i_re = 1;
    tick(d);                       /* latches read, processes read-clear */
    uint8_t v = (uint8_t)d->o_rdata;
    d->i_re = 0;
    tick(d);                       /* clears access_pending */
    return v;
}

#define CHECK(name, cond) do { \
    tests++; \
    if (!(cond)) { errors++; printf("  FAIL: %s\n", name); } \
} while (0)

/*
 * Drive RX low for `n_cycles` system clocks.  At default params
 * (CLK_FREQ=25 MHz, REF_FREQ=1.8432 MHz, divisor=1) one bit takes
 * ~217 cycles, so 10 bits ≈ 2170; we run a little past that.
 */
static void hold_break(Vuart* d, int n_cycles) {
    d->i_rx = 0;
    for (int i = 0; i < n_cycles; i++)
        tick(d);
}

static void test_idle_state(Vuart* d) {
    printf("test_idle_state\n");
    reset(d);
    /* Run a few cycles with line idle.  Expect TX-ready bits set,
     * BI=0, DR=0. */
    d->i_rx = 1;
    for (int i = 0; i < 100; i++) tick(d);
    uint8_t lsr = read_reg(d, 5);
    CHECK("LSR THRE set at idle",  lsr & 0x20);
    CHECK("LSR TEMT set at idle",  lsr & 0x40);
    CHECK("LSR BI clear at idle",  (lsr & 0x10) == 0);
    CHECK("LSR DR clear at idle",  (lsr & 0x01) == 0);
}

static void test_break_detection(Vuart* d) {
    printf("test_break_detection\n");
    reset(d);
    d->i_rx = 1;
    for (int i = 0; i < 100; i++) tick(d);

    /* Hold RX low for >1 character frame so the RX FSM walks
     * IDLE → START → DATA → STOP and detects the all-zero frame. */
    hold_break(d, 2500);

    /* Release line so the FSM doesn't keep re-detecting BREAK. */
    d->i_rx = 1;
    for (int i = 0; i < 100; i++) tick(d);

    uint8_t lsr = read_reg(d, 5);
    CHECK("LSR.BI set after break",  lsr & 0x10);
    CHECK("LSR.DR set after break",  lsr & 0x01);

    uint8_t rbr = read_reg(d, 0);
    CHECK("RBR = 0x00 (break char)", rbr == 0x00);

    /* LSR.BI is sticky-cleared on LSR read; reading RBR cleared DR. */
    lsr = read_reg(d, 5);
    CHECK("LSR.BI cleared on read",  (lsr & 0x10) == 0);
    CHECK("LSR.DR cleared after RBR",(lsr & 0x01) == 0);
}

static void test_lsr_read_clears_break(Vuart* d) {
    /* Independent check: LSR-read alone clears BI, even without
     * reading RBR.  This matches the 16450 read-to-clear convention. */
    printf("test_lsr_read_clears_break\n");
    reset(d);
    d->i_rx = 1;
    for (int i = 0; i < 100; i++) tick(d);
    hold_break(d, 2500);
    d->i_rx = 1;
    for (int i = 0; i < 100; i++) tick(d);

    uint8_t lsr1 = read_reg(d, 5);
    uint8_t lsr2 = read_reg(d, 5);
    CHECK("first LSR read shows BI",      lsr1 & 0x10);
    CHECK("second LSR read clears BI",    (lsr2 & 0x10) == 0);
}

int main() {
    printf("=== tb_uart ===\n");
    Vuart* d = new Vuart;

    test_idle_state(d);
    test_break_detection(d);
    test_lsr_read_clears_break(d);

    delete d;
    printf("%d/%d tests passed (%d failed)\n", tests - errors, tests, errors);
    return errors ? 1 : 0;
}
