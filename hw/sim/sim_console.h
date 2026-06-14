// sim_console.h — shared interactive console for the Penumbra RTL sims.
//
// Bridges host stdin/stdout to the boot ROM's UART and an emulated SD card to
// its SPI, independent of CPU generation. A per-core SimCore shim owns the
// Verilated DUT and hides clocking / port differences; run_console() drives
// the terminal, SD, trace, and exit loop against that interface. The shared
// frontend deliberately includes no Verilated header — it speaks only SimCore
// — so one compiled object links into every generation's sim.

#ifndef SIM_CONSOLE_H
#define SIM_CONSOLE_H

#include <cstdint>
#include <string>
#include <vector>

// Per-core shim contract. Usage per cycle: present inputs (uart_rx, the SD
// response), tick() one CPU cycle, then read outputs. tick() must sample
// uart_rx_ack at the point its value is valid (mid-cycle for the sim UART) and
// hold it for uart_rx_ack() afterwards.
struct SimCore {
    virtual ~SimCore() {}

    virtual void reset() = 0;
    virtual void tick()  = 0;            // advance one CPU cycle

    // UART byte stream
    virtual void uart_rx(bool valid, uint8_t data) = 0;  // present before tick()
    virtual bool uart_rx_ack() const = 0;                // valid after tick()
    virtual bool uart_tx(uint8_t& data) const = 0;       // true + data if a TX byte fired

    // SPI / SD byte stream
    virtual bool spi_cs0() const = 0;                    // raw CS0 (active-low)
    virtual bool spi_cmd(uint8_t& data) const = 0;       // true + MOSI if a byte shifted
    virtual void spi_resp(bool valid, uint8_t data) = 0; // present MISO for the next exchange

    // Status
    virtual bool     halted() const = 0;                 // ROM executed BREAK
    virtual uint32_t pc()     const = 0;                 // PC at halt (0 if unobservable)

    // Per-cycle trace lines (\n-terminated), appended to `out`. Core-specific
    // observability; non-const because a core may poke debug ports to build a
    // line. Default: no trace. trace_supported() gates the +trace= plumbing.
    virtual void trace(std::vector<std::string>& out, uint64_t cycle) { (void)out; (void)cycle; }
    virtual bool trace_supported() const { return false; }
};

struct ConsoleOpts {
    const char* sd_path      = nullptr;  // +sdcard=<image>
    const char* trace_path   = nullptr;  // +trace=<file>
    size_t      trace_ring   = 0;        // +trace_window=<N> (0 = stream)
    const char* halt_pattern = nullptr;  // +halt_on=<string>
    const char* stdin_file   = nullptr;  // +stdin_file=<file>
};

// Parse the common +plusargs (call after Verilated::commandArgs).
ConsoleOpts parse_console_opts(int argc, char** argv);

// Run the interactive console against `core`. Returns 0.
int run_console(SimCore& core, const ConsoleOpts& opts);

#endif  // SIM_CONSOLE_H
