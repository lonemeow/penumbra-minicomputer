/* rt_internal.h — shared between runtime files; parts never include it. */

#ifndef RT_INTERNAL_H
#define RT_INTERNAL_H

#include "rt.h"

#define ACFG_CLASS_MEMORY   1
#define ACFG_CLASS_DISPLAY  6
#define ACFG_CLASS_AUDIO    7

/* The first device of a class; size may be null.  Returns 0 if none. */
int rt_find_device(uint32_t bootdata, uint32_t cls,
                   uint32_t *base, uint32_t *size);

/* Interrupt context: entered with interrupts masked, and must leave
 * them so — rt_ticks() re-enables them and is not callable here. */
void rt_timer_irq(void);
void rt_music_irq(void);

extern uint32_t rt_audio_base;

#endif /* RT_INTERNAL_H */
