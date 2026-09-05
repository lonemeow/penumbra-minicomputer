/*
 * entry.c — bare-metal exercise for the CLASS_AUDIO device.
 *
 * First contact between the CPU and the audio device.  Everything
 * below the register interface has been proven in simulation and on
 * the bring-up probe, but the register protocol itself — the bus
 * attach, the FIFO port, the frame handoff to the output clock — has
 * only ever run against a testbench.  A flat polled program says which
 * of those is wrong; the same fault inside the kernel is silence in an
 * interrupt handler with the whole audio(4) stack equally suspect.
 *
 * Three phases.  Discovery and the protocol checks report over the
 * UART and are machine-checkable.  A channel-identification burst puts
 * a tone in one side at a time, so a swapped or dead ladder leg is
 * obvious rather than subtle.  Then a clip plays on a loop for a human
 * to judge, reporting what feeding the device cost the CPU — or, when
 * the build linked no material, a plain tone, so the program is useful
 * without anyone having to supply a file.
 *
 * Polled throughout: an interrupt that never fires and a FIFO that
 * never drains sound identical, and only the polling loop tells them
 * apart.
 */

#include <stdint.h>
#include "bench.h"
#include "audio.h"

/* ── Bootdata layout (mirror of hw/rom/bootdata.h) ────────────── */
#define BOOTDATA_BASE       0x00000040
#define BTAG_END            0
#define BTAG_DEVICE         2
#define ACFG_CLASS_AUDIO    7     /* matches penumbra_pkg::ACFG_CLASS_AUDIO */

struct btag_hdr {
    uint32_t type;
    uint32_t size;
};

struct btag_device {
    struct btag_hdr hdr;
    uint32_t cls;
    uint32_t base;
    uint32_t dev_size;
    uint32_t id;
    char     name[16];
};

/* How many times through the clip before handing the console back. */
#define PLAY_PASSES         3

/* Identification tone: half a second per side, low enough to leave the
 * ladder's own noise floor audible underneath. */
#define IDENT_HZ            440
#define IDENT_MS            500
#define IDENT_ATTEN         3     /* bits, see tri() */

/* Stand-in when the build linked no clip. */
#define TONE_ONLY_MS        3000

static uint32_t au_base;
static uint32_t fifo_depth;
static uint32_t rate_hz;

#define REG(off)   (*(volatile uint32_t *)(au_base + (off)))
#define CH(off)    REG(PA_CHAN(0) + (off))

/* The clip, linked in by sample.S.  One word is one frame. */
extern const uint32_t audio_clip[];
extern const uint32_t audio_clip_end[];

static uint32_t clip_frames;

/* ── Reporting ────────────────────────────────────────────────── */
static int failures;

static void print_hex(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    bench_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        bench_putchar(digits[(v >> i) & 0xF]);
}

static void check_eq(uint32_t got, uint32_t want, const char *what)
{
    if (got == want)
        return;
    bench_puts("  FAIL ");
    bench_puts(what);
    bench_puts(": got ");
    print_hex(got);
    bench_puts(" want ");
    print_hex(want);
    bench_puts("\r\n");
    failures++;
}

/* ── Discovery ────────────────────────────────────────────────── */
static const struct btag_device *find_audio(uint32_t bootdata)
{
    /* Past the bootdata header: magic, version, total_size. */
    uint32_t p = (bootdata ? bootdata : BOOTDATA_BASE) + 12;

    for (;;) {
        const struct btag_hdr *h = (const struct btag_hdr *)p;
        if (h->type == BTAG_END)
            return 0;
        /* An entry that does not advance the cursor would spin here
         * forever, so treat a malformed list as no device rather than
         * a hang with nothing on the console to say why. */
        if (h->size < sizeof(struct btag_hdr))
            return 0;
        if (h->type == BTAG_DEVICE) {
            const struct btag_device *d = (const struct btag_device *)p;
            if (d->cls == ACFG_CLASS_AUDIO)
                return d;
        }
        p += h->size;
    }
}

/* ── Device state ─────────────────────────────────────────────── */
static void wait_us(uint32_t us)
{
    uint32_t ticks = bench_us_to_ticks(us);

    bench_timer_start();
    while (bench_timer_elapsed_ticks() < ticks)
        ;
}

/*
 * Silence the device and drop every sticky flag.  The program may be
 * re-run with `go` without an intervening reset, so nothing may assume
 * a register still holds its reset value.
 */
