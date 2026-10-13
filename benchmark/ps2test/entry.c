/*
 * entry.c — bare-metal exercise for the CLASS_PS2 port.
 *
 * The port's registers are proven in simulation, against a model of a
 * keyboard.  What sits on a real port is not that model: on the Nexys
 * Video it is the board's USB bridge, which the manual describes only
 * in outline.  This program asks the device, and prints what it does.
 *
 * The ROM monitor is the wrong instrument for that.  It reads a byte
 * at a time, and any read of RX_DATA takes the byte waiting there, so
 * it cannot show a byte beside its own flags, and a person typing
 * commands cannot say when a byte arrived.  Here every read is of the
 * whole word, and every byte is stamped as it is taken.
 *
 * Seven phases, reported over the UART:
 *
 *   waiting   whatever the device had sent before the program ran
 *   commands  one byte to the device at a time: how long the port was
 *             busy, whether the device took it, and all it sent back
 *   soak      the same commands many times over, a symbol to each: how
 *             often the device answers, and whether it fails in runs
 *   resend    a driver's commands with each byte sent again when
 *             nothing answers: whether that is enough
 *   after a reset  the first commands behind one, which is where a
 *             driver sends its own
 *   keys      every byte the device sends while keys are typed
 *   hold      nothing read while keys are typed, then everything the
 *             device kept meanwhile
 *
 * Polled throughout.  Nothing here judges the device: what a keyboard
 * should answer belongs to its command set, and what this prints is for
 * a person to read against it.
 */

#include <stdint.h>
#include "bench.h"

/* ── Bootdata layout (mirror of hw/rom/bootdata.h) ────────────── */
#define BOOTDATA_BASE       0x00000040
#define BTAG_END            0
#define BTAG_DEVICE         2
#define ACFG_CLASS_PS2      9     /* matches penumbra_pkg::ACFG_CLASS_PS2 */

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

/* ── The port's registers (doc/system/devices/ps2.md) ─────────── */
#define PS2_CAP             0x00
#define PS2_IRQ_ENABLE      0x08
#define PS2_CTRL            0x0C
#define PS2_RX_DATA         0x10
#define PS2_TX_DATA         0x14

#define PS2_CTRL_EN         0x001
#define PS2_RX_VALID        0x100
#define PS2_RX_ERR          0x200
#define PS2_TX_BUSY         0x100
#define PS2_TX_ERR          0x200

/* How long each phase runs. */
#define WAITING_US          500000
#define SOAK_WAIT_US        8000
#define SOAK_RESET_WAIT_US  600000
#define KEYS_US             10000000
#define KEYS_SLICE_US       100000
#define HOLD_SETTLE_US      1000000
#define HOLD_US             5000000
#define HOLD_DRAIN_US       2000000

/* The interface ends a transmission inside 20 ms.  Waiting longer
 * shows a port that breaks the promise, where waiting just as long
 * would only show one that kept it late. */
#define TX_WAIT_US          30000

static uint32_t ps2_base;

#define REG(off)   (*(volatile uint32_t *)(ps2_base + (off)))

/* ── Reporting ────────────────────────────────────────────────── */
static void print_hex(uint32_t v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";

    for (int i = 4 * (digits - 1); i >= 0; i -= 4)
        bench_putchar(hex[(v >> i) & 0xF]);
}

/* Timer ticks as milliseconds, to the microsecond. */
static void print_ms(uint32_t ticks)
{
    bench_print_milli((uint64_t)ticks * 1000000u / bench_timer_freq_hz());
    bench_puts(" ms");
}

/* ── Discovery ────────────────────────────────────────────────── */
static const struct btag_device *find_port(uint32_t bootdata)
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
            if (d->cls == ACFG_CLASS_PS2)
                return d;
        }
        p += h->size;
    }
}

/* ── Received bytes ───────────────────────────────────────────── */
/* RX_DATA as read with VALID set, and the timer when it was read. */
struct rx_entry {
    uint32_t rx;
    uint32_t at;
};

