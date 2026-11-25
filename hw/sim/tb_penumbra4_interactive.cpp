// tb_penumbra4_interactive.cpp — Penumbra/4 shim for the shared sim console.
//
// A fork of tb_penumbra3_interactive that differs in the machine: the gen4
// machine keeps the gen3 one's bus and device ports, so it wraps
// Vmachine_penumbra4_sim behind the SimCore interface the same way: a CPU clock plus a faster
// i_sdram_clk for the full SDRAM model, and BREAK observed via o_prog_end,
// which pulses as a BREAK's trap commits. The trace is a line for each op
// the machine lands (penumbra4_landing.h), with the SR it landed on and the
// register writes of its cycle; the halt PC is the last op landed. All
// terminal / SD / stdin / exit logic lives in sim_console.cpp.
//
// Usage: make simulate-rtl CORE=penumbra4   (TRACE=trace.log for the trace)

#include "verilated.h"
#include "Vmachine_penumbra4_sim.h"
#include "sim_console.h"
#include "penumbra4_landing.h"

struct Penumbra4Core : SimCore {
    Vmachine_penumbra4_sim* dut = new Vmachine_penumbra4_sim;
    bool rx_ack_latched = false;
    uint32_t last_landed_pc = 0;

    ~Penumbra4Core() override { delete dut; }

    void reset() override {
        dut->i_rst = 1;
        dut->i_irq = 0;
        dut->i_timer_irq = 0;
        dut->i_uart_rx_valid = 0;
        dut->i_uart_rx_data = 0;
        dut->i_spi_resp_valid = 0;
        dut->i_spi_resp_data = 0xFF;
        dut->i_sdram_clk = 0;
        tick();   // two reset cycles
        tick();
        dut->i_rst = 0;
    }

    // One CPU cycle with the dual SDRAM clock: 4 SDRAM half-cycles per CPU
    // half-cycle (hardware's 25 MHz CPU / 100 MHz SDRAM ratio). o_uart_rx_ack is
    // sampled at the falling edge, where the sim UART's combinational ack is
    // valid, before the SDRAM toggles.
    void tick() override {
        dut->i_clk = 0;
        dut->eval();
        rx_ack_latched = dut->o_uart_rx_ack;
        for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
        dut->i_clk = 1;
        dut->eval();
        for (int s = 0; s < 4; s++) { dut->i_sdram_clk = !dut->i_sdram_clk; dut->eval(); }
        Landing now[2];
        int n = read_landings(dut, 0, now);
        if (n) last_landed_pc = now[n - 1].pc;
    }

    void uart_rx(bool v, uint8_t d) override { dut->i_uart_rx_valid = v; dut->i_uart_rx_data = d; }
    bool uart_rx_ack() const override { return rx_ack_latched; }
    bool uart_tx(uint8_t& d) const override {
        if (dut->o_uart_tx_valid) { d = dut->o_uart_tx_data; return true; }
        return false;
    }
    bool spi_cs0() const override { return dut->o_spi_cs0; }
    bool spi_cmd(uint8_t& d) const override {
        if (dut->o_spi_cmd_valid) { d = dut->o_spi_cmd_data; return true; }
        return false;
    }
    void spi_resp(bool v, uint8_t d) override { dut->i_spi_resp_valid = v; dut->i_spi_resp_data = d; }

    bool     halted() const override { return dut->o_prog_end; }
    uint32_t pc()     const override { return last_landed_pc; }

    bool trace_supported() const override { return true; }

    // The SR is the one the ops landed on, before their own writes to it.
    // A cycle's register writes go on its last line: with one slot issuing,
    // they are that op's.
    void trace(std::vector<std::string>& out, uint64_t cycle) override {
        Landing now[2];
        int n = read_landings(dut, cycle, now);
        const uint32_t sr = dut->o_sr;
        for (int i = 0; i < n; i++) {
            char line[256];
            int off = snprintf(line, sizeof(line), "%s SR=%08x [%c%c%c%c %c%c]",
                    format_landing(now[i]).c_str(), sr,
                    (sr & (1u <<  0)) ? 'N' : '-',
                    (sr & (1u <<  1)) ? 'Z' : '-',
                    (sr & (1u <<  2)) ? 'C' : '-',
                    (sr & (1u <<  3)) ? 'V' : '-',
                    (sr & (1u << 31)) ? 'S' : 'u',
                    (sr & (1u << 30)) ? 'I' : '-');
            for (int p = 0; i == n - 1 && p < 2; p++) {
                if (!(dut->o_commit_we & (1u << p))) continue;
                off += snprintf(line + off, sizeof(line) - off, " %s=%08x",
                        landing_phys_reg_name(dut->o_commit_phys >> (4 * p)),
                        uint32_t(dut->o_commit_data >> (32 * p)));
            }
            out.push_back(std::string(line) + "\n");
        }
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
    Penumbra4Core core;
    return run_console(core, parse_console_opts(argc, argv));
}
