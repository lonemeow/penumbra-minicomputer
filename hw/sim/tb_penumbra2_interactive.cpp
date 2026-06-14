// tb_penumbra2_interactive.cpp — Penumbra/2 shim for the shared sim console.
//
// Wraps Vmachine_penumbra2_sim behind the SimCore interface: a single CPU
// clock (no separate SDRAM domain) and BREAK observed via o_prog_end (gen2
// traps a retiring BREAK rather than driving an o_halted line). It carries no
// instruction-trace ports yet, so trace_supported() stays false (+trace= is
// ignored with a notice). All terminal / SD / stdin / exit logic lives in
// sim_console.cpp.
//
// Usage: make simulate-rtl CORE=penumbra2

#include "verilated.h"
#include "Vmachine_penumbra2_sim.h"
#include "sim_console.h"

struct Penumbra2Core : SimCore {
    Vmachine_penumbra2_sim* dut = new Vmachine_penumbra2_sim;
    bool rx_ack_latched = false;

    ~Penumbra2Core() override { delete dut; }

    void reset() override {
        dut->i_rst = 1;
        dut->i_irq = 0;
        dut->i_timer_irq = 0;
        dut->i_uart_rx_valid = 0;
        dut->i_uart_rx_data = 0;
        dut->i_spi_resp_valid = 0;
        dut->i_spi_resp_data = 0xFF;
        tick();   // two reset cycles
        tick();
        dut->i_rst = 0;
    }

    // One CPU cycle: a falling then a rising edge. o_uart_rx_ack is sampled at
    // the falling edge, where the sim UART's combinational ack is valid.
    void tick() override {
        dut->i_clk = 0;
        dut->eval();
        rx_ack_latched = dut->o_uart_rx_ack;
        dut->i_clk = 1;
        dut->eval();
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
    uint32_t pc()     const override { return 0; }   // no PC observability port on the gen2 wrapper
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);   // expose +rom_hex= etc. to the RTL
    Penumbra2Core core;
    return run_console(core, parse_console_opts(argc, argv));
}