#define RX_LOG_MAX          256

static struct rx_entry rx_log[RX_LOG_MAX];
static uint32_t        rx_logged;

/*
 * Take every byte that arrives in the next `us` into the log.  The
 * caller starts the timer, so the stamps count from whatever it means
 * to time.  Nothing is printed meanwhile: a line on the UART takes
 * milliseconds, and a byte that arrived behind one would be stamped
 * late.  A full log ends the wait early.
 */
static void collect(uint32_t us)
{
    uint32_t until = bench_timer_elapsed_ticks() + bench_us_to_ticks(us);

    while (bench_timer_elapsed_ticks() < until && rx_logged < RX_LOG_MAX) {
        uint32_t rx = REG(PS2_RX_DATA);

        if (rx & PS2_RX_VALID) {
            rx_log[rx_logged].rx = rx;
            rx_log[rx_logged].at = bench_timer_elapsed_ticks();
            rx_logged++;
        }
    }
}

/* Print the log a byte to a line with its time, and empty it. */
static void report_timed(const char *if_none)
{
    for (uint32_t i = 0; i < rx_logged; i++) {
        bench_puts("    ");
        print_hex(rx_log[i].rx & 0xFF, 2);
        bench_puts((rx_log[i].rx & PS2_RX_ERR) ? " ERR  at " : "      at ");
        print_ms(rx_log[i].at);
        bench_puts("\r\n");
    }
    if (rx_logged == 0)
        bench_puts(if_none);
    rx_logged = 0;
}

/*
 * Print the log as a run of bytes, sixteen to a line, and empty it.
 * A byte whose frame was malformed is followed by '!'.  `column` is
 * where the run stands on its line, kept by the caller across calls.
 */
static void report_run(uint32_t *column)
{
    for (uint32_t i = 0; i < rx_logged; i++) {
        if (*column == 0)
            bench_puts("    ");
        print_hex(rx_log[i].rx & 0xFF, 2);
        bench_putchar((rx_log[i].rx & PS2_RX_ERR) ? '!' : ' ');
        if (++*column == 16) {
            bench_puts("\r\n");
            *column = 0;
        }
    }
    rx_logged = 0;
}

static void end_run(uint32_t column, uint32_t total)
{
    if (column != 0)
        bench_puts("\r\n");
    bench_puts("    ");
    bench_print_uint(total);
    bench_puts(" bytes\r\n");
}

/* ── Phases ───────────────────────────────────────────────────── */
/*
 * Send one byte, then log what comes back for `reply_us`.  Times count
 * from the write.  Returns TX_DATA as it read once the port was done,
 * and in `busy` how long that took.
 */
static uint32_t exchange(uint8_t byte, uint32_t reply_us, uint32_t *busy)
{
    uint32_t limit = bench_us_to_ticks(TX_WAIT_US);
    uint32_t tx;

    bench_timer_start();
    REG(PS2_TX_DATA) = byte;
    do {
        tx = REG(PS2_TX_DATA);
        *busy = bench_timer_elapsed_ticks();
    } while ((tx & PS2_TX_BUSY) && *busy < limit);
    collect(reply_us);
    return tx;
}

static void command(uint8_t byte, const char *what, uint32_t reply_us)
{
    uint32_t tx, busy;

    bench_puts("  send ");
    print_hex(byte, 2);
    bench_puts("  ");
    bench_puts(what);
    bench_puts("\r\n");

    tx = exchange(byte, reply_us, &busy);

    bench_puts("    port busy ");
    print_ms(busy);
    if (tx & PS2_TX_BUSY)
        bench_puts(", and still: the interface allows 20 ms\r\n");
    else if (tx & PS2_TX_ERR)
        bench_puts(", ERR: nothing took the byte\r\n");
    else
        bench_puts(", the byte taken\r\n");
    report_timed("    no reply\r\n");
}

