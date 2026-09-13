/*
 * modplay.c — ProTracker replayer, one device channel per track.
 *
 * Pitch, volume and flush take effect on the next output frame and only
 * sample data queues, so nothing can be computed ahead: each tick runs
 * from the timer interrupt when it is due, and the song position is the
 * position being heard.
 *
 * Effects are the subset tools/mod2song.py keeps.
 */

#include "rt_internal.h"
#include "audio.h"

#define SONG_MAGIC      0x474E4F53u     /* "SONG" */
#define ROWS            64
#define PERIOD_MIN      113             /* ProTracker's B-3 */
#define VOLUME_MAX      64

/* Paula's PAL clock over the output rate, in 16.16: INC = PAULA_INC / period. */
#define PAULA_INC       4842694u        /* 3546895 * 65536 / 48000 */

/* Frames queued at a note's start, before the other tracks are serviced. */
#define PRIME_FRAMES    64

struct song_sample {
    uint32_t data;              /* offset from the song */
    uint32_t length;
    uint32_t loop_start;
    uint32_t loop_len;          /* 0: one-shot */
    uint32_t volume;
};

struct song {
    uint32_t magic;
    uint8_t  length;
    uint8_t  restart;
    uint8_t  npatterns;
    uint8_t  pad;
    uint32_t patterns;          /* offset from the song */
    uint8_t  orders[128];
    struct song_sample samples[31];
};

struct track {
    const struct song_sample *smp;
    const uint32_t *data;
    uint32_t pos, end;          /* frames into data */
    uint32_t playing;
    uint32_t period;
    uint32_t volume;
    uint32_t effect, param;
};

extern const struct song song_start;
extern const uint8_t song_end[];

static const struct song *song;
static const uint32_t *cells;

/* The song's sample bytes as device frames, indexed from sample 1's data. */
static const uint32_t *frames;
static struct track tracks[AUDIO_TRACKS];

static uint32_t running;
static uint32_t speed, tick;
static uint32_t order, row;
static uint32_t jump;           /* order + 1 of a pending Bxx, or 0 */
static uint32_t position;

static void set_period(uint32_t ch, struct track *t, uint32_t period)
{
    t->period = period;
    AU_CH(ch, PA_C_INC) = PAULA_INC / period;
}

static void set_volume(uint32_t ch, struct track *t)
{
    uint32_t gain = t->volume << 10;    /* 64 is unity */

    if (gain > 0xFFFF)
        gain = 0xFFFF;
    AU_CH(ch, PA_C_VOL) = gain | gain << 16;
}

/* Queue up to `room` frames, following the sample's loop. */
static void push(uint32_t ch, struct track *t, uint32_t room)
{
    volatile uint32_t *fifo = &AU_CH(ch, PA_C_FIFO);

    while (room && t->playing) {
        uint32_t n = t->end - t->pos;
        if (n > room)
            n = room;

        const uint32_t *p = t->data + t->pos;
        for (uint32_t i = 0; i < n; i++)
            *fifo = p[i];

        t->pos += n;
        room -= n;

        if (t->pos == t->end) {
            if (t->smp->loop_len)
                t->pos = t->smp->loop_start;
            else
                break;
        }
    }
}

static void trigger(uint32_t ch, struct track *t)
{
    const struct song_sample *s = t->smp;

    AU_CH(ch, PA_C_CTRL) = PA_C_CTRL_FLUSH;
    t->data = frames + (s->data - song->samples[0].data);
    t->pos = 0;
    t->end = s->length;
    t->playing = s->length != 0;
    push(ch, t, PRIME_FRAMES);
}

static void play_row(void)
{
    const uint32_t *c = cells + (song->orders[order] * ROWS + row) * AUDIO_TRACKS;

    for (uint32_t ch = 0; ch < AUDIO_TRACKS; ch++) {
        struct track *t = &tracks[ch];
        uint32_t period = c[ch] & 0xFFF;
        uint32_t sample = (c[ch] >> 12) & 0xFF;

        t->effect = (c[ch] >> 20) & 0xF;
        t->param  = c[ch] >> 24;

        if (sample) {
            t->smp = &song->samples[sample - 1];
            t->volume = t->smp->volume;
        }
        if (period && t->smp) {
            set_period(ch, t, period);
            trigger(ch, t);
        }

        switch (t->effect) {
        case 0xB:
            jump = t->param + 1;
            break;
        case 0xC:
            t->volume = t->param < VOLUME_MAX ? t->param : VOLUME_MAX;
            break;
        case 0xF:
            speed = t->param;
            break;
        }
        set_volume(ch, t);
    }
}

static void play_effects(void)
{
    for (uint32_t ch = 0; ch < AUDIO_TRACKS; ch++) {
        struct track *t = &tracks[ch];
        uint32_t up = t->param >> 4, down = t->param & 0xF;

        switch (t->effect) {
        case 0x1:
            if (!t->period)
                break;
            set_period(ch, t, t->period > PERIOD_MIN + t->param
                                  ? t->period - t->param : PERIOD_MIN);
            break;
        case 0xA:
            /* ProTracker slides up whenever x is set, ignoring y. */
            if (up)
                t->volume = t->volume + up < VOLUME_MAX
                                ? t->volume + up : VOLUME_MAX;
            else
                t->volume = t->volume > down ? t->volume - down : 0;
            set_volume(ch, t);
            break;
        }
    }
}

static void advance(void)
{
    if (jump) {
        order = jump - 1;
        row = 0;
        jump = 0;
    } else if (++row == ROWS) {
        row = 0;
        order++;
    }
    if (order >= song->length)
        order = song->restart;
}

void rt_music_irq(void)
{
    if (!running)
        return;

    if (tick == 0) {
        position = order << 8 | row;
        play_row();
    } else {
        play_effects();
    }

    for (uint32_t ch = 0; ch < AUDIO_TRACKS; ch++)
        push(ch, &tracks[ch], AU_CH(ch, PA_C_LEVEL));

    if (++tick >= speed) {
        tick = 0;
        advance();
    }
}

int rt_music_start(void)
{
    if ((const uint8_t *)&song_start == song_end)
        return 0;

    song = &song_start;
    if (song->magic != SONG_MAGIC)
        rt_panic("song blob is not from tools/mod2song.py");
    cells = (const uint32_t *)((const uint8_t *)song + song->patterns);

    /* 8-bit to 16-bit, the same value in both lanes, so a refill is a
     * straight word copy. */
    const uint8_t *b = (const uint8_t *)song + song->samples[0].data;
    const uint8_t *b_end = (const uint8_t *)song + song->patterns;
    uint32_t *w = rt_alloc((uint32_t)(b_end - b) * 4);
    frames = w;
    while (b < b_end)
        *w++ = *b++ * 0x01000100u;

    speed = 6;
    for (uint32_t ch = 0; ch < AUDIO_TRACKS; ch++)
        AU_CH(ch, PA_C_VOL) = 0;

    /* Four full-scale tracks sum to exactly full scale: no clipping. */
    AU_REG(PA_MASTER_VOL) = 0x40004000u;
    AU_REG(PA_CHAN_EN)    = (1u << AUDIO_TRACKS) - 1;
    AU_REG(PA_CTRL)       = PA_CTRL_ENABLE;

    running = 1;
    return 1;
}

uint32_t rt_music_position(void)
{
    return position;
}
