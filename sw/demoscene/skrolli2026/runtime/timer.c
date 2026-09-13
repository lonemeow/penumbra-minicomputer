/*
 * timer.c — free-running tick count: the interrupt advances a base by
 * one period per underflow, and readers add the partial count.
 *
 * The interrupt is also the replayer's tick, one ProTracker tick at the
 * default 125 BPM.  The tick rate is the CPU clock over an integer
 * prescaler and rarely divides by 50, so the period is rounded: at most
 * half a count in ~20000, 25 ppm, inside the crystal's own tolerance.
 */

#include "rt_internal.h"
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

#define TIMER_IRQ_HZ   50

static uint32_t tick_hz;
static uint32_t period;
static uint32_t base_ticks;

void rt_timer_init(void)
{
    tick_hz = penumbra_read_sysreg(SYSDEV_TIMER, TM_FREQ);
    period = (tick_hz + TIMER_IRQ_HZ / 2) / TIMER_IRQ_HZ;
    if (period < 2 || period - 1 > 0xFFFF)
        rt_panic("timer rate cannot pace a 50 Hz interrupt");

    base_ticks = 0;
    penumbra_write_sysreg(SYSDEV_TIMER, TM_RELOAD, period - 1);
    penumbra_write_sysreg(SYSDEV_TIMER, TM_COUNT,  period - 1);
    penumbra_write_sysreg(SYSDEV_TIMER, TM_STATUS, TMST_UDF);
    penumbra_write_sysreg(SYSDEV_TIMER, TM_CR,
                          TMCR_TICK_EN | TMCR_IRQ_EN | TMCR_AUTOLOAD);

    __asm__ __volatile__("ei" ::: "memory");
}

void rt_timer_irq(void)
{
    penumbra_write_sysreg(SYSDEV_TIMER, TM_STATUS, TMST_UDF);
    base_ticks += period;
    rt_music_irq();
}

/* Call with interrupts enabled; they are re-enabled on return. */
uint32_t rt_ticks(void)
{
    uint32_t count, base;

    __asm__ __volatile__("di" ::: "memory");
    count = penumbra_read_sysreg(SYSDEV_TIMER, TM_COUNT);
    base = base_ticks;
    if (penumbra_read_sysreg(SYSDEV_TIMER, TM_STATUS) & TMST_UDF) {
        /* Underflowed but not yet serviced: count may predate the reload. */
        base += period;
        count = penumbra_read_sysreg(SYSDEV_TIMER, TM_COUNT);
    }
    __asm__ __volatile__("ei" ::: "memory");

    return base + (period - 1 - count);
}

uint32_t rt_tick_hz(void)
{
    return tick_hz;
}
