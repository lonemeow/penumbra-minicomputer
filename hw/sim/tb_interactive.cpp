// Verilator testbench for Penumbra CPU — interactive terminal
//
// Bridges host stdin/stdout to UART RX/TX for interactive use
// with the boot ROM monitor. No cycle limit — runs until BREAK
// or Ctrl-C (SIGINT).
//
// Terminal is set to raw mode (no echo, no line buffering) so the
// boot ROM handles all character processing. Status messages go
// to stderr to keep stdout clean for UART output.
//
// No VCD tracing — interactive sessions can run for millions of
// cycles. Use `make sim` with tb_cpu_prog for traced runs.
//
// Usage: make interactive

#include <cstdio>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include "Vmachine_sim.h"
#include "sd_card_sim.h"

static volatile sig_atomic_t running = 1;
static struct termios orig_termios;
static bool term_raw = false;
static FILE* trace_fp = nullptr;

// Rolling-window trace state. cap=0 means streaming (write each line
// directly to trace_fp). cap>0 keeps the last `cap` lines in memory
// and dumps them on exit.
static std::vector<std::string> trace_ring;
static size_t trace_ring_cap   = 0;
static size_t trace_ring_head  = 0;
static size_t trace_ring_count = 0;

// Halt-on-UART-pattern matcher. When `halt_pattern` is non-null,
// every UART TX byte is fed through a streaming substring matcher;
// when the pattern matches, we set running=0 so the main loop exits
// cleanly (and dumps the ring buffer if one is configured).
static const char* halt_pattern   = nullptr;
static size_t      halt_match_pos = 0;

static void sigint_handler(int) { running = 0; }

static void restore_term() {
    if (term_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        term_raw = false;
    }
}

static void raw_mode() {
    if (!isatty(STDIN_FILENO)) return;  // piped input, skip
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_term);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);  // no echo, no line buffering
    // ISIG stays enabled so Ctrl-C generates SIGINT
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    term_raw = true;
}

static void reset(Vmachine_sim* cpu) {
    cpu->i_rst = 1;
    cpu->i_irq = 0;
    cpu->i_uart_rx_valid = 0;
    cpu->i_uart_rx_data = 0;
    cpu->i_spi_resp_valid = 0;
    cpu->i_spi_resp_data = 0xFF;
    cpu->i_dbg_reg_addr = 0;
    // Two reset cycles
    cpu->i_clk = 0; cpu->eval();
    cpu->i_clk = 1; cpu->eval();
    cpu->i_clk = 0; cpu->eval();
    cpu->i_clk = 1; cpu->eval();
    cpu->i_rst = 0;
}

// Extract +sdcard=<path> plusarg from command line (via Verilator)
static const char* get_sdcard_path() {
    // Verilator stores plusargs; use environment as fallback
    const char* p = getenv("SDCARD_IMG");
    return p;
}

