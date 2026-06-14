// sim_console.cpp — the shared interactive frontend (see sim_console.h).
//
// Terminal raw mode, the UART hold-until-ack RX feed, TX→stdout plus the
// +halt_on= matcher, the SD↔SdCardSim bridge, +stdin_file= replay and the
// throttled live-stdin poll, trace-file management (ring buffer / streaming /
// dump-on-exit), and the main loop + exit. Generation-independent: it drives a
// SimCore and never touches a Verilated DUT directly.

#include "sim_console.h"
#include "sd_card_sim.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <termios.h>
#include <poll.h>

static volatile sig_atomic_t running = 1;
static struct termios orig_termios;
static bool term_raw = false;

// 1 byte every STDIN_FILE_PERIOD cycles for +stdin_file= replay — a fixed
// cadence so a scripted boot sees the same byte at the same sim cycle every
// run (live poll() timing is the dominant run-to-run nondeterminism source).
static const uint64_t STDIN_FILE_PERIOD = 100000;  // ~46 char-times at 2170 cyc/char

static void sigint_handler(int) { running = 0; }

static void restore_term() {
    if (term_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        term_raw = false;
    }
}

// Raw mode: no echo, no line buffering (the boot ROM does all character
// processing). ISIG stays enabled so Ctrl-C still raises SIGINT. Skipped for
// piped input so non-interactive runs behave normally.
static void raw_mode() {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_term);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    term_raw = true;
}

ConsoleOpts parse_console_opts(int argc, char** argv) {
    ConsoleOpts o;
    for (int i = 1; i < argc; i++) {
        if      (strncmp(argv[i], "+sdcard=", 8) == 0)        o.sd_path      = argv[i] + 8;
        else if (strncmp(argv[i], "+trace=", 7) == 0)         o.trace_path   = argv[i] + 7;
        else if (strncmp(argv[i], "+trace_window=", 14) == 0) o.trace_ring   = strtoul(argv[i] + 14, nullptr, 10);
        else if (strncmp(argv[i], "+halt_on=", 9) == 0)       o.halt_pattern = argv[i] + 9;
        else if (strncmp(argv[i], "+stdin_file=", 12) == 0)   o.stdin_file   = argv[i] + 12;
    }
    if (!o.sd_path) o.sd_path = getenv("SDCARD_IMG");   // env fallback
    if (o.halt_pattern && !*o.halt_pattern) o.halt_pattern = nullptr;
    return o;
}

