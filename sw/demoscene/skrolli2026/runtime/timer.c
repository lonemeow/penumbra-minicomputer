/*
 * timer.c — free-running tick count: a page-0 accumulator the crt0.S
 * IRQ handler advances by one period per underflow, plus the partial
 * count.
 */

#include "rt.h"
#include "penumbra.h"

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

/* The period is reload + 1, chosen to fit the handler's ADD immediate. */
#define TIMER_RELOAD   0xFFFE
#define TIMER_PERIOD   0xFFFF

/* Also hardcoded in the crt0.S IRQ handler. */
#define TIMER_BASE_ADDR   0x38
#define timer_base_ticks  (*(volatile uint32_t *)TIMER_BASE_ADDR)

static uint32_t tick_hz;

void rt_timer_init(void)
{
    tick_hz = penumbra_read_sysreg(SYSDEV_TIMER, TM_FREQ);

    timer_base_ticks = 0;
    penumbra_write_sysreg(SYSDEV_TIMER, TM_RELOAD, TIMER_RELOAD);
    penumbra_write_sysreg(SYSDEV_TIMER, TM_COUNT,  TIMER_RELOAD);
    penumbra_write_sysreg(SYSDEV_TIMER, TM_STATUS, TMST_UDF);
    penumbra_write_sysreg(SYSDEV_TIMER, TM_CR,
                          TMCR_TICK_EN | TMCR_IRQ_EN | TMCR_AUTOLOAD);

    /* The accumulator advances only from the IRQ. */
    __asm__ __volatile__("ei" ::: "memory");
}

/* Call with interrupts enabled; they are re-enabled on return. */
uint32_t rt_ticks(void)
{
    uint32_t count, base;

    __asm__ __volatile__("di" ::: "memory");
    count = penumbra_read_sysreg(SYSDEV_TIMER, TM_COUNT);
    base = timer_base_ticks;
    if (penumbra_read_sysreg(SYSDEV_TIMER, TM_STATUS) & TMST_UDF) {
        /* Underflowed but not yet serviced: count may predate the reload. */
        base += TIMER_PERIOD;
        count = penumbra_read_sysreg(SYSDEV_TIMER, TM_COUNT);
    }
    __asm__ __volatile__("ei" ::: "memory");

    return base + (TIMER_RELOAD - count);
}

uint32_t rt_tick_hz(void)
{
    return tick_hz;
}