static void quiesce(void)
{
    REG(PA_CTRL)       = 0;
    REG(PA_CHAN_EN)    = 0;
    CH(PA_C_CTRL)      = PA_C_CTRL_FLUSH;
    CH(PA_C_WM)        = 0;
    REG(PA_IRQ_ENABLE) = 0;
    REG(PA_IRQ_STATUS) = ~0u;
    REG(PA_UNDERRUN)   = ~0u;
}

/* Wait for everything queued to reach the output stage. */
static void drain(void)
{
    while (CH(PA_C_LEVEL) != fifo_depth)
        ;
}

/* ── Protocol checks ──────────────────────────────────────────── */
static void check_protocol(uint32_t cap)
{
    check_eq(PA_CAP_VERSION(cap), 1, "CAP version");
    check_eq(PA_CAP_CHANNELS(cap) >= 1, 1, "CAP declares a channel");
    check_eq(rate_hz != 0, 1, "RATE is non-zero");

    /* Unimplemented optional registers read their unity values, so a
     * consumer that reads no capability bits still plays at the right
     * pitch and full volume rather than silently wrong. */
    if (!(cap & PA_CAP_PITCH)) {
        CH(PA_C_INC) = PA_INC_UNITY / 2;
        check_eq(CH(PA_C_INC), PA_INC_UNITY, "INC unity without CAP.PITCH");
    }
    if (!(cap & PA_CAP_VOLUME)) {
        CH(PA_C_VOL) = 0;
        check_eq(CH(PA_C_VOL), PA_VOL_UNITY, "VOL unity without CAP.VOLUME");
        REG(PA_MASTER_VOL) = 0;
        check_eq(REG(PA_MASTER_VOL), PA_VOL_UNITY,
                 "MASTER_VOL unity without CAP.VOLUME");
    }

    REG(PA_CTRL) = PA_CTRL_ENABLE;
    check_eq(REG(PA_CTRL) & PA_CTRL_ENABLE, PA_CTRL_ENABLE, "CTRL.ENABLE sets");
    REG(PA_CTRL) = 0;
    check_eq(REG(PA_CTRL) & PA_CTRL_ENABLE, 0, "CTRL.ENABLE clears");

    /* ENABLE gates draining, so the queue holds still while these run. */
    check_eq(CH(PA_C_LEVEL), fifo_depth, "LEVEL is DEPTH when empty");

    for (uint32_t i = 0; i < 64; i++)
        CH(PA_C_FIFO) = 0;
    check_eq(CH(PA_C_LEVEL), fifo_depth - 64, "LEVEL tracks 64 writes");

    CH(PA_C_CTRL) = PA_C_CTRL_FLUSH;
    check_eq(CH(PA_C_LEVEL), fifo_depth, "FLUSH empties the queue");

    /* Frames past LEVEL are discarded; a queue that wrapped instead
     * would play the overrun as a glitch rather than dropping it. */
    for (uint32_t i = 0; i < fifo_depth + 16; i++)
        CH(PA_C_FIFO) = 0;
    check_eq(CH(PA_C_LEVEL), 0, "overrun writes are discarded");
    CH(PA_C_CTRL) = PA_C_CTRL_FLUSH;

    /* WM of 0 disables the flag even though free space trivially meets
     * it — otherwise a consumer could never mask the interrupt. */
    REG(PA_IRQ_STATUS) = ~0u;
    check_eq(REG(PA_IRQ_STATUS), 0, "WM of 0 raises no flag");

    CH(PA_C_WM) = fifo_depth;
    check_eq(REG(PA_IRQ_STATUS) & 1, 1, "watermark flag sets at WM");

    CH(PA_C_WM) = 0;
    REG(PA_IRQ_STATUS) = ~0u;
    check_eq(REG(PA_IRQ_STATUS) & 1, 0, "watermark flag clears on write-1");

    /* An enabled channel with nothing queued is the condition UNDERRUN
     * exists to make visible. */
    REG(PA_UNDERRUN) = ~0u;
    REG(PA_CHAN_EN)  = 1;
    REG(PA_CTRL)     = PA_CTRL_ENABLE;
    wait_us(2000);
    check_eq(REG(PA_UNDERRUN) & 1, 1, "UNDERRUN reports a dry channel");

    REG(PA_CTRL)     = 0;
    REG(PA_CHAN_EN)  = 0;
    REG(PA_UNDERRUN) = ~0u;
    check_eq(REG(PA_UNDERRUN) & 1, 0, "UNDERRUN clears on write-1");
}

