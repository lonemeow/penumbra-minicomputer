/* rt.h — the runtime interface parts are written against. */

#ifndef RT_H
#define RT_H

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef signed int     int32_t;

/* ── Failure ──────────────────────────────────────────────────── */
void rt_panic(const char *msg) __attribute__((noreturn));

/* ── Console ──────────────────────────────────────────────────── */
/* Polled 115200-baud UART: a line costs more than a frame. */
void rt_putchar(int c);
void rt_puts(const char *s);
void rt_print_uint(uint32_t v);

/* ── Memory ─────────────────────────────────────────────────── */
/* RAM above the stack, never freed; exhaustion panics.  Also panics if
 * the loaded image has grown into the stack. */
void  rt_mem_init(uint32_t bootdata);
void *rt_alloc(uint32_t bytes);

/* ── Timing ───────────────────────────────────────────────────── */
void     rt_timer_init(void);
uint32_t rt_ticks(void);            /* free-running, wraps; use deltas */
uint32_t rt_tick_hz(void);
uint32_t rt_cycles(void);           /* CPU cycle counter, wraps */

/* ── Display ──────────────────────────────────────────────────── */
/* FB_SEL picks cells or pixels for the whole screen.  Both keep their
 * contents, and switching does not re-time the output. */
#define RT_FB_W     320
#define RT_FB_H     240
#define RT_TEXT_W   80
#define RT_TEXT_H   30

typedef enum { RT_MODE_TEXT = 0, RT_MODE_FB = 1 } rt_mode_t;

/* Panics unless the device matches the geometry above, which is fixed
 * so that row addressing multiplies by a constant. */
void rt_display_init(uint32_t bootdata);
void rt_display_mode(rt_mode_t m);
rt_mode_t rt_display_current_mode(void);
void rt_display_enable(int on);

/* Slots from RT_PAL_TELEMETRY up belong to the telemetry bar and are
 * re-applied after every part's enter().  Wrap a colour ramp with a
 * power-of-two mask: `% RT_PAL_RESERVED` costs about 20 cycles. */
#define RT_PAL_TELEMETRY   248
#define RT_PAL_RESERVED    RT_PAL_TELEMETRY   /* parts own 0..247 */

void rt_palette_reserved(void);

/* 8bpp, four pixels per word: store composed words, not bytes. */
volatile uint32_t *rt_fb_words(void);      /* RT_FB_W/4 * RT_FB_H */
void rt_fb_palette(uint32_t index, uint32_t rgb);

volatile uint32_t *rt_cells(void);         /* RT_TEXT_W * RT_TEXT_H */
#define RT_CELL(glyph, fg, bg) \
    ((uint32_t)(glyph) | ((uint32_t)((bg) << 4 | (fg)) << 8))

/* ── Performance counters ─────────────────────────────────────── */
typedef struct {
    uint32_t cycles;
    uint32_t retired;
    uint32_t stall_funit;
    uint32_t stall_ifetch;
    uint32_t stall_load;
    uint32_t stall_store;
    uint32_t stall_hazard;
    uint32_t stall_flush;
} rt_perf_t;

#define RT_PERF_BUCKETS 7          /* retired + six stall causes */

void rt_perf_sample(rt_perf_t *out);
/* Bucket deltas in the bar's stacking order; returns the cycle delta. */
uint32_t rt_perf_delta(const rt_perf_t *a, const rt_perf_t *b,
                       uint32_t out[RT_PERF_BUCKETS]);

/* ── Parts ────────────────────────────────────────────────────── */
typedef struct {
    const char *name;
    rt_mode_t   mode;
    uint32_t    duration_frames;
    void (*precalc_init)(void);
    /* Runs in leftover frame time: returns 1 when done, and returns by
     * the deadline either way. */
    int  (*precalc_step)(uint32_t deadline_ticks);
    void (*enter)(void);
    void (*frame)(uint32_t t);      /* t: timer ticks since the part began */
    void (*leave)(void);
} rt_part_t;

/* ── Telemetry ────────────────────────────────────────────────── */
void rt_telemetry_draw(const rt_perf_t *before, const rt_perf_t *after,
                       rt_mode_t mode);

/* ── Director ─────────────────────────────────────────────────── */
void rt_run(const rt_part_t *const *parts, int count);

#endif /* RT_H */
