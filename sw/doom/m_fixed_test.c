// Host test for the Penumbra FixedDiv: checks it against the int64_t
// reference over the input space FixedDiv's guard actually admits.
//
//   make -C sw/doom test
//
// Runs natively, so the implementation can be checked without building an
// image or booting the machine.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

typedef int32_t fixed_t;
#define FRACBITS 16

#define PENUMBRA_FIXED_HOST_TEST 1
#include "m_fixed_penumbra.c"

static fixed_t
reference(fixed_t a, fixed_t b)
{
    if ((abs(a) >> 14) >= abs(b))
    {
        return (a ^ b) < 0 ? INT_MIN : INT_MAX;
    }

    return (fixed_t) (((int64_t) a << FRACBITS) / b);
}

static int failures;

static void
check(fixed_t a, fixed_t b)
{
    fixed_t want, got;

    if (b == 0)
    {
        return;
    }

    want = reference(a, b);
    got = FixedDiv(a, b);

    if (want != got && failures++ < 10)
    {
        printf("  FixedDiv(%d, %d) = %d, want %d\n", a, b, got, want);
    }
}

int
main(void)
{
    static const fixed_t edge[] = {
        1, -1, 2, -2, 3, 0x7FFF, 0x8000, 0xFFFF, 0x10000, -0x10000,
        0x7FFFFFFF, INT_MIN, 0x40000000, -0x40000000, 65535, 1 << 14,
    };
    unsigned i, j;
    uint32_t s = 12345;
    long n = 0;

    for (i = 0; i < sizeof(edge) / sizeof(edge[0]); i++)
    {
        for (j = 0; j < sizeof(edge) / sizeof(edge[0]); j++)
        {
            check(edge[i], edge[j]); n++;
        }
    }

    for (n = 0; n < 4000000; n++)
    {
        s = s * 1103515245u + 12345u;
        fixed_t a = (fixed_t) s;
        s = s * 1103515245u + 12345u;
        fixed_t b = (fixed_t) s;

        check(a, b);
        check(a >> (s & 15), b >> ((s >> 4) & 15));
    }

    if (failures)
    {
        printf("FAIL: %d mismatches\n", failures);
        return 1;
    }

    printf("FixedDiv: OK\n");
    return 0;
}
