/* rt_internal.h — shared between runtime files; parts never include it. */

#ifndef RT_INTERNAL_H
#define RT_INTERNAL_H

#include "rt.h"
#include "penumbra.h"

/* Traps to the ROM monitor. */
static inline void rt_break(void)
{
    __asm__ __volatile__("break");
}

/* The first device of a class; size may be null.  Returns 0 if none. */
int rt_find_device(uint32_t bootdata, uint32_t cls,
                   uint32_t *base, uint32_t *size);

/* Interrupt context: entered with interrupts masked, and must leave
 * them so — rt_ticks() re-enables them and is not callable here. */
void rt_timer_irq(void);
void rt_music_irq(void);

/* Elapsed share of the current timer interrupt period, in [0, 1);
 * just under 1 once the period has ended but its interrupt is still
 * pending.  Call with interrupts masked. */
fix16_t rt_timer_subtick(void);

/* Panics if the build linked no song. */
void rt_music_start(void);
void rt_music_stop(void);

extern uint32_t rt_audio_base;

#endif /* RT_INTERNAL_H */