/* The commands a keyboard driver depends on, once each, with times. */
static void commands(void)
{
    bench_puts("commands\r\n");
    command(0xEE, "echo", 100000);
    command(0xF4, "enable", 200000);
    command(0xFF, "reset", 1000000);
    command(0xF2, "read ID", 200000);
    command(0xED, "set LEDs", 200000);
    command(0x00, "all three dark", 200000);
}

/* ── Soak ─────────────────────────────────────────────────────── */
/*
 * One byte to send, the bytes a keyboard answers it with, and how long
 * to wait for them.
 */
struct soak_step {
    uint8_t        byte;
    const uint8_t *want;
    uint32_t       want_n;
    uint32_t       wait_us;
};

static const uint8_t want_echo[]   = { 0xEE };
static const uint8_t want_ack[]    = { 0xFA };
static const uint8_t want_id[]     = { 0xFA, 0xAB, 0x83 };
static const uint8_t want_reset[]  = { 0xFA, 0xAA };
static const uint8_t want_resend[] = { 0xFE };

#define STEP(byte, want)    { (byte), (want), sizeof(want), SOAK_WAIT_US }
#define RESET_STEP          { 0xFF, want_reset, sizeof(want_reset), SOAK_RESET_WAIT_US }

/* Whether the log holds exactly the answer a keyboard gives. */
static int answered(const struct soak_step *step)
{
    if (rx_logged != step->want_n)
        return 0;
    for (uint32_t i = 0; i < rx_logged; i++) {
        /* VALID set, ERR clear, and the byte. */
        if ((rx_log[i].rx & 0x3FF) != (PS2_RX_VALID | step->want[i]))
            return 0;
    }
    return 1;
}

/*
 * What became of one exchange, as a symbol, emptying the log:
 *   .  the answer a keyboard gives     -  no answer
 *   r  FE alone, where it was not due  ?  anything else
 *   !  the port did not get the byte across
 */
static char outcome(uint32_t tx, const struct soak_step *step)
{
    char c;

    if (tx & (PS2_TX_BUSY | PS2_TX_ERR))
        c = '!';
    else if (answered(step))
        c = '.';
    else if (rx_logged == 0)
        c = '-';
    else if (rx_logged == 1 && (rx_log[0].rx & 0x3FF) == (PS2_RX_VALID | 0xFE))
        c = 'r';
    else
        c = '?';
    rx_logged = 0;
    return c;
}

/*
 * A wait that differs each time, by up to a millisecond: with it a run
 * of exchanges does not walk in step with anything the device does on
 * a period of its own.
 */
static uint32_t jitter_us(void)
{
    static uint32_t lcg = 0x2468ACE1u;

    lcg = lcg * 1664525u + 1013904223u;
    return lcg >> 22;
}

/*
 * The steps in turn, `rounds` times over, a symbol to each exchange,
 * and at the end of the row how many were not answered as a keyboard
 * answers.  A device that drops what it is sent shows here as more
 * than an anecdote: how often, in runs or alone, and of which commands.
 */
static void soak_row(const char *label, const struct soak_step *steps, uint32_t nsteps,
                     uint32_t rounds)
{
    uint32_t missed = 0;

    bench_puts("  ");
    bench_puts(label);
    for (uint32_t r = 0; r < rounds; r++) {
        for (uint32_t s = 0; s < nsteps; s++) {
            uint32_t busy;
            uint32_t tx = exchange(steps[s].byte, steps[s].wait_us + jitter_us(), &busy);
            char c = outcome(tx, &steps[s]);

            if (c != '.')
                missed++;
            bench_putchar(c);
        }
        if (nsteps > 2)
            bench_putchar(' ');
    }
    bench_puts("  ");
    bench_print_uint(missed);
    bench_puts("\r\n");
}

