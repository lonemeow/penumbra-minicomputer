// FixedDiv for a machine with no 64/32 divide.
//
// Doom's FixedDiv computes (a << 16) / b.  Every ISA it shipped on had a
// 64/32 divide, so m_fixed.c writes that as one int64_t division and the
// compiler emits a single instruction.  Penumbra divides 32/32 only, so
// the same line becomes a call to __divdi3 -- a shift-subtract loop of
// several hundred instructions -- at 437 calls per frame in the renderer.
//
// FixedDiv's own guard is what makes a short version possible.  It
// returns saturated when abs(a) >> 14 >= abs(b), so past that point
//
//     abs(a) < abs(b) << 14
//
// which bounds the quotient below 2^30.  Nothing here has to detect
// overflow; the caller already excluded it.
//
// DIVU yields quotient and remainder from one instruction:
//
//     divu  Rd, Rs, Rdh      // Rd = Rd / Rs, Rdh = Rd % Rs
//
// reached from C by writing `q = a / b; r = a % b;` on the same operands
// -- the backend fuses the pair.  DIVU and MUL each stall the pipeline
// ~33 cycles, so an algorithm's cost is roughly (instructions x CPI 3)
// plus 33 per multiply or divide.  There is no CLZ instruction;
// __builtin_clz expands to about 25 instructions.

#ifndef PENUMBRA_FIXED_HOST_TEST
#include "doomtype.h"
#include "m_fixed.h"
#endif

#include <stdint.h>
#include <stdlib.h>

fixed_t
FixedDiv(fixed_t a, fixed_t b)
{
    uint32_t num, den, quot;
    uint32_t q, rem, frac;

    if ((abs(a) >> 14) >= abs(b))
    {
        return (a ^ b) < 0 ? INT_MIN : INT_MAX;
    }

    // Magnitudes: the sign is reapplied at the end, so the division
    // itself is unsigned and DIVU applies.
    num = (uint32_t) (a < 0 ? -(int64_t) a : (int64_t) a);
    den = (uint32_t) (b < 0 ? -(int64_t) b : (int64_t) b);

    // One DIVU splits the problem: the quotient is the top half of the
    // answer (below 2^14), the remainder is what the low half divides.
    q = num / den;               // fuses to one DIVU with the line below
    rem = num % den;

    if (rem <= 0xFFFF)
    {
        // rem << 16 stays in 32 bits, so the low half is one more DIVU.
        frac = (rem << 16) / den;
    }
    else
    {
        // rem << 16 would overflow, so bring the bits down one at a time,
        // keeping the bit shifted off the top rather than losing it.  The
        // conditional subtract is data-dependent and must stay branchless
        // -- as a branch it mispredicts on roughly every iteration.
        int i;

        frac = 0;

        // Rolled: unrolled, the loop is ~200 instructions resident, a
        // quarter of the L1I, to save sixteen predictable branches.
#pragma clang loop unroll(disable)
        for (i = 0; i < 16; i++)
        {
            // Widen each predicate to a full mask before combining them.
            // Written as `carry | (rem >= den)`, InstCombine rewrites the
            // sign test into `icmp slt` and it lowers to a branch that is
            // data-dependent -- see doc/TODO/compiler-selection.md,
            // "signed comparisons have no branchless materialisation".
            // Masking keeps the shift a shift: `0 - (x >> 31)` is a
            // single SAR.
            uint32_t mask_hi = (uint32_t) -(int32_t) (rem >> 31);
            uint32_t mask_ge;

            rem <<= 1;
            frac <<= 1;

            mask_ge = (uint32_t) -(int32_t) (rem >= den);

            rem -= den & (mask_hi | mask_ge);
            frac -= mask_hi | mask_ge;
        }
    }

    quot = (q << 16) | frac;

    return (a ^ b) < 0 ? -(fixed_t) quot : (fixed_t) quot;
}
