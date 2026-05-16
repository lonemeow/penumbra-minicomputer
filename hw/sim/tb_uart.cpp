// Verilator testbench for Penumbra UART (hw/rtl/io/uart.sv).
//
// Covers:
//  - RX BREAK detection (LSR.BI) — the path that lets serial-BREAK
//    drop the kernel into DDB on real hardware via cn_check_magic.
//  - NS16550A FIFO mode: com(4) probe signature, RX FIFO push/pop,
//    trigger threshold, character timeout (paste-friendliness),
//    FCR RX-reset flush, bypass-mode regression.

#include <cstdio>
#include <cstdint>
#include "Vuart.h"

static int errors = 0, tests = 0;

// One bit at 115200 baud / 25 MHz ≈ 217 cycles.  We use this for
// driving i_rx through a full UART frame.
static constexpr int BIT_CYCLES = 217;

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

static void write_reg(Vuart* d, int reg, uint8_t val) {
    d->i_addr  = (uint32_t)reg << 2;
    d->i_wdata = val;
    d->i_we    = 1;
    tick(d);
    d->i_we    = 0;
    d->i_wdata = 0;
}

/* Drive i_rx through one 10-bit UART frame (start + 8 data LSB-first + stop). */
static void serial_send_byte(Vuart* d, uint8_t byte) {
    d->i_rx = 0;                       /* start bit */
    for (int i = 0; i < BIT_CYCLES; i++) tick(d);
    for (int b = 0; b < 8; b++) {
        d->i_rx = (byte >> b) & 1;
        for (int i = 0; i < BIT_CYCLES; i++) tick(d);
    }
    d->i_rx = 1;                       /* stop bit */
    for (int i = 0; i < BIT_CYCLES; i++) tick(d);
}

/* Spin for n cycles with i_rx idle.  Used between frames and to wait
 * out the character-timeout window. */
