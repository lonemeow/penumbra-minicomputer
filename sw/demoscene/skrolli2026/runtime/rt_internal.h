/* rt_internal.h — shared between runtime files; parts never include it. */

#ifndef RT_INTERNAL_H
#define RT_INTERNAL_H

#include "rt.h"

#define ACFG_CLASS_MEMORY   1
#define ACFG_CLASS_DISPLAY  6

/* The first device of a class; size may be null.  Returns 0 if none. */
int rt_find_device(uint32_t bootdata, uint32_t cls,
                   uint32_t *base, uint32_t *size);

#endif /* RT_INTERNAL_H */