/* ── Channel identification ───────────────────────────────────── */
/*
 * A triangle needs no table and no libm, and its harmonics fall away
 * fast enough to stay a tone rather than a buzz.  Phase is 16.16; only
 * the fractional half selects the position in the cycle.
 *
 * Attenuated to sit under the music rather than over it. A triangle's
 * crest factor is about 4.8 dB against roughly 12 dB for recorded
 * material, so matching peak levels would leave the tone far louder to
 * a listener; dropping it three bits puts the two in the same range.
 */
static uint32_t tri(uint32_t phase)
{
    uint32_t p = phase & 0xFFFF;
    int32_t v = (p < 0x8000) ? (int32_t)(p * 2) - 0x8000
                             : 0x17FFF - (int32_t)(p * 2);

    return (uint32_t)(uint16_t)(int16_t)(v >> IDENT_ATTEN);
}

/*
 * Returns how long the device took to consume the tone, in ms.
 *
 * That number is the whole digital path measured end to end: the loop
 * can only retire frames as fast as the output stage frees space, so a
 * result near `ms` proves frames are leaving at RATE. Silence with a
 * correct duration puts the fault after the frame output — in the
 * modulator, the ladder or the wiring — and silence with a wrong one
 * puts it before.
 */
static uint32_t tone(int on_left, int on_right, uint32_t ms)
{
    uint32_t inc    = (IDENT_HZ * 65536u) / rate_hz;
    uint32_t frames = (rate_hz / 1000) * ms;
    uint32_t phase  = 0;
    uint32_t ticks;

    REG(PA_CHAN_EN) = 1;
    REG(PA_CTRL)    = PA_CTRL_ENABLE;

    bench_timer_start();
    while (frames) {
        uint32_t room = CH(PA_C_LEVEL);

        while (room && frames) {
            uint32_t s = tri(phase);
            CH(PA_C_FIFO) = (on_left ? s : 0) | (on_right ? s << 16 : 0);
            phase += inc;
            room--;
            frames--;
        }
    }
    drain();
    ticks = bench_timer_elapsed_ticks();

    REG(PA_CTRL)    = 0;
    REG(PA_CHAN_EN) = 0;

    return ticks / (bench_timer_freq_hz() / 1000);
}

static void identify_channels(void)
{
    uint32_t took;

    bench_puts("  left only...\r\n");
    tone(1, 0, IDENT_MS);
    bench_puts("  right only...\r\n");
    tone(0, 1, IDENT_MS);
    bench_puts("  both...\r\n");
    took = tone(1, 1, IDENT_MS);

    bench_puts("  consumed ");
    bench_print_uint(IDENT_MS);
    bench_puts(" ms of frames in ");
    bench_print_uint(took);
    bench_puts(" ms\r\n");
    if (took < IDENT_MS - IDENT_MS / 8 || took > IDENT_MS + IDENT_MS / 8) {
        bench_puts("  drain rate is wrong — the fault is before the pins\r\n");
        failures++;
    }
}

/* ── Playback ─────────────────────────────────────────────────── */
/*
 * Top the channel up from the clip.
 *
 * `*pos` is the read cursor in frames and wraps at clip_frames; the
 * generator fades the clip's ends into each other, so the wrap is
 * silent.  One word is one frame — 16-bit stereo, left in the low half
 * — so frames go into the port with no repacking.
 *
 * Returns the number of frames written, which the caller uses both to
 * decide when the clip has played out and to detect a refill that made
 * no progress.
 */
static uint32_t refill(uint32_t *pos)
{
    /* Half the queue: slack for a late refill, without sitting brim
     * full where an underrun would need the consumer to vanish
     * entirely before it showed up. */
    uint32_t target = fifo_depth / 2;
    uint32_t queued = fifo_depth - CH(PA_C_LEVEL);
    uint32_t to_write;
    uint32_t until_wrap;

    if (queued >= target)
        return 0;

    to_write = target - queued;
    until_wrap = clip_frames - *pos;
    if (to_write > until_wrap)
        to_write = until_wrap;      /* the rest arrives next call */

    for (uint32_t i = 0; i < to_write; i++)
        CH(PA_C_FIFO) = audio_clip[*pos + i];

    *pos += to_write;
    if (*pos == clip_frames)
        *pos = 0;

    return to_write;
}