static void idle_cycles(Vuart* d, int n) {
    d->i_rx = 1;
    for (int i = 0; i < n; i++) tick(d);
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

/* ─────────────────────────────────────────────────────────
 *  NS16550A FIFO mode tests
 * ───────────────────────────────────────────────────────── */

static void test_iir_fifo_signature(Vuart* d) {
    /* com(4) probes the 16550A by setting FCR[0]=1 and reading the
     * mode bits back from IIR[7:6].  00 = 16450, 11 = 16550A FIFO. */
    printf("test_iir_fifo_signature\n");
    reset(d);
    d->i_rx = 1;
    for (int i = 0; i < 50; i++) tick(d);

    uint8_t iir_off = read_reg(d, 2);
    CHECK("IIR[7:6]=00 with FIFO disabled", (iir_off & 0xC0) == 0x00);

    /* Turn FIFOs on (FCR[0]=1, default trigger=00 → 1 byte). */
    write_reg(d, 2, 0x01);
    uint8_t iir_on = read_reg(d, 2);
    CHECK("IIR[7:6]=11 with FIFO enabled",  (iir_on  & 0xC0) == 0xC0);
}

static void test_rx_fifo_multibyte(Vuart* d) {
    /* Push 3 bytes through the serial RX path; read them out of RBR
     * and confirm FIFO order is preserved. */
    printf("test_rx_fifo_multibyte\n");
    reset(d);
    d->i_rx = 1;
    for (int i = 0; i < 50; i++) tick(d);

    write_reg(d, 2, 0x01);                  /* FCR: FIFO on, trig=1   */
    write_reg(d, 1, 0x01);                  /* IER: ERBFI on          */

    uint8_t expected[] = { 0x55, 0xAA, 0x42 };
    for (uint8_t b : expected)
        serial_send_byte(d, b);

    idle_cycles(d, 50);

    uint8_t lsr = read_reg(d, 5);
    CHECK("LSR.DR set after 3 bytes received", lsr & 0x01);

    for (uint8_t want : expected) {
        uint8_t got = read_reg(d, 0);
        char name[64];
        snprintf(name, sizeof(name), "FIFO pop returns 0x%02x", want);
        CHECK(name, got == want);
    }

    uint8_t lsr2 = read_reg(d, 5);
    CHECK("LSR.DR clear after all 3 bytes drained", (lsr2 & 0x01) == 0);
}

static void test_rx_trigger_threshold(Vuart* d) {
    /* Set trigger=4.  With 1, 2, 3 bytes in the FIFO, IIR should show
     * "none" or character-timeout but not RX-data.  At 4 bytes the
     * priority encoder switches to RX-data (0x04 / 0xC4 with FIFO bits). */
    printf("test_rx_trigger_threshold\n");
    reset(d);
    d->i_rx = 1;
    for (int i = 0; i < 50; i++) tick(d);

    write_reg(d, 2, 0x41);                  /* FCR: FIFO on, trig=01→4*/
    write_reg(d, 1, 0x01);                  /* IER: ERBFI on          */

    for (int n = 0; n < 3; n++) {
        serial_send_byte(d, (uint8_t)(0x10 + n));
        /* Sample IIR right after the byte completes — too soon for
         * char_timeout to have latched (640 baud16x ticks ≈ 8680 sys
         * cycles, and serial_send_byte returns with only ~217 of
         * post-stop slack). */
        uint8_t iir = read_reg(d, 2);
        char name[64];
        snprintf(name, sizeof(name), "IIR != RX-data with %d bytes (trig=4)", n + 1);
        CHECK(name, (iir & 0x0F) != 0x04);
    }

    serial_send_byte(d, 0x99);              /* 4th byte: trigger fires */
    idle_cycles(d, 50);
    uint8_t iir4 = read_reg(d, 2);
    CHECK("IIR == RX-data at 4 bytes (trig=4)", (iir4 & 0x0F) == 0x04);
    CHECK("IIR keeps FIFO bits at trigger",     (iir4 & 0xC0) == 0xC0);
}

static void test_character_timeout(Vuart* d) {
    /* Set trigger=14, deliver one byte, wait > 4 char-times idle.
     * char_timeout should latch and IIR should report 0xCC bits[3:0].
     * This is the paste-friendliness path. */
    printf("test_character_timeout\n");
    reset(d);
    d->i_rx = 1;
    for (int i = 0; i < 50; i++) tick(d);

    write_reg(d, 2, 0xC1);                  /* FCR: FIFO on, trig=11→14 */
    write_reg(d, 1, 0x01);                  /* IER: ERBFI on            */

    serial_send_byte(d, 0x7E);

    /* Right after the stop bit there's been almost no idle time;
     * char_timeout should not yet be latched. */
    uint8_t iir_early = read_reg(d, 2);
    CHECK("char_timeout not premature",
          (iir_early & 0x0F) != 0x0C);

    /* 4 char-times = 640 baud16x ticks ≈ 8680 system cycles.  Wait
     * generously to ensure latching, then check IIR. */
    idle_cycles(d, 10000);

    uint8_t iir_late = read_reg(d, 2);
    CHECK("IIR reports char_timeout after idle",
          (iir_late & 0x0F) == 0x0C);
    CHECK("char_timeout keeps FIFO bits",
          (iir_late & 0xC0) == 0xC0);

    /* Reading RBR drains the FIFO; the timeout should clear. */
    (void)read_reg(d, 0);
    idle_cycles(d, 20);
    uint8_t iir_clr = read_reg(d, 2);
    CHECK("char_timeout clears after RBR drain",
          (iir_clr & 0x0F) != 0x0C);
}

static void test_idle_fifo_no_timeout(Vuart* d) {
    /* Regression: with the FIFO enabled but empty, char_timeout must
     * NOT latch periodically.  An earlier version omitted the
     * !rx_fifo_empty gate on the counter and produced ~2880 spurious
     * RX-timeout interrupts per second, making the kernel crawl. */
    printf("test_idle_fifo_no_timeout\n");
    reset(d);
    d->i_rx = 1;
    for (int i = 0; i < 50; i++) tick(d);

    write_reg(d, 2, 0xC1);                  /* FIFO on, trig=14, FIFO empty */
    write_reg(d, 1, 0x01);                  /* IER: ERBFI on                */

    /* Wait several full timeout windows worth of idle. */
    idle_cycles(d, 30000);

    uint8_t iir = read_reg(d, 2);
    CHECK("idle empty FIFO: IIR != timeout",     (iir & 0x0F) != 0x0C);
    CHECK("idle empty FIFO: o_irq deasserted",   d->o_irq == 0);

    /* Now send a byte, drain it, and continue idling.  The counter
     * resets when rbr_read drains the FIFO; with the bug, it would
     * climb again and re-fire ~347 µs after the drain. */
    serial_send_byte(d, 0xA5);
    idle_cycles(d, 10000);
    uint8_t iir_post_recv = read_reg(d, 2);
    CHECK("post-recv: IIR reports timeout",      (iir_post_recv & 0x0F) == 0x0C);

    (void)read_reg(d, 0);                   /* drain RBR -> FIFO empty       */
    idle_cycles(d, 30000);                  /* longer than one timeout window*/
    uint8_t iir_post_drain = read_reg(d, 2);
    CHECK("post-drain: timeout does not re-fire",
          (iir_post_drain & 0x0F) != 0x0C);
}

static void test_fcr_rx_reset(Vuart* d) {
    /* Push 2 bytes into the RX FIFO, then write FCR[1]=1 to flush.
     * LSR.DR must go low and the next RBR read must not return a
     * stale byte. */
    printf("test_fcr_rx_reset\n");
    reset(d);
    d->i_rx = 1;
    for (int i = 0; i < 50; i++) tick(d);

    write_reg(d, 2, 0x01);                  /* FIFO on, trig=1 */
    serial_send_byte(d, 0x11);
    serial_send_byte(d, 0x22);
    idle_cycles(d, 50);

    uint8_t lsr_before = read_reg(d, 5);
    CHECK("LSR.DR set before flush", lsr_before & 0x01);

    write_reg(d, 2, 0x03);                  /* FIFO on + RX reset pulse */
    tick(d); tick(d);                       /* let flush settle         */

    uint8_t lsr_after = read_reg(d, 5);
    CHECK("LSR.DR cleared by RX FIFO reset", (lsr_after & 0x01) == 0);
}

static void test_bypass_rx_regression(Vuart* d) {
    /* With FCR[0]=0 the part behaves as 16450: one byte at a time,
     * rbr/rx_ready as before.  Make sure the refactor didn't break it. */
    printf("test_bypass_rx_regression\n");
    reset(d);
    d->i_rx = 1;
    for (int i = 0; i < 50; i++) tick(d);

    /* FCR not written → fcr_fifo_enable stays 0. */
    serial_send_byte(d, 0x5A);
    idle_cycles(d, 50);

    uint8_t lsr = read_reg(d, 5);
    CHECK("bypass: LSR.DR set after byte", lsr & 0x01);

    uint8_t got = read_reg(d, 0);
    CHECK("bypass: RBR == 0x5A", got == 0x5A);

    uint8_t lsr2 = read_reg(d, 5);
    CHECK("bypass: LSR.DR clear after RBR", (lsr2 & 0x01) == 0);

    uint8_t iir = read_reg(d, 2);
    CHECK("bypass: IIR[7:6] = 00", (iir & 0xC0) == 0x00);
}

int main() {
    printf("=== tb_uart ===\n");
    Vuart* d = new Vuart;

    test_idle_state(d);
    test_break_detection(d);
    test_lsr_read_clears_break(d);
    test_iir_fifo_signature(d);
    test_bypass_rx_regression(d);
    test_rx_fifo_multibyte(d);
    test_rx_trigger_threshold(d);
    test_character_timeout(d);
    test_idle_fifo_no_timeout(d);
    test_fcr_rx_reset(d);

    delete d;
    printf("%d/%d tests passed (%d failed)\n", tests - errors, tests, errors);
    return errors ? 1 : 0;
}