int run_console(SimCore& core, const ConsoleOpts& opts) {
    // ── Trace file (only if the core can produce trace lines) ──────────────
    FILE*                    trace_fp = nullptr;
    std::vector<std::string> trace_ring;
    size_t trace_ring_cap = 0, trace_ring_head = 0, trace_ring_count = 0;
    if (opts.trace_path) {
        if (!core.trace_supported()) {
            fprintf(stderr, "[TRACE] not supported on this core — ignoring +trace=\n");
        } else if (!(trace_fp = fopen(opts.trace_path, "w"))) {
            fprintf(stderr, "[TRACE] cannot open '%s'\n", opts.trace_path);
        } else if ((trace_ring_cap = opts.trace_ring) > 0) {
            trace_ring.resize(trace_ring_cap);
            fprintf(stderr, "[TRACE] rolling window of %zu lines -> '%s'\n", trace_ring_cap, opts.trace_path);
        } else {
            fprintf(stderr, "[TRACE] streaming to '%s'\n", opts.trace_path);
        }
    }
    auto trace_write = [&](const std::string& line) {
        if (!trace_fp) return;
        if (trace_ring_cap > 0) {
            trace_ring[trace_ring_head] = line;
            trace_ring_head = (trace_ring_head + 1) % trace_ring_cap;
            if (trace_ring_count < trace_ring_cap) trace_ring_count++;
        } else {
            fputs(line.c_str(), trace_fp);
        }
    };

    // ── +stdin_file= replay source ─────────────────────────────────────────
    FILE* stdin_file_fp = nullptr;
    uint64_t stdin_file_next = 0;
    if (opts.stdin_file) {
        stdin_file_fp = fopen(opts.stdin_file, "rb");
        if (!stdin_file_fp)
            fprintf(stderr, "[STDIN_FILE] cannot open '%s'\n", opts.stdin_file);
        else
            fprintf(stderr, "[STDIN_FILE] replaying '%s' (1 byte / %lu cycles, then live stdin)\n",
                    opts.stdin_file, (unsigned long)STDIN_FILE_PERIOD);
    }
    if (opts.halt_pattern) fprintf(stderr, "[HALT_ON] '%s'\n", opts.halt_pattern);

    SdCardSim sd(opts.sd_path);
    if (sd.is_present()) fprintf(stderr, "[SD] card emulation active\n");

    signal(SIGINT, sigint_handler);
    raw_mode();
    fprintf(stderr, "-- Interactive Monitor (Ctrl-C to exit) --\n\n");
    core.reset();

    bool     rx_pending = false;
    uint8_t  rx_byte = 0;
    uint64_t cycles = 0;
    uint8_t  sd_resp = 0xFF;
    bool     sd_resp_valid = false;
    size_t   halt_match_pos = 0;
    std::vector<std::string> trace_lines;

    while (running) {
        // ── One CPU cycle ──────────────────────────────────────────────────
        core.uart_rx(rx_pending, rx_byte);
        core.tick();
        cycles++;

        // ── Trace lines for this cycle (core-specific generation) ──────────
        if (trace_fp) {
            trace_lines.clear();
            core.trace(trace_lines, cycles);
            for (const std::string& l : trace_lines) trace_write(l);
        }

        // ── UART TX -> stdout (and the +halt_on= streaming matcher) ────────
        uint8_t tx;
        if (core.uart_tx(tx)) {
            putchar((char)tx);
            fflush(stdout);
            if (opts.halt_pattern) {
                if ((char)tx == opts.halt_pattern[halt_match_pos]) {
                    if (opts.halt_pattern[++halt_match_pos] == '\0') {
                        fprintf(stderr, "\n[HALT_ON matched '%s' after %lu cycles]\n",
                                opts.halt_pattern, (unsigned long)cycles);
                        running = 0;
                    }
                } else {
                    halt_match_pos = ((char)tx == opts.halt_pattern[0]) ? 1 : 0;
                }
            }
        }

        // ── RX accepted this cycle? ────────────────────────────────────────
        if (rx_pending && core.uart_rx_ack()) rx_pending = false;

        // ── SD card: bridge the SPI MOSI/MISO bytes to the card model ──────
        sd.select(!core.spi_cs0());           // CS0 active-low
        uint8_t mosi;
        if (core.spi_cmd(mosi)) {
            sd_resp = sd.exchange(mosi);
            sd_resp_valid = true;
        }
        core.spi_resp(sd_resp_valid, sd_resp);

        // ── Deliver next byte from +stdin_file= replay ─────────────────────
        if (!rx_pending && stdin_file_fp && cycles >= stdin_file_next) {
            int c = fgetc(stdin_file_fp);
            if (c == EOF) {
                fclose(stdin_file_fp);
                stdin_file_fp = nullptr;
                fprintf(stderr, "[STDIN_FILE] exhausted, falling back to live stdin\n");
            } else {
                rx_byte = (uint8_t)c;
                rx_pending = true;
                stdin_file_next = cycles + STDIN_FILE_PERIOD;
            }
        }

        // ── Poll live stdin (throttled; ~3.8 polls per char-time) ──────────
        if (!rx_pending && !stdin_file_fp && (cycles & 0x1FFF) == 0) {
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                char c;
                if (read(STDIN_FILENO, &c, 1) == 1) {
                    rx_byte = (uint8_t)c;
                    rx_pending = true;
                }
            }
        }

        // ── BREAK halts the simulation ─────────────────────────────────────
        if (core.halted()) {
            fprintf(stderr, "\n[BREAK after %lu cycles, PC=%x]\n",
                    (unsigned long)cycles, core.pc());
            break;
        }
    }

    if (!core.halted())
        fprintf(stderr, "\n[Interrupted after %lu cycles, PC=0x%08X]\n",
                (unsigned long)cycles, core.pc());

    // ── Flush the trace (dump the rolling window in chronological order) ───
    if (trace_fp) {
        if (trace_ring_cap > 0 && trace_ring_count > 0) {
            size_t start = (trace_ring_count == trace_ring_cap) ? trace_ring_head : 0;
            for (size_t i = 0; i < trace_ring_count; i++)
                fputs(trace_ring[(start + i) % trace_ring_cap].c_str(), trace_fp);
            fprintf(stderr, "[TRACE] dumped %zu lines from rolling window\n", trace_ring_count);
        }
        fclose(trace_fp);
        fprintf(stderr, "[TRACE] done\n");
    }
    restore_term();
    return 0;
}