static void soak(void)
{
    static const struct soak_step echo[]    = { STEP(0xEE, want_echo) };
    static const struct soak_step id[]      = { STEP(0xF2, want_id) };
    static const struct soak_step enable[]  = { STEP(0xF4, want_ack) };
    static const struct soak_step invalid[] = { STEP(0x00, want_resend) };
    static const struct soak_step leds[]    = { STEP(0xED, want_ack), STEP(0x00, want_ack) };
    static const struct soak_step reset[]   = { RESET_STEP };

    bench_puts("soak: . answered  - no answer  r FE  ? other  ! not sent;"
               " misses at the end of a row\r\n");
    soak_row("EE     ", echo, 1, 64);
    soak_row("F2     ", id, 1, 64);
    soak_row("F4     ", enable, 1, 64);
    soak_row("00     ", invalid, 1, 64);
    soak_row("ED 00  ", leds, 2, 32);
    soak_row("FF     ", reset, 1, 8);
}

/*
 * A reset, then one command three times over: whether the first
 * command behind a reset fares worse than the rest, and whether asking
 * again helps.  A driver sends its first commands right there.
 */
static void after_reset(void)
{
    static const struct soak_step echo[] = {
        RESET_STEP, STEP(0xEE, want_echo), STEP(0xEE, want_echo), STEP(0xEE, want_echo),
    };
    static const struct soak_step id[] = {
        RESET_STEP, STEP(0xF2, want_id), STEP(0xF2, want_id), STEP(0xF2, want_id),
    };
    static const struct soak_step enable[] = {
        RESET_STEP, STEP(0xF4, want_ack), STEP(0xF4, want_ack), STEP(0xF4, want_ack),
    };
    static const struct soak_step leds[] = {
        RESET_STEP, STEP(0xED, want_ack), STEP(0x00, want_ack),
        STEP(0xED, want_ack), STEP(0x00, want_ack),
    };

    bench_puts("after a reset: FF, then the command, in groups\r\n");
    soak_row("FF EE EE EE     ", echo, 4, 8);
    soak_row("FF F2 F2 F2     ", id, 4, 8);
    soak_row("FF F4 F4 F4     ", enable, 4, 8);
    soak_row("FF ED 00 ED 00  ", leds, 5, 8);
}

/* ── Resend ───────────────────────────────────────────────────── */
/*
 * A keyboard answers every byte it is sent, inside 20 ms.  So a byte
 * that nothing has answered by then was lost, and a host may send it
 * again.
 */
#define RESEND_WAIT_US      25000
#define RESEND_TRIES        4
#define RESEND_ACK          (PS2_RX_VALID | 0xFA)

/* The next byte to arrive within `us`, with its flags, or -1. */
static int wait_byte(uint32_t us)
{
    uint32_t until = bench_timer_elapsed_ticks() + bench_us_to_ticks(us);

    do {
        uint32_t rx = REG(PS2_RX_DATA);

        if (rx & PS2_RX_VALID)
            return (int)(rx & 0x3FF);
    } while (bench_timer_elapsed_ticks() < until);
    return -1;
}

/*
 * Send one byte as a driver for a device that loses some would: again
 * whenever nothing answers.  Returns the answer's first byte, or -1
 * when every try went unanswered, and counts the tries past the first
 * in `resent`.
 */
static int send_resending(uint8_t byte, uint32_t *resent)
{
    uint32_t limit = bench_us_to_ticks(TX_WAIT_US);

    for (int tries = 0; tries < RESEND_TRIES; tries++) {
        int answer;

        if (tries)
            (*resent)++;
        bench_timer_start();
        REG(PS2_TX_DATA) = byte;
        while ((REG(PS2_TX_DATA) & PS2_TX_BUSY) && bench_timer_elapsed_ticks() < limit)
            ;
        answer = wait_byte(RESEND_WAIT_US);
        if (answer >= 0)
            return answer;
    }
    return -1;
}

static void resend_report(const char *what, uint32_t count, uint32_t resent, uint32_t failed)
{
    bench_puts("  ");
    bench_puts(what);
    bench_print_uint(count);
    bench_puts(" times: ");
    bench_print_uint(resent);
    bench_puts(" bytes sent again, ");
    bench_print_uint(failed);
    bench_puts(" commands failed\r\n");
}

