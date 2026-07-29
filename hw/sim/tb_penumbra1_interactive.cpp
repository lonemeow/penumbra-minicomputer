// tb_penumbra1_interactive.cpp — Penumbra/1 shim for the shared sim console.
//
// Wraps Vmachine_sim behind the SimCore interface: the 4:1 SDRAM clock step,
// the UART/SPI port accessors, and gen1's rich per-instruction trace (o_trace_*
// plus the o_dbg_reg_addr register-dump loop and the one-cycle trap-marker
// deferral). All terminal / SD / stdin / exit logic lives in sim_console.cpp.
//
// Usage: make simulate-rtl   (CORE=penumbra1, the default)

#include "verilated.h"
#include "Vmachine_sim.h"
#include "sim_console.h"

#include <cstdio>

// Vector number -> name, mirroring penumbra_pkg.sv (VEC_BUS_FAULT=0 .. EXT_IRQ=9).
static const char* const vec_names[16] = {
    "BUS_FAULT", "TIMER",   "TLB_MISS", "TLB_PROT",
    "PRIV",      "SYSCALL", "BREAK",    "ILLEGAL",
    "ALIGN",     "EXT_IRQ", "rsvd10",   "rsvd11",
    "rsvd12",    "rsvd13",  "rsvd14",   "rsvd15",
};

// Format a trap-entry ("ENTER") or ERET marker. vector is meaningful only for
// ENTER; pass -1 for ERET.
static std::string fmt_trap_marker(const char* kind, uint64_t cycle,
                                   uint32_t pc, uint32_t sr, int vector) {
    char m[256];
    const char* vec_name = (vector != -1) ? vec_names[vector] : "N/A";
    snprintf(m, sizeof(m), "[TRAP %s cyc=%lu pc=%08x vec=%s S=%d I=%d]\n",
             kind, (unsigned long)cycle, pc, vec_name,
             !!(sr & 0x80000000u), !!(sr & 0x40000000u));
    return std::string(m);
}

struct Penumbra1Core : SimCore {
    Vmachine_sim* cpu = new Vmachine_sim;
    bool rx_ack_latched = false;
    // Trap-marker deferral: o_trace_except_entry pulses one cycle before
    // o_trace_vector is valid, so emit the marker on the following cycle.
    bool         trap_pending = false;
    uint64_t     trap_cycle   = 0;
    uint32_t     trap_pc      = 0;

    ~Penumbra1Core() override { delete cpu; }

    void reset() override {
        cpu->i_rst = 1;
        cpu->i_sdram_clk = 0;
        cpu->i_irq = 0;
        cpu->i_uart_rx_valid = 0;
        cpu->i_uart_rx_data = 0;
        cpu->i_spi_resp_valid = 0;
        cpu->i_spi_resp_data = 0xFF;
        cpu->i_dbg_reg_addr = 0;
        // No USB device attached in the interactive console (its input
        // channel is a separate concern from the UART-owned stdin); the
        // domain clock still runs so the controller stays reachable.
        cpu->i_usb_clk = 0;
        cpu->i_usb_rx_valid = 0;
        cpu->i_usb_rx_data = 0;
        cpu->i_usb_rx_last = 0;
        cpu->i_usb_dev_connect = 0;
        cpu->i_usb_dev_speed = 1;
        tick();   // two reset cycles, both clocks running
        tick();
        cpu->i_rst = 0;
    }

    // One CPU cycle = 4 SDRAM cycles (8 sdram_clk toggles), each its own eval()
    // so the SDRAM domain advances independently. o_uart_rx_ack is sampled at
    // the falling edge, where it is valid.
    void tick() override {
        cpu->i_clk = 0;
        cpu->eval();
        rx_ack_latched = cpu->o_uart_rx_ack;
        for (int s = 0; s < 4; s++) {
            cpu->i_sdram_clk = !cpu->i_sdram_clk; cpu->eval();
            if (s & 1) { cpu->i_usb_clk = !cpu->i_usb_clk; cpu->eval(); }
        }
        cpu->i_clk = 1;
        cpu->eval();
        for (int s = 0; s < 4; s++) {
            cpu->i_sdram_clk = !cpu->i_sdram_clk; cpu->eval();
            if (s & 1) { cpu->i_usb_clk = !cpu->i_usb_clk; cpu->eval(); }
        }
    }

    void uart_rx(bool v, uint8_t d) override { cpu->i_uart_rx_valid = v; cpu->i_uart_rx_data = d; }
    bool uart_rx_ack() const override { return rx_ack_latched; }
    bool uart_tx(uint8_t& d) const override {
        if (cpu->o_uart_tx_valid) { d = cpu->o_uart_tx_data; return true; }
        return false;
    }
    bool spi_cs0() const override { return cpu->o_spi_cs0; }
    bool spi_cmd(uint8_t& d) const override {
        if (cpu->o_spi_cmd_valid) { d = cpu->o_spi_cmd_data; return true; }
        return false;
    }
    void spi_resp(bool v, uint8_t d) override { cpu->i_spi_resp_valid = v; cpu->i_spi_resp_data = d; }

    bool     halted() const override { return cpu->o_halted; }
    uint32_t pc()     const override { return cpu->o_pc; }

    bool trace_supported() const override { return true; }

    void trace(std::vector<std::string>& out, uint64_t cycle) override {
        // Deferred trap marker from the previous cycle (vector now valid).
        if (trap_pending) {
            out.push_back(fmt_trap_marker("ENTER", trap_cycle, trap_pc,
                                          cpu->o_trace_sr, (int)(cpu->o_trace_vector & 0xF)));
            trap_pending = false;
        }
        // Per-instruction line: PC, SR flags, and R1..R14 via the debug port.
        if (cpu->o_trace_valid) {
            uint32_t sr = cpu->o_trace_sr;
            char line[512];
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
                off += snprintf(line + off, sizeof(line) - off, " R%d=%08x", r, cpu->o_dbg_reg_data);
            }
            if (off < (int)sizeof(line) - 1) line[off++] = '\n';
            line[off] = '\0';
            out.push_back(std::string(line));
        }
        // Trap entry (defer) / ERET (immediate) detection for the trace stream.
        if (cpu->o_trace_except_entry) {
            trap_pending = true;
            trap_cycle   = cycle;
            trap_pc      = cpu->o_pc;
        }
        if (cpu->o_trace_eret)
            out.push_back(fmt_trap_marker("ERET", cycle, cpu->o_pc, cpu->o_trace_sr, -1));
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
    Penumbra1Core core;
    return run_console(core, parse_console_opts(argc, argv));
}
