/*
 * fix16.c — 16.16 division.  The divide is 32/32 only, so the integer
 * part comes from one DIVU and the fraction from the remainder, exactly.
 */

#include "rt.h"

fix16_t fix16_div(fix16_t a, fix16_t b)
{
    int negative = (a ^ b) < 0;
    uint32_t n = a < 0 ? -(uint32_t)a : (uint32_t)a;
    uint32_t d = b < 0 ? -(uint32_t)b : (uint32_t)b;
    uint32_t q = n / d;
    uint32_t r = n % d;

    if (q > 0x7FFF)
        return negative ? FIX16_MIN : FIX16_MAX;

    /* Long division by the remainder, as many quotient bits per DIVU as
     * r << step can hold: r < d, so d's free top bits bound the step. */
    uint32_t step;
    if (d <= 0xFFFF)
        step = 16;
    else if (d <= 0xFFFFFF)
        step = 8;
    else if (d <= 0xFFFFFFF)
        step = 4;
    else if (d <= 0x3FFFFFFF)
        step = 2;
    else
        step = 1;

    /* The remainder is spelled t - digit * d: written as t % d, the
     * optimizer merges the loop's remainders through the phi, parting
     * each from its divide and costing a second DIVU per step. */
    for (uint32_t done = 0; done < 16; done += step) {
        uint32_t t = r << step;
        uint32_t digit = t / d;

        q = q << step | digit;
        r = t - digit * d;
    }

    return negative ? -(fix16_t)q : (fix16_t)q;
}
