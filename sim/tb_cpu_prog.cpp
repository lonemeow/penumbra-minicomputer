// Verilator testbench for Penumbra CPU — program runner
//
// Runs a program to completion (halt loop detected), then checks
// the result register. No cycle-by-cycle internal state inspection.
//
// Convention:
//   - Program result in R1
//   - Program ends with a halt loop (B .)
//   - Testbench detects halt when PC is stable for several cycles
//
// Usage: make sim MOD=cpu_top TB=tb_cpu_prog PROG=test_fib

#include <cstdio>
#include <cstdint>
#include "Vcpu_top.h"

static int errors = 0, tests = 0;

static void tick(Vcpu_top* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) {
        printf("  FAIL [%s]: got %u (0x%08X), expected %u (0x%08X)\n",
               n, got, got, exp, exp);
        errors++;
    } else {
        printf("  OK   [%s]: %u\n", n, got);
    }
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
        printf("  FAIL: program did not halt within cycle limit\n");
        errors++;
    } else {
        printf("  Program halted after %d cycles (PC = 0x%08X)\n\n",
               cycles, cpu->o_pc);

        // Check result register (R1)
        check("fib(10)", read_reg(cpu, 1), 55);
    }

    printf("\nprog: %d/%d tests passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);

    delete cpu;
    return errors ? 1 : 0;
}
