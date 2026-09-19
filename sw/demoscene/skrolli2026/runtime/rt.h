/* rt.h — the runtime interface parts are written against. */

#ifndef RT_H
#define RT_H

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef signed short   int16_t;
typedef unsigned int   uint32_t;
typedef signed int     int32_t;
typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

#include "fix16.h"
#include "rand.h"
#include "trig.h"

typedef __SIZE_TYPE__ size_t;

/* ── Failure ──────────────────────────────────────────────────── */
void rt_panic(const char *msg) __attribute__((noreturn));

/* ── Console ──────────────────────────────────────────────────── */
/* Polled 115200-baud UART: a line costs more than a frame. */
void rt_putchar(int c);
void rt_puts(const char *s);
void rt_print_uint(uint32_t v);

/* ── Strings and blocks ───────────────────────────────────────── */
/* NetBSD's libc routines for this machine, a word at a time when the
 * alignment allows.  The compiler also calls memcpy and memset itself,
 * for struct copies, large initializers and loops it recognizes.
 * memcpy does not handle overlap, and there is no memmove. */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *dst, const char *src);

/* ── Memory ─────────────────────────────────────────────────── */
/* RAM above the stack.  Exhaustion panics, as does freeing anything
 * that is not a live allocation; rt_mem_init() panics if the loaded
 * image has grown into the stack.
 *
 * A part frees its buffers from leave(), where the director tells it
 * its cues are done.  Blocks are one cache line aligned, and their
 * contents are undefined.  Not callable from interrupt context. */
void  rt_mem_init(uint32_t bootdata);
void *rt_alloc(uint32_t bytes);
void  rt_free(void *p);                 /* null is a no-op */
/* Free bytes in all blocks, and the largest single allocation left;
 * either may be null. */
void  rt_mem_stats(uint32_t *total, uint32_t *largest);

/* ── Pinned data ──────────────────────────────────────────────── */
/* Globals the TLB always maps, for hot tables: each page they fill gets
 * a pinned slot, so a part streaming through the framebuffer's pages
 * cannot evict them.  Six pages at most, checked at link time.  This
 * pins the translation only — the L1-D caches them like any data.
 * Const and writable data need separate sections, hence two macros. */
#define RT_PINNED        __attribute__((section(".pinned.data")))
#define RT_PINNED_CONST  __attribute__((section(".pinned.rodata")))

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

/* The hardware cursor: the cell in inverse video, blinking about once a
 * second.  A cell whose colours are the same both ways shows nothing.
 * The director hides it between parts. */
void rt_display_cursor(uint32_t col, uint32_t row);
void rt_display_cursor_hide(void);

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

/* The device's built-in text palette, CGA order.  Both nibbles of the
 * attribute are full indices, so any of the 16 is a background too. */
#define RT_COL_BLACK      0     /* 000000 */
#define RT_COL_BLUE       1     /* 0000AA */
#define RT_COL_GREEN      2     /* 00AA00 */
#define RT_COL_CYAN       3     /* 00AAAA */
#define RT_COL_RED        4     /* AA0000 */
#define RT_COL_MAGENTA    5     /* AA00AA */
#define RT_COL_BROWN      6     /* AA5500 */
#define RT_COL_GREY       7     /* AAAAAA */
#define RT_COL_DARKGREY   8     /* 555555 */
#define RT_COL_LTBLUE     9     /* 5555FF */
#define RT_COL_LTGREEN    10    /* 55FF55 */
#define RT_COL_LTCYAN     11    /* 55FFFF */
#define RT_COL_LTRED      12    /* FF5555 */
#define RT_COL_LTMAGENTA  13    /* FF55FF */
#define RT_COL_YELLOW     14    /* FFFF55 */
#define RT_COL_WHITE      15    /* FFFFFF */

/* ── Images ───────────────────────────────────────────────────── */
/* assets/<name>.png, an indexed PNG, is compiled by tools/png2image.py
 * into <name>_image.h: the image <name>_image and its size in
 * <NAME>_IMAGE_W and <NAME>_IMAGE_H.  The build fails if a pixel uses a
 * palette slot the telemetry owns.  One byte per pixel, and rows are
 * padded to whole words, so a row landing on a 4-pixel boundary copies
 * to the framebuffer a word at a time. */
typedef struct {
    uint16_t        w, h;
    uint16_t        stride;             /* bytes per row, a multiple of 4 */
    uint16_t        colors;             /* palette entries, from slot 0 */
    const uint32_t *palette;            /* 0x00RRGGBB */
    const uint8_t  *pixels;             /* h rows of stride bytes */
} rt_image_t;

/* Loads the image's colours into the framebuffer palette from slot 0. */
void rt_image_palette(const rt_image_t *img);

/* ── Typed text ───────────────────────────────────────────────── */
/* parts/<name>.txt is compiled by tools/text2typer.py, which checks it,
 * into <name>_text.h: the stream <name>_text[] and the extent of its
 * largest page in <NAME>_TEXT_COLS and <NAME>_TEXT_ROWS.  A typer plays
 * the stream into a window of cells, on song time, keeping the cursor
 * on the cell it types next; {clear} blanks the window.  Plain text is
 * grey on black.  Typing outside the window panics. */
typedef struct {
    const uint8_t *p;
    fix16_t next;                       /* song time of the next step */
    fix16_t step;                       /* song time per character */
    uint8_t col0, row0, cols, rows;     /* the window */
    uint8_t col, row;                   /* the cell typed next */
    uint8_t fg, bg;
    uint8_t done;
} rt_typer_t;

void rt_typer_start(rt_typer_t *t, const unsigned char *text,
                    uint32_t col, uint32_t row,
                    uint32_t cols, uint32_t rows,
                    fix16_t chars_per_row, fix16_t start);
/* Types everything due by now; returns 1 once the text is all out. */
int  rt_typer_update(rt_typer_t *t, fix16_t now);

/* ── Music ────────────────────────────────────────────────────── */
/* Panics unless the device has 4 channels with rate and gain at 48 kHz. */
void rt_audio_init(uint32_t bootdata);

/* Song time: rows since the song began, in 16.16, advancing smoothly
 * within a row.  It is the heard position, and it counts
 * order * 64 + row, so it runs backwards across a Bxx jump. */
#define RT_POS(order, row)  ((fix16_t)(((order) * 64 + (row)) << 16))

fix16_t rt_music_now(void);

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
    void (*precalc_init)(void);
    /* Runs in the refresh time the frame before it left: returns 1 when
     * done, and returns by the deadline (rt_ticks()) either way. */
    int  (*precalc_step)(uint32_t deadline_ticks);
    void (*enter)(void);
    /* Song times: now, and the cues this part runs between. */
    void (*frame)(fix16_t now, fix16_t start, fix16_t end);
    void (*leave)(void);
} rt_part_t;

/* A part runs from its cue's position to the next cue's.  The table
 * ends with a null part, whose position ends the demo. */
typedef struct {
    fix16_t          pos;           /* RT_POS() */
    const rt_part_t *part;
} rt_cue_t;

/* ── Telemetry ────────────────────────────────────────────────── */
/* Draws every frame, recomputing a few times a second from the counts
 * summed since.  restart() starts a part's own window, and has the
 * next draw recompute at once. */
void rt_telemetry_restart(void);
void rt_telemetry_draw(const rt_perf_t *before, const rt_perf_t *after,
                       rt_mode_t mode);

/* ── Director ─────────────────────────────────────────────────── */
/* Starts the song and plays the cue table; panics if no song is linked. */
void rt_run(const rt_cue_t *cues);

#endif /* RT_H */
