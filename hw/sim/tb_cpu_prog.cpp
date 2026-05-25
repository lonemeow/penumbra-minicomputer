// Verilator testbench for Penumbra CPU — program runner
//
// Runs a program until BREAK fires (o_halted pulses), then checks
// the result register R1 for pass/fail.
//
// Convention:
//   - R1 = 1 means PASS, R1 = 0 means FAIL
//   - Program ends with BREAK instruction (triggers exception, pulses o_halted)
//   - Testbench detects the pulse immediately — no PC stability polling
//   - On failure: dumps all registers and PC
//
// UART support:
//   - TX bytes are printed to stdout as they are transmitted
//   - RX is not driven (i_uart_rx_valid = 0) — future: stdin or buffer
//
// Usage: make sim MOD=machine_sim TB=tb_cpu_prog PROG=test_fib

#include <cstdio>
#include <cstdint>
#include "Vmachine_sim.h"
#if VM_TRACE
#include "verilated_vcd_c.h"
static VerilatedVcdC* tfp = nullptr;
static uint64_t sim_time = 0;
#endif

static void tick(Vmachine_sim* d) {
    // 4 SDRAM half-cycles per CPU half-cycle matches hardware's
    // 25 MHz CPU / 100 MHz SDRAM ratio.  Each SDRAM toggle gets
    // its own eval() so the SDRAM-domain RTL (controller, CDC's
    // sd side, model) advances on its own clock.
    d->i_clk = 0; d->eval();
#if VM_TRACE
    if (tfp) { tfp->dump(sim_time); sim_time++; }
#endif
    for (int s = 0; s < 4; s++) {
        d->i_sdram_clk = !d->i_sdram_clk;
        d->eval();
#if VM_TRACE
        if (tfp) { tfp->dump(sim_time); sim_time++; }
#endif
    }
    d->i_clk = 1; d->eval();
#if VM_TRACE
    if (tfp) { tfp->dump(sim_time); sim_time++; }
#endif
    for (int s = 0; s < 4; s++) {
        d->i_sdram_clk = !d->i_sdram_clk;
        d->eval();
#if VM_TRACE
        if (tfp) { tfp->dump(sim_time); sim_time++; }
#endif
    }

    // Check for UART TX output on rising edge
    if (d->o_uart_tx_valid) {
        putchar(d->o_uart_tx_data);
        fflush(stdout);
    }
}

static uint32_t read_reg(Vmachine_sim* cpu, int reg) {
    cpu->i_dbg_reg_addr = reg;
    cpu->eval();
    return cpu->o_dbg_reg_data;
}

// Run until o_halted goes high, or until cycle limit.
// Returns total cycles, or -1 if limit exceeded.
static int run_until_halt(Vmachine_sim* cpu, int limit) {
    int cycles = 0;

    while (cycles < limit) {
        tick(cpu);
        cycles++;
        if (cpu->o_halted)
            return cycles;
    }
    return -1;  // Did not halt
}

static void reset(Vmachine_sim* cpu) {
    cpu->i_rst = 1;
    cpu->i_irq = 0;
    cpu->i_uart_rx_valid = 0;
    cpu->i_uart_rx_data = 0;
    cpu->i_dbg_reg_addr = 0;
    cpu->i_sdram_clk = 0;
    tick(cpu);
    tick(cpu);
    cpu->i_rst = 0;
}

int main() {
    Vmachine_sim* cpu = new Vmachine_sim;
#if VM_TRACE
    Verilated::traceEverOn(true);
    tfp = new VerilatedVcdC;
    cpu->trace(tfp, 99);
    tfp->open("waves/machine_sim.vcd");
#endif

    printf("── Program Runner ──\n\n");
    reset(cpu);

    int cycles = run_until_halt(cpu, 2000000);
#if VM_TRACE
    if (tfp) { tfp->close(); delete tfp; tfp = nullptr; }
#endif

    if (cycles < 0) {
        printf("  FAIL: no BREAK within cycle limit\n");
        printf("  (missing BREAK instruction? infinite loop?)\n");
        printf("  Final state — PC = 0x%08X\n", cpu->o_pc);
        printf("  Register dump:\n");
        for (int i = 0; i < 16; i++) {
            printf("    R%-2d = 0x%08X\n", i, read_reg(cpu, i));
        }
        // Sample PC every ~1k cycles for 20k more cycles so the operator
        // can tell at a glance whether we're stuck in a tight loop
        // (PC bouncing in a small range) vs wandering (PC drifting,
        // e.g. an unhandled trap chain into vector-page data).
        printf("  PC samples, ~1k cycles apart:\n");
        for (int s = 0; s < 20; s++) {
            for (int k = 0; k < 1000; k++) tick(cpu);
            printf("    cyc+%5d  PC=0x%08X\n", s * 1000, cpu->o_pc);
        }
        printf("\n");
        printf("prog: 0/1 tests passed\n");
        printf("  *** 1 FAILED ***\n");
        delete cpu;
        return 1;
    }

    printf("  Halted after %d cycles (PC = 0x%08X)\n",
           cycles, cpu->o_pc);

    // Convention: R1 = 1 means pass, R1 = 0 means fail
    uint32_t r1 = read_reg(cpu, 1);
    if (r1 == 1) {
        printf("  PASS (R1 = 1)\n\n");
        printf("prog: 1/1 tests passed\n");
    } else {
        printf("  FAIL (R1 = %u / 0x%08X, expected 1)\n", r1, r1);
        printf("  Register dump:\n");
        for (int i = 0; i < 16; i++) {
            printf("    R%-2d = 0x%08X\n", i, read_reg(cpu, i));
        }
        printf("\n");
        printf("prog: 0/1 tests passed\n");
        printf("  *** 1 FAILED ***\n");
        delete cpu;
        return 1;
    }

    delete cpu;
    return 0;
}
