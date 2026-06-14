// tb_penumbra2_interactive.cpp — interactive terminal for the Penumbra/2 machine
//
// Bridges host stdin/stdout to the gen2 machine's sim UART so the boot ROM
// monitor is usable from a real terminal. Single-clock: machine_penumbra2_sim
// has no separate SDRAM clock domain, so each iteration is one CPU cycle (a
// falling then a rising edge), unlike the gen1 tb_interactive's 4:1 SDRAM step.
//
// Runs until the ROM retires a BREAK (o_prog_end — the monitor's `break`
// command) or the operator hits Ctrl-C. The ROM image loads from program.hex
// (machine_penumbra2_sim's INIT_FILE default), which `make -C hw/rom` builds.
//
// Usage: make simulate-rtl CORE=penumbra2

#include <cstdio>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include "verilated.h"
#include "Vmachine_penumbra2_sim.h"
#include "sd_card_sim.h"

static volatile sig_atomic_t running = 1;
static struct termios orig_termios;
static bool term_raw = false;

static void sigint_handler(int) { running = 0; }

static void restore_terminal() {
    if (term_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        term_raw = false;
    }
}

// Raw mode so the boot ROM owns all character processing (no host echo, no
// line buffering, no signal/flow-control interception). Skipped for piped
// input so non-interactive runs behave normally.
static void raw_mode() {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    term_raw = true;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
    Vmachine_penumbra2_sim* dut = new Vmachine_penumbra2_sim;

    // SD card: +sdcard=<image> attaches a card behind sim_spi; absent → MISO
    // high (the ROM probes and finds no card).
    const char* sd_path = nullptr;
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "+sdcard=", 8) == 0) sd_path = argv[i] + 8;
    SdCardSim sd(sd_path);
    if (sd.is_present())
        fprintf(stderr, "[SD] card image attached\r\n");

    signal(SIGINT, sigint_handler);
    raw_mode();
    fprintf(stderr, "-- Penumbra/2 Interactive Monitor (Ctrl-C to exit) --\r\n\r\n");

    // ── Reset: hold for two cycles ──────────────────────────────
    dut->i_irq = 0;
    dut->i_timer_irq = 0;
    dut->i_uart_rx_valid = 0;
    dut->i_uart_rx_data = 0;
    dut->i_spi_resp_valid = 0;
    dut->i_spi_resp_data = 0xFF;
    dut->i_rst = 1;
    for (int i = 0; i < 2; i++) { dut->i_clk = 0; dut->eval(); dut->i_clk = 1; dut->eval(); }
    dut->i_rst = 0;

    bool    rx_pending = false;
    uint8_t rx_byte    = 0;
    uint8_t sd_resp       = 0xFF;
    bool    sd_resp_valid = false;

    while (running) {
        // ── Fetch one host byte if we are not already holding one ──
        if (!rx_pending) {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                uint8_t c;
                if (read(STDIN_FILENO, &c, 1) == 1) { rx_byte = c; rx_pending = true; }
            }
        }

        // ── Present the pending RX byte (if any) to the UART ───────
        dut->i_uart_rx_valid = rx_pending ? 1 : 0;
        dut->i_uart_rx_data  = rx_byte;

        // ── Falling edge: combinational outputs settle ─────────────
        dut->i_clk = 0;
        dut->eval();

        // TODO(human): honor the sim_uart RX contract — "hold i_rx_valid +
        // i_rx_data until o_rx_ack" (sim_uart.sv). o_uart_rx_ack is valid now,
        // post-falling-edge (it is combinational: o_rx_ack = i_rx_valid &&
        // !rx_ready, and the UART latches the byte on the coming rising edge).
        // Sample o_uart_rx_ack and, when it pulses, clear rx_pending so the
        // next host byte is fetched on the following iteration. Clearing it
        // unconditionally would drop characters when the operator types faster
        // than the ROM drains the holding register.
        if (dut->o_uart_rx_ack && rx_pending) {
            rx_pending = false;
        }

        // ── Rising edge: state commits ─────────────────────────────
        dut->i_clk = 1;
        dut->eval();

        // ── Drain one TX byte to the host (o_tx_valid is a 1-cycle pulse) ──
        if (dut->o_uart_tx_valid) {
            uint8_t c = dut->o_uart_tx_data;
            ssize_t w = write(STDOUT_FILENO, &c, 1);
            (void)w;
        }

        // ── SD card: bridge sim_spi's MOSI/MISO bytes to the card model ──
        // CS0 is active-low. Each o_cmd_valid pulse is one full-duplex byte;
        // the response is held on i_spi_resp_* until the next exchange.
        sd.select(!dut->o_spi_cs0);
        if (dut->o_spi_cmd_valid) {
            sd_resp = sd.exchange(dut->o_spi_cmd_data);
            sd_resp_valid = true;
        }
        dut->i_spi_resp_valid = sd_resp_valid ? 1 : 0;
        dut->i_spi_resp_data  = sd_resp;

        // ── The monitor's `break` command retires a BREAK ──────────
        if (dut->o_prog_end) running = 0;
    }

    restore_terminal();
    fprintf(stderr, "\r\n-- Halted --\r\n");
    delete dut;
    return 0;
}
