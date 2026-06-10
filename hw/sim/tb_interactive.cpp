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

// Deterministic stdin replay. When +stdin_file=<path> is given, RX bytes
// are sourced from the file at a fixed cycle cadence (STDIN_FILE_PERIOD)
// instead of live host stdin. Once the file is exhausted, we fall back
// to live stdin so the operator can explore after a scripted boot.
// See discussion: live poll(STDIN_FILENO) timing is the dominant
// nondeterminism source between RTL boot runs.
static FILE*   stdin_file_fp     = nullptr;
static uint64_t stdin_file_next   = 0;       // cycle at which to deliver next byte
static const uint64_t STDIN_FILE_PERIOD = 100000;  // ~46 char-times at 2170 cyc/char

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

// Trap-marker detection: o_trace_except_entry pulses one cycle BEFORE
// the *_pending flops latch, so the vector number on o_trace_vector is
// only valid the cycle after the pulse.  Defer emit by one cycle.
static bool        trap_emit_pending      = false;
static unsigned long trap_emit_cycle      = 0;
static uint32_t    trap_emit_pc           = 0;

// Vector number → human-readable name.  Indexed by 4-bit vector_num.
// Order mirrors penumbra_pkg.sv (VEC_BUS_FAULT=0 .. VEC_EXT_IRQ=9).
static const char* const vec_names[16] = {
    "BUS_FAULT", "TIMER",   "TLB_MISS", "TLB_PROT",
    "PRIV",      "SYSCALL", "BREAK",    "ILLEGAL",
    "ALIGN",     "EXT_IRQ", "rsvd10",   "rsvd11",
    "rsvd12",    "rsvd13",  "rsvd14",   "rsvd15",
};

static void sigint_handler(int) { running = 0; }

// Append a single line (already \n-terminated) to either the rolling
// ring buffer or directly to the trace file, depending on configuration.
// Used by both the per-instruction trace and the trap markers, so they
// land in the same chronological stream.
static void trace_write(const char* line, size_t len) {
    if (!trace_fp) return;
    if (trace_ring_cap > 0) {
        trace_ring[trace_ring_head].assign(line, len);
        trace_ring_head = (trace_ring_head + 1) % trace_ring_cap;
        if (trace_ring_count < trace_ring_cap) trace_ring_count++;
    } else {
        fputs(line, trace_fp);
    }
}

// Format a trap-entry or ERET marker line.
//
// kind:    "ENTER" (exception/IRQ entry) or "ERET" (bulk SR restore).
// vector:  meaningful only when kind == "ENTER"; pass -1 for ERET.
static void emit_trap_marker(const char* kind, unsigned long cycle,
                             uint32_t pc, uint32_t sr, int vector) {
    char marker[256];
    int  off = 0;
    const char *vec_name = vector != -1 ? vec_names[vector] : "N/A";
    off = snprintf(marker, sizeof(marker),
                   "[TRAP %s cyc=%lu pc=%08x vec=%s S=%d I=%d]\n",
                   kind, cycle, pc, vec_name,
                   !!(sr & 0x80000000u), !!(sr & 0x40000000u));
    if (off > 0 && off < (int)sizeof(marker))
        trace_write(marker, (size_t)off);
}

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

// Advance simulation by one CPU clock cycle.  Within that CPU cycle
// the SDRAM clock toggles 8 times (4 full SDRAM cycles), matching
// the 25 MHz CPU / 100 MHz SDRAM hardware ratio.  Each SDRAM toggle
// gets its own eval() so the SDRAM-domain RTL (sdram_ctrl, the CDC's
// SDRAM side, sdram_model) advances independently of the CPU clock.
static void tick_one_cpu_cycle(Vmachine_sim* cpu) {
    // 4 SDRAM half-cycles per CPU half-cycle.
    cpu->i_clk = 0;
    cpu->eval();
    for (int s = 0; s < 4; s++) {
        cpu->i_sdram_clk = !cpu->i_sdram_clk;
        cpu->eval();
    }
    cpu->i_clk = 1;
    cpu->eval();
    for (int s = 0; s < 4; s++) {
        cpu->i_sdram_clk = !cpu->i_sdram_clk;
        cpu->eval();
    }
}

static void reset(Vmachine_sim* cpu) {
    cpu->i_rst = 1;
    cpu->i_sdram_clk = 0;
    cpu->i_irq = 0;
    cpu->i_uart_rx_valid = 0;
    cpu->i_uart_rx_data = 0;
    cpu->i_spi_resp_valid = 0;
    cpu->i_spi_resp_data = 0xFF;
    cpu->i_dbg_reg_addr = 0;
    // Two reset cycles (both clocks running during reset)
    tick_one_cpu_cycle(cpu);
    tick_one_cpu_cycle(cpu);
    cpu->i_rst = 0;
}

