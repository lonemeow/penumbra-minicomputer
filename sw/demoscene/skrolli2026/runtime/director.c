/*
 * director.c — runs the parts.  Frames are paced off the timer, since
 * the display has no vblank; leftover frame time goes to the next
 * part's precalc.  Counters bracket the part's frame only, so the bar
 * never reports its own drawing.
 */

#include "rt.h"

#define TARGET_FPS  30

static uint32_t frame_ticks;

/* Frame time held back from precalc, to absorb a step's overrun. */
#define PRECALC_MARGIN_DIV  16

static void run_part(const rt_part_t *p, const rt_part_t *next,
                     int *next_ready)
{
    uint32_t start = rt_ticks();
    uint32_t end   = start + p->duration_frames * frame_ticks;
    int shown = 0;

    if (p->enter)
        p->enter();

    /* After enter(), so the reserved slots win over the part's palette. */
    rt_palette_reserved();

    uint32_t next_frame = rt_ticks();

    while ((int32_t)(rt_ticks() - end) < 0) {
        rt_perf_t a, b;

        next_frame += frame_ticks;

        rt_perf_sample(&a);
        if (p->frame)
            p->frame(rt_ticks() - start);
        rt_perf_sample(&b);

        rt_telemetry_draw(&a, &b, p->mode);

        /* Selected only once drawn: display memory is undefined at
         * start-up, and a cut to the other store should show it done. */
        if (!shown) {
            rt_display_mode(p->mode);
            rt_display_enable(1);
            shown = 1;
        }

        if (next && !*next_ready && next->precalc_step) {
            uint32_t margin = frame_ticks / PRECALC_MARGIN_DIV;
            uint32_t deadline = next_frame - margin;
            while ((int32_t)(rt_ticks() - deadline) < 0) {
                if (next->precalc_step(deadline)) {
                    *next_ready = 1;
                    break;
                }
            }
        }

        while ((int32_t)(rt_ticks() - next_frame) < 0)
            ;
    }

    if (p->leave)
        p->leave();
}

void rt_run(const rt_part_t *const *parts, int count)
{
    frame_ticks = rt_tick_hz() / TARGET_FPS;
    if (frame_ticks == 0)
        rt_panic("timer too slow to pace frames");

    for (int i = 0; i < count; i++) {
        const rt_part_t *p = parts[i];
        const rt_part_t *next = (i + 1 < count) ? parts[i + 1] : 0;
        int next_ready = 0;

        /* Nothing runs ahead of the first part, so its precalc blocks. */
        if (i == 0 && p->precalc_init) {
            p->precalc_init();
            while (p->precalc_step && !p->precalc_step(rt_ticks() + rt_tick_hz()))
                ;
        }
        if (next && next->precalc_init)
            next->precalc_init();

        run_part(p, next, &next_ready);

        /* Finish what the previous part's frames left undone. */
        if (next && !next_ready && next->precalc_step)
            while (!next->precalc_step(rt_ticks() + rt_tick_hz()))
                ;
    }
}
