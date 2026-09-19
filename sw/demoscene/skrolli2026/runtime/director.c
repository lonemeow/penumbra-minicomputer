/*
 * director.c — runs the cue table against song time.  Frames run free,
 * capped at the display's refresh since a faster one is never seen; the
 * rest of the refresh goes to the next part's precalc.  Counters bracket
 * the part's frame only, so the bar never reports its own drawing.
 */

#include "rt_internal.h"

/* The display has no vblank to pace against, only its fixed rate. */
#define DISPLAY_HZ  60

static uint32_t refresh_ticks;

static void blocking_precalc(const rt_part_t *p)
{
    while (!p->precalc_step(rt_ticks() + rt_tick_hz()))
        ;
}

static void report(const char *name, const char *what, fix16_t late)
{
    rt_puts("  ");
    rt_puts(name);
    rt_puts(what);
    rt_print_uint((uint32_t)fix16_to_int(late));
    rt_puts(" rows\r\n");
}

static void run_part(const rt_cue_t *cue, const rt_part_t *next,
                     int *next_ready)
{
    const rt_part_t *p = cue->part;
    fix16_t start = cue->pos, end = cue[1].pos;
    int shown = 0;

    rt_display_cursor_hide();
    if (p->enter)
        p->enter();

    /* After enter(), so the reserved slots win over the part's palette. */
    rt_palette_reserved();
    rt_telemetry_restart();

    for (;;) {
        uint32_t refresh_end = rt_ticks() + refresh_ticks;
        fix16_t now = rt_music_now();
        rt_perf_t a, b;

        if (now >= end)
            break;

        rt_perf_sample(&a);
        if (p->frame)
            p->frame(now, start, end);
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
            while ((int32_t)(rt_ticks() - refresh_end) < 0) {
                if (next->precalc_step(refresh_end)) {
                    *next_ready = 1;
                    break;
                }
            }
        }

        while ((int32_t)(rt_ticks() - refresh_end) < 0)
            ;
    }

    if (!shown)
        report(p->name, " drew no frame, arriving late by ",
               rt_music_now() - start);

    if (p->leave)
        p->leave();
}

void rt_run(const rt_cue_t *cues)
{
    refresh_ticks = rt_tick_hz() / DISPLAY_HZ;

    if (!cues[0].part)
        rt_panic("empty cue table");

    /* Nothing runs ahead of the first part, so its precalc blocks, and
     * the song waits for it. */
    if (cues[0].part->precalc_init)
        cues[0].part->precalc_init();
    if (cues[0].part->precalc_step)
        blocking_precalc(cues[0].part);

    rt_music_start();

    for (const rt_cue_t *c = cues; c->part; c++) {
        const rt_part_t *next = c[1].part;
        int next_ready = 0;

        if (next && next->precalc_init)
            next->precalc_init();

        run_part(c, next, &next_ready);

        /* The song does not wait, so finishing here costs the next part
         * its opening. */
        if (next && !next_ready && next->precalc_step) {
            blocking_precalc(next);
            report(next->name, " precalc overran its cue by ",
                   rt_music_now() - c[1].pos);
        }
    }

    rt_music_stop();
}