int main(int argc, char** argv) {
    Vmachine_sim* cpu = new Vmachine_sim;

    // Check for +sdcard=, +trace=, +trace_window=, +halt_on= plusargs
    const char* sd_path = nullptr;
    const char* trace_path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "+sdcard=", 8) == 0)
            sd_path = argv[i] + 8;
        else if (strncmp(argv[i], "+trace=", 7) == 0)
            trace_path = argv[i] + 7;
        else if (strncmp(argv[i], "+trace_window=", 14) == 0)
            trace_ring_cap = strtoul(argv[i] + 14, nullptr, 10);
        else if (strncmp(argv[i], "+halt_on=", 9) == 0)
            halt_pattern = argv[i] + 9;
    }
    if (!sd_path) sd_path = get_sdcard_path();

    if (trace_path) {
        trace_fp = fopen(trace_path, "w");
        if (!trace_fp) {
            fprintf(stderr, "[TRACE] cannot open '%s'\n", trace_path);
        } else if (trace_ring_cap > 0) {
            trace_ring.resize(trace_ring_cap);
            fprintf(stderr, "[TRACE] rolling window of %zu lines → '%s'\n",
                    trace_ring_cap, trace_path);
        } else {
            fprintf(stderr, "[TRACE] streaming to '%s'\n", trace_path);
        }
    }
    if (halt_pattern && *halt_pattern) {
        fprintf(stderr, "[HALT_ON] '%s'\n", halt_pattern);
    } else {
        halt_pattern = nullptr;
    }

    SdCardSim sd(sd_path);
    if (sd.is_present())
        fprintf(stderr, "[SD] card emulation active\n");

    signal(SIGINT, sigint_handler);
    raw_mode();

    fprintf(stderr, "── Interactive Monitor (Ctrl-C to exit) ──\n\n");
    reset(cpu);

    bool rx_pending = false;
    uint8_t rx_byte = 0;
    uint64_t cycles = 0;
    uint8_t sd_resp = 0xFF;
    bool sd_resp_valid = false;

    while (running) {
        // ── Drive UART RX signals ──────────────────────────────
        cpu->i_uart_rx_valid = rx_pending ? 1 : 0;
        cpu->i_uart_rx_data  = rx_byte;

        // ── Falling edge — combinational outputs are pre-posedge ─
        cpu->i_clk = 0;
        cpu->eval();
        bool rx_ack = cpu->o_uart_rx_ack;

        // ── Rising edge — commits state ─────────────────────────
        cpu->i_clk = 1;
        cpu->eval();
        cycles++;


        // ── Instruction trace → file or ring buffer ─────────────
        if (trace_fp && cpu->o_trace_valid) {
            uint32_t sr = cpu->o_trace_sr;
            char line[512];
            int off = snprintf(line, sizeof(line),
                    "PC=%08x SR=%08x [%c%c%c%c]",
                    cpu->o_pc, sr,
                    (sr & 0x80000000) ? 'N' : '-',
                    (sr & 0x40000000) ? 'Z' : '-',
                    (sr & 0x20000000) ? 'C' : '-',
                    (sr & 0x10000000) ? 'V' : '-');
            for (int r = 1; r <= 14; r++) {
                cpu->i_dbg_reg_addr = r;
                cpu->eval();
                off += snprintf(line + off, sizeof(line) - off,
                                " R%d=%08x", r, cpu->o_dbg_reg_data);
            }
            if (off < (int)sizeof(line) - 1) line[off++] = '\n';
            line[off] = '\0';

            if (trace_ring_cap > 0) {
                trace_ring[trace_ring_head].assign(line, off);
                trace_ring_head = (trace_ring_head + 1) % trace_ring_cap;
                if (trace_ring_count < trace_ring_cap) trace_ring_count++;
            } else {
                fputs(line, trace_fp);
            }
        }

        // ── UART TX → stdout (and halt-on-pattern matcher) ──────
        if (cpu->o_uart_tx_valid) {
            char c = (char)cpu->o_uart_tx_data;
            putchar(c);
            fflush(stdout);

            if (halt_pattern) {
                if (c == halt_pattern[halt_match_pos]) {
                    halt_match_pos++;
                    if (halt_pattern[halt_match_pos] == '\0') {
                        fprintf(stderr,
                                "\n[HALT_ON matched '%s' after %lu cycles]\n",
                                halt_pattern, (unsigned long)cycles);
                        running = 0;
                    }
                } else {
                    halt_match_pos = (c == halt_pattern[0]) ? 1 : 0;
                }
            }
        }

        // ── RX accepted? ────────────────────────────────────────
        if (rx_pending && rx_ack) {
            rx_pending = false;
        }

        // ── SD card emulation ──────────────────────────────────
        // CS0 is active-low: selected when bit is 0
        sd.select(!cpu->o_spi_cs0);
        if (cpu->o_spi_cmd_valid) {
            sd_resp = sd.exchange(cpu->o_spi_cmd_data);
            sd_resp_valid = true;
        }
        cpu->i_spi_resp_valid = sd_resp_valid ? 1 : 0;
        cpu->i_spi_resp_data  = sd_resp;

        // ── Poll stdin for new input (every 1024 cycles) ────────
        // Avoids a syscall per tick while keeping sub-character
        // latency (UART baud ≈ 2170 cycles/char).
        if (!rx_pending && (cycles & 0x3FF) == 0) {
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                char c;
                if (read(STDIN_FILENO, &c, 1) == 1) {
                    rx_byte = (uint8_t)c;
                    rx_pending = true;
                }
            }
        }

        // ── BREAK halts the simulation ──────────────────────────
        if (cpu->o_halted) {
            fprintf(stderr, "\n[BREAK after %lu cycles, PC=%x]\n", (unsigned long)cycles, cpu->o_pc);
            break;
        }
    }

    if (!cpu->o_halted) {
        fprintf(stderr, "\n[Interrupted after %lu cycles, PC=0x%08X]\n",
                (unsigned long)cycles, cpu->o_pc);
    }

    if (trace_fp) {
        if (trace_ring_cap > 0 && trace_ring_count > 0) {
            // Dump the ring buffer in chronological order. If we wrapped,
            // the oldest entry lives at trace_ring_head; otherwise we
            // never wrapped and entries 0..count-1 are in order.
            size_t start = (trace_ring_count == trace_ring_cap) ? trace_ring_head : 0;
            for (size_t i = 0; i < trace_ring_count; i++) {
                fputs(trace_ring[(start + i) % trace_ring_cap].c_str(), trace_fp);
            }
            fprintf(stderr, "[TRACE] dumped %zu lines from rolling window\n",
                    trace_ring_count);
        }
        fclose(trace_fp);
        fprintf(stderr, "[TRACE] done\n");
    }
    restore_term();
    delete cpu;
    return 0;
}