// Extract +sdcard=<path> plusarg from command line (via Verilator)
static const char* get_sdcard_path() {
    // Verilator stores plusargs; use environment as fallback
    const char* p = getenv("SDCARD_IMG");
    return p;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
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
        else if (strncmp(argv[i], "+stdin_file=", 12) == 0) {
            const char* path = argv[i] + 12;
            stdin_file_fp = fopen(path, "rb");
            if (!stdin_file_fp)
                fprintf(stderr, "[STDIN_FILE] cannot open '%s'\n", path);
            else
                fprintf(stderr, "[STDIN_FILE] replaying '%s' (1 byte every %lu cycles, then live stdin)\n",
                        path, (unsigned long)STDIN_FILE_PERIOD);
        }
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
        // 4 SDRAM half-cycles per CPU half-cycle (matches the
        // 25 MHz CPU / 100 MHz SDRAM hardware ratio).
        for (int s = 0; s < 4; s++) {
            cpu->i_sdram_clk = !cpu->i_sdram_clk;
            cpu->eval();
        }

        // ── Rising edge — commits state ─────────────────────────
        cpu->i_clk = 1;
        cpu->eval();
        for (int s = 0; s < 4; s++) {
            cpu->i_sdram_clk = !cpu->i_sdram_clk;
            cpu->eval();
        }
        cycles++;


        // ── Trap-marker emit (deferred from previous cycle) ─────
        // o_trace_except_entry pulses one cycle before vector_num is
        // valid (the *_pending flops latch on its rising edge).  We
        // saw the pulse last cycle; emit the marker now using the
        // newly-valid o_trace_vector.
        if (trace_fp && trap_emit_pending) {
            uint32_t sr = cpu->o_trace_sr;
            int vec = (int)cpu->o_trace_vector & 0xF;
            emit_trap_marker("ENTER", trap_emit_cycle, trap_emit_pc, sr, vec);
            trap_emit_pending = false;
        }

        // ── Instruction trace → file or ring buffer ─────────────
        if (trace_fp && cpu->o_trace_valid) {
            uint32_t sr = cpu->o_trace_sr;
            char line[512];
            // SR layout (penumbra_pkg.sv): N=bit0, Z=bit1, C=bit2, V=bit3,
            // I=bit30, S=bit31. Earlier versions of this trace pulled the
            // condition flags from bits 31:28, which actually decoded
            // [S, I, x, x] under NZCV labels — fixed.
            int off = snprintf(line, sizeof(line),
                    "PC=%08x SR=%08x [%c%c%c%c %c%c]",
                    cpu->o_pc, sr,
                    (sr & (1u <<  0)) ? 'N' : '-',
                    (sr & (1u <<  1)) ? 'Z' : '-',
                    (sr & (1u <<  2)) ? 'C' : '-',
                    (sr & (1u <<  3)) ? 'V' : '-',
                    (sr & (1u << 31)) ? 'S' : 'u',
                    (sr & (1u << 30)) ? 'I' : '-');
            for (int r = 1; r <= 14; r++) {
                cpu->i_dbg_reg_addr = r;
                cpu->eval();
                off += snprintf(line + off, sizeof(line) - off,
                                " R%d=%08x", r, cpu->o_dbg_reg_data);
            }
            if (off < (int)sizeof(line) - 1) line[off++] = '\n';
            line[off] = '\0';
            trace_write(line, (size_t)off);
        }

        // ── Trap entry / ERET detection ─────────────────────────
        // except_entry is a 1-cycle pulse on any of the eight exception
        // sources (incl. IRQ).  Stash PC/cycle and emit the marker on
        // the FOLLOWING cycle, when o_trace_vector is valid.
        if (trace_fp && cpu->o_trace_except_entry) {
            trap_emit_pending = true;
            trap_emit_cycle   = (unsigned long)cycles;
            trap_emit_pc      = cpu->o_pc;
        }
        // ctl_sr_load fires on ERET (and rare WRSPR SR).  No deferral
        // needed — the SR being restored is whatever the µ-op presents
        // this cycle.
        if (trace_fp && cpu->o_trace_eret) {
            emit_trap_marker("ERET", (unsigned long)cycles, cpu->o_pc,
                             cpu->o_trace_sr, -1);
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

        // ── Deliver next byte from +stdin_file= replay ──────────
        // Fixed cadence so the kernel sees the same byte at the
        // same sim cycle every run — eliminates host-scheduler
        // entropy from the live stdin poll path.
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

        // ── Poll live stdin for new input (every 8192 cycles) ───
        // Avoids a poll() per tick.  UART baud is ≈ 2170 cycles/char
        // at 115200 / 25 MHz, so 8192 still gives ~3.8 polls per
        // char-time — well below the rate at which the kernel can
        // drain bytes from RBR.  At 1024 cycles this was issuing
        // ~24 k poll() syscalls per simulated second, an order of
        // magnitude more than necessary.  Skipped while +stdin_file=
        // replay is active — see comment above for determinism
        // rationale.
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
