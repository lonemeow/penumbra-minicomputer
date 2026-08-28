// Doom's clock, on CLOCK_MONOTONIC.

#include <time.h>

#include "doomtype.h"
#include "i_timer.h"

#define TICRATE_MS (1000 / TICRATE)

static uint64_t basetime_ms = 0;

static uint64_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void
I_InitTimer(void)
{
    basetime_ms = now_ms();
}

int
I_GetTimeMS(void)
{
    if (basetime_ms == 0)
    {
        I_InitTimer();
    }

    return (int) (now_ms() - basetime_ms);
}

int
I_GetTime(void)
{
    return I_GetTimeMS() * TICRATE / 1000;
}

void
I_Sleep(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000;

    nanosleep(&ts, NULL);
}

void
I_WaitVBL(int count)
{
    I_Sleep((count * 1000) / 70);
}