static void play_clip(void)
{
    uint32_t pos       = 0;
    uint32_t written   = 0;
    uint32_t remaining = clip_frames * PLAY_PASSES;
    uint32_t busy      = 0;
    uint32_t elapsed, played_ms, busy_ms;

    quiesce();

    /* Prime before enabling: a channel started dry underruns on its
     * first output frame, which is the one thing this program should
     * not do by accident. */
    while (CH(PA_C_LEVEL) && refill(&pos))
        ;

    REG(PA_UNDERRUN) = ~0u;
    REG(PA_CHAN_EN)  = 1;
    REG(PA_CTRL)     = PA_CTRL_ENABLE;

    /*
     * Only productive refills are counted. This loop does nothing but
     * poll, so most calls find the queue already at target and return
     * having done no work; charging those to the device would measure
     * the polling rather than the copying. What carries over to a
     * driver is the cost of the frames it actually moves.
     */
    bench_timer_start();
    while (written < remaining) {
        uint32_t t0 = bench_timer_elapsed_ticks();
        uint32_t n  = refill(&pos);
        uint32_t dt = bench_timer_elapsed_ticks() - t0;

        if (n) {
            busy    += dt;
            written += n;
        } else if (CH(PA_C_LEVEL) == fifo_depth) {
            break;              /* nothing queued and nothing to queue */
        }
    }
    elapsed = bench_timer_elapsed_ticks();

    drain();
    REG(PA_CTRL)    = 0;
    REG(PA_CHAN_EN) = 0;

    /* Against the audio's own duration, not this loop's runtime: the
     * share of the machine that feeding the device would take from
     * anything running alongside it. */
    played_ms = written / (rate_hz / 1000);
    busy_ms   = busy / (bench_timer_freq_hz() / 1000);

    bench_puts("  fed ");
    bench_print_uint(written);
    bench_puts(" frames in ");
    bench_print_uint(elapsed / (bench_timer_freq_hz() / 1000));
    bench_puts(" ms, copying cost ");
    bench_print_uint(played_ms ? (busy_ms * 100) / played_ms : 0);
    bench_puts("% of real time\r\n");

    if (REG(PA_UNDERRUN) & 1) {
        bench_puts("  UNDERRUN during playback\r\n");
        failures++;
    }
}

/* ── Entry ────────────────────────────────────────────────────── */
void bench_main(uint32_t bootdata)
{
    bench_init();
    bench_puts("\r\n=== Penumbra audio test ===\r\n");

    const struct btag_device *d = find_audio(bootdata);
    if (d == 0) {
        bench_puts("  no CLASS_AUDIO device in the boot data\r\n");
        return;
    }
    au_base = d->base;

    uint32_t cap = REG(PA_CAP);
    rate_hz    = REG(PA_RATE);
    fifo_depth = CH(PA_C_DEPTH);
    clip_frames = (uint32_t)(audio_clip_end - audio_clip);

    bench_puts("  audio at ");
    print_hex(au_base);
    bench_puts("  CAP ");
    print_hex(cap);
    bench_puts("\r\n  rate ");
    bench_print_uint(rate_hz);
    bench_puts(" Hz, channels ");
    bench_print_uint(PA_CAP_CHANNELS(cap));
    bench_puts(", FIFO ");
    bench_print_uint(fifo_depth);
    bench_puts(" frames, clip ");
    bench_print_uint(clip_frames / (rate_hz / 1000));
    bench_puts(" ms\r\n");

    quiesce();
    check_protocol(cap);
    quiesce();

    bench_puts("  identifying channels\r\n");
    identify_channels();

    /* Real material is the point of the listening test, but it is the
     * user's file and never this tree's, so the default build has none
     * and a tone stands in. */
    if (clip_frames) {
        bench_puts("  playing clip\r\n");
        play_clip();
    } else {
        bench_puts("  no clip linked in — tone only"
                   " (rebuild with AUDIO_WAV=<file> for material)\r\n");
        (void)tone(1, 1, TONE_ONLY_MS);
    }

    if (failures == 0)
        bench_puts("  PASS\r\n");
    else {
        bench_puts("  FAILURES: ");
        bench_print_uint((uint32_t)failures);
        bench_puts("\r\n");
    }
}
