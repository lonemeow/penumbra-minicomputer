/*
 * bench.c — Penumbra bare-metal benchmark harness implementation
 *
 * Timer measurement uses the hardware timer (sysreg device 7) in
 * free-running autoload mode with TMRELOAD = 0xFFFE (period = 0xFFFF
 * ticks).  The IRQ handler (installed by crt0.S) accumulates raw
 * ticks at address 0x38 on each underflow (adds 0xFFFF per wrap).
 * The readback function adds the partial count and converts to µs
 * using the actual timer frequency from the TMFREQ register.
 */

#include "bench.h"

/* ── Stringification for inline asm sysreg operands ────────────── */
#define _STR(x)  #x
#define XSTR(x)  _STR(x)

/* ── UART registers (0xFF000000, word-strided, data in bits 7:0) ─ */
#define UART_BASE  0xFF000000
#define UART_THR   (*(volatile uint32_t *)(UART_BASE + 0x00))
#define UART_LSR   (*(volatile uint32_t *)(UART_BASE + 0x14))
#define LSR_THRE   0x20

/* ── Timer base tick counter (on page 0, accumulated by IRQ handler) */
#define TIMER_BASE_ADDR   0x38
#define timer_base_ticks  (*(volatile uint32_t *)TIMER_BASE_ADDR)

/* Timer reload value: 0xFFFE gives period = 0xFFFF (65535) ticks,
 * which fits in a single ADD immediate in the ISR. */
#define TIMER_RELOAD      0xFFFE
#define TIMER_PERIOD      0xFFFF    /* reload + 1 */

/* Timer frequency and ticks-per-µs, set by bench_init() */
static uint32_t timer_freq;         /* Hz, from TMFREQ register */
static uint32_t timer_ticks_per_us; /* freq / 1000000 */

/* ── Timer sysregs (device 7) ──────────────────────────────────── */
#define SYSDEV_TIMER   7
#define TM_FREQ        0
#define TM_CR          1
#define TM_COUNT       2
#define TM_RELOAD      3
#define TM_STATUS      4
#define TMCR_TICK_EN   0x01
#define TMCR_IRQ_EN    0x02
#define TMCR_AUTOLOAD  0x04
#define TMST_UDF       0x01

/* ── Sysreg access macros ──────────────────────────────────────── */
#define read_sysreg(dev, reg) ({                                    \
    uint32_t __v;                                                   \
    __asm__ __volatile__("rdsys %0, " XSTR(dev) ", " XSTR(reg)     \
                         : "=r"(__v));                              \
    __v;                                                            \
})

#define write_sysreg(dev, reg, val) do {                            \
    uint32_t __v = (val);                                           \
    __asm__ __volatile__("wrsys %0, " XSTR(dev) ", " XSTR(reg)     \
                         : : "r"(__v));                             \
} while (0)

static inline void disable_interrupts(void) {
    __asm__ __volatile__("di" ::: "memory");
}

static inline void enable_interrupts(void) {
    __asm__ __volatile__("ei" ::: "memory");
}

/* ── Timer ─────────────────────────────────────────────────────── */

void bench_init(void) {
    /* Read actual timer tick frequency from hardware */
    timer_freq = read_sysreg(SYSDEV_TIMER, TM_FREQ);
    timer_ticks_per_us = timer_freq / 1000000;
    if (timer_ticks_per_us == 0)
        timer_ticks_per_us = 1;     /* safety floor */

    /* Set up timer: free-running countdown, period = 0xFFFF ticks */
    timer_base_ticks = 0;
    write_sysreg(SYSDEV_TIMER, TM_RELOAD, TIMER_RELOAD);
    write_sysreg(SYSDEV_TIMER, TM_COUNT,  TIMER_RELOAD);
    write_sysreg(SYSDEV_TIMER, TM_STATUS, TMST_UDF);   /* clear UDF */
    write_sysreg(SYSDEV_TIMER, TM_CR,
                 TMCR_TICK_EN | TMCR_IRQ_EN | TMCR_AUTOLOAD);
    enable_interrupts();
}

void bench_timer_start(void) {
    disable_interrupts();
    timer_base_ticks = 0;
    write_sysreg(SYSDEV_TIMER, TM_COUNT,  TIMER_RELOAD);
    write_sysreg(SYSDEV_TIMER, TM_STATUS, TMST_UDF);   /* clear UDF */
    enable_interrupts();
}

/* Fast tick readback + µs conversion wrapper. */
uint32_t bench_timer_elapsed_ticks(void) {
    disable_interrupts();
    uint32_t count = read_sysreg(SYSDEV_TIMER, TM_COUNT);
    uint32_t base = timer_base_ticks;
    if (read_sysreg(SYSDEV_TIMER, TM_STATUS) & TMST_UDF) {
        /* Underflow occurred but ISR hasn't run yet — add one period
         * and re-read the count (timer may have already reloaded). */
        base += TIMER_PERIOD;
        count = read_sysreg(SYSDEV_TIMER, TM_COUNT);
    }
    enable_interrupts();
    return base + (TIMER_RELOAD - count);
}

uint32_t bench_us_to_ticks(uint32_t us) {
    return us * timer_ticks_per_us;
}

uint32_t bench_timer_elapsed_us(void) {
    return bench_timer_elapsed_ticks() / timer_ticks_per_us;
}

uint32_t bench_timer_freq_hz(void) {
    return timer_freq;
}

/* ── Console output ────────────────────────────────────────────── */

void bench_putchar(int c) {
    while (!(UART_LSR & LSR_THRE))
        ;
    UART_THR = (uint32_t)c;
}

void bench_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            bench_putchar('\r');
        bench_putchar(*s++);
    }
}

void bench_print_uint(uint32_t val) {
    char buf[12];
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    if (val == 0) {
        *--p = '0';
    } else {
        while (val) {
            *--p = '0' + (val % 10);
            val /= 10;
        }
    }
    bench_puts(p);
}

void bench_print_int(int val) {
    if (val < 0) {
        bench_putchar('-');
        bench_print_uint((uint32_t)(-val));
    } else {
        bench_print_uint((uint32_t)val);
    }
}
