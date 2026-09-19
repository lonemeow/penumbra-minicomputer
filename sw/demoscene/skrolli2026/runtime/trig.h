/*
 * trig.h — sine and cosine, via rt.h.
 *
 * Angles are fix16_t turns: FIX16_ONE is a full circle, and any value
 * wraps, negative ones included.  One table of RT_SIN_STEPS serves
 * both functions, since cos(a) is sin(a + 1/4) and the wrap is the
 * index mask either way.
 *
 * The index is rounded, so a result is at most 0.0031 off: 0.6 pixels
 * at the corner of the screen, rotating about its centre.  Entries are
 * Q14 in a pinned page; tools/sintab.py generates them.
 */

#ifndef TRIG_H
#define TRIG_H

#define RT_SIN_STEPS    1024            /* a power of two: the mask wraps */
#define RT_SIN_SHIFT    6               /* 16.16 turns to table steps */

_Static_assert(FIX16_ONE >> RT_SIN_SHIFT == RT_SIN_STEPS, "one turn fills the table");

extern const int16_t rt_sintab[RT_SIN_STEPS];

static inline fix16_t rt_sin(fix16_t turns)
{
    /* Unsigned, so whole turns and negative angles fall away at the mask;
     * the half step rounds to the nearest entry. */
    uint32_t step = ((uint32_t)turns + (1u << (RT_SIN_SHIFT - 1))) >> RT_SIN_SHIFT;

    return rt_sintab[step & (RT_SIN_STEPS - 1)] * 4;     /* Q14 to 16.16 */
}

static inline fix16_t rt_cos(fix16_t turns)
{
    return rt_sin(turns + FIX16_ONE / 4);
}

#endif /* TRIG_H */
