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
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include "Vmachine_sim.h"

static volatile sig_atomic_t running = 1;
static struct termios orig_termios;
static bool term_raw = false;

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
    cpu->i_dbg_reg_addr = 0;
    // Two reset cycles
    cpu->i_clk = 0; cpu->eval();
    cpu->i_clk = 1; cpu->eval();
    cpu->i_clk = 0; cpu->eval();
    cpu->i_clk = 1; cpu->eval();
    cpu->i_rst = 0;
}

int main() {
    Vmachine_sim* cpu = new Vmachine_sim;

    signal(SIGINT, sigint_handler);
    raw_mode();

    fprintf(stderr, "── Interactive Monitor (Ctrl-C to exit) ──\n\n");
    reset(cpu);

    bool rx_pending = false;
    uint8_t rx_byte = 0;
    uint64_t cycles = 0;

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

        // ── UART TX → stdout ────────────────────────────────────
        if (cpu->o_uart_tx_valid) {
            putchar(cpu->o_uart_tx_data);
            fflush(stdout);
        }

        // ── RX accepted? ────────────────────────────────────────
        if (rx_pending && rx_ack) {
            rx_pending = false;
        }

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
            fprintf(stderr, "\n[BREAK after %lu cycles]\n", (unsigned long)cycles);
            break;
        }
    }

    if (!cpu->o_halted) {
        fprintf(stderr, "\n[Interrupted after %lu cycles]\n", (unsigned long)cycles);
    }

    restore_term();
    delete cpu;
    return 0;
}
