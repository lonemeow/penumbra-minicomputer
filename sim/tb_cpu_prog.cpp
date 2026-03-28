// Verilator testbench for Penumbra CPU — program runner
//
// Runs a program to completion (halt loop detected), then checks
// the result register R1 for pass/fail.
//
// Convention:
//   - R1 = 1 means PASS, R1 = 0 means FAIL
//   - Program ends with a halt loop (B .)
//   - Testbench detects halt when PC is stable for several cycles
//
// Future: replace halt-loop detection with SYSCALL trap once
// the instruction is implemented. The current approach wastes
// cycles spinning on PC stability detection.
//
// Usage: make sim MOD=cpu_top TB=tb_cpu_prog PROG=test_fib

#include <cstdio>
#include <cstdint>
#include "Vcpu_top.h"

static void tick(Vcpu_top* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static uint32_t read_reg(Vcpu_top* cpu, int reg) {
    cpu->i_dbg_reg_addr = reg;
    cpu->eval();
    return cpu->o_dbg_reg_data;
}

// Run until PC is stable for `stable_needed` cycles, or until limit.
// Returns total cycles, or -1 if limit exceeded.
static int run_until_halt(Vcpu_top* cpu, int limit, int stable_needed = 10) {
    int cycles = 0;
    int stable = 0;
    uint32_t prev_pc = 0xFFFFFFFF;

    while (cycles < limit) {
        tick(cpu);
        cycles++;
        if (cpu->o_pc == prev_pc) {
            stable++;
            if (stable >= stable_needed)
                return cycles;
        } else {
            stable = 0;
            prev_pc = cpu->o_pc;
        }
    }
    return -1;  // Did not halt
}

static void reset(Vcpu_top* cpu) {
    cpu->i_rst = 1;
    cpu->i_irq = 0;
    cpu->i_dbg_reg_addr = 0;
    tick(cpu);
    tick(cpu);
    cpu->i_rst = 0;
}

int main() {
    Vcpu_top* cpu = new Vcpu_top;

    printf("── Program Runner ──\n\n");
    reset(cpu);

    int cycles = run_until_halt(cpu, 5000);
    if (cycles < 0) {
        printf("  FAIL: program did not halt within cycle limit\n\n");
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
        printf("  FAIL (R1 = %u / 0x%08X, expected 1)\n\n", r1, r1);
        printf("prog: 0/1 tests passed\n");
        printf("  *** 1 FAILED ***\n");
        delete cpu;
        return 1;
    }

    delete cpu;
    return 0;
}