/*
 * The three commands a keyboard driver cannot do without, many times
 * over, each byte sent again when nothing answers: whether sending
 * again is enough for a device that loses bytes, which is what decides
 * if a driver can use it at all.  A command has failed when a byte of
 * it went unanswered on every try, or was answered with anything but
 * an acknowledgement, or a reset's self-test result did not follow.
 */
static void resend(void)
{
    uint32_t resent, failed;

    bench_puts("resend: each byte sent again when nothing answers in 25 ms, 4 tries\r\n");

    resent = failed = 0;
    for (int i = 0; i < 400; i++) {
        if (send_resending(0xF4, &resent) != RESEND_ACK)
            failed++;
    }
    resend_report("enable, F4, ", 400, resent, failed);

    resent = failed = 0;
    for (int i = 0; i < 400; i++) {
        if (send_resending(0xED, &resent) != RESEND_ACK ||
            send_resending(0x00, &resent) != RESEND_ACK)
            failed++;
    }
    resend_report("set LEDs, ED 00, ", 400, resent, failed);

    resent = failed = 0;
    for (int i = 0; i < 40; i++) {
        if (send_resending(0xFF, &resent) != RESEND_ACK ||
            wait_byte(1000000) != (int)(PS2_RX_VALID | 0xAA))
            failed++;
    }
    resend_report("reset, FF, ", 40, resent, failed);

    /* Whatever a failed command left behind. */
    bench_timer_start();
    collect(100000);
    rx_logged = 0;
}

static void keys(void)
{
    uint32_t column = 0, total = 0;

    bench_puts("keys: type for 10 s\r\n");
    bench_timer_start();
    for (uint32_t t = 0; t < KEYS_US; t += KEYS_SLICE_US) {
        collect(KEYS_SLICE_US);
        total += rx_logged;
        report_run(&column);
    }
    end_run(column, total);
}

/*
 * The port holds the device off behind the first byte nobody takes,
 * and a device held off is meant to keep what it has to send.  Ten
 * taps of one key are thirty bytes: its code, then F0 and its code
 * again.
 */
static void hold(void)
{
    uint32_t until = bench_us_to_ticks(HOLD_US);
    uint32_t column = 0, total;

    /* The last keys of the phase before are still coming up, and what
     * they send is not part of this count. */
    bench_puts("hold: hands off the keys for a second\r\n");
    bench_timer_start();
    collect(HOLD_SETTLE_US);
    rx_logged = 0;

    bench_puts("hold: tap one key ten times now, nothing is read for 5 s\r\n");
    bench_timer_start();
    while (bench_timer_elapsed_ticks() < until)
        ;
    bench_puts("hold: stop, reading what the device kept\r\n");

    bench_timer_start();
    collect(HOLD_DRAIN_US);
    total = rx_logged;
    report_run(&column);
    end_run(column, total);
}

void bench_main(uint32_t bootdata)
{
    bench_init();
    bench_puts("\r\n=== Penumbra PS/2 port test ===\r\n");

    const struct btag_device *d = find_port(bootdata);
    if (d == 0) {
        bench_puts("  no CLASS_PS2 device in the boot data\r\n");
        return;
    }
    ps2_base = d->base;

    bench_puts("  port at 0x");
    print_hex(ps2_base, 8);
    bench_puts("  CAP 0x");
    print_hex(REG(PS2_CAP), 8);
    bench_puts("\r\n");

    /* CTRL.EN is set without being cleared first: clearing it would
     * discard a byte already waiting, which is the first thing worth
     * seeing.  A device held off keeps what it had to send, through a
     * reset of the machine too, and hands it over a frame at a time
     * once the port reads: so this waits long enough for all of it. */
    REG(PS2_IRQ_ENABLE) = 0;
    REG(PS2_CTRL) = PS2_CTRL_EN;

    bench_puts("waiting at the start\r\n");
    bench_timer_start();
    collect(WAITING_US);
    report_timed("    nothing\r\n");

    commands();
    soak();
    resend();
    after_reset();
    keys();
    hold();

    bench_puts("done\r\n");
}
