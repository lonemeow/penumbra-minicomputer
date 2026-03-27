// Verilator testbench for the Penumbra CPU Top
//
// Loads a test program and verifies register values via the debug port.
// Test program: LLI R1,#5 → LLI R2,#3 → ADD R1,R2 → LLI R3,#0
// Expected: R1=8, R2=3, R3=0

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
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp);
        errors++;
    }
}

static uint32_t read_reg(Vcpu_top* cpu, int reg) {
    cpu->i_dbg_reg_addr = reg;
    cpu->eval();
    return cpu->o_dbg_reg_data;
}

int main() {
    Vcpu_top* cpu = new Vcpu_top;

    // Reset
    cpu->i_rst = 1;
    cpu->i_dbg_reg_addr = 0;
    tick(cpu);
    tick(cpu);
    cpu->i_rst = 0;

    check("reset_pc", cpu->o_pc, 0x00000000);

    // Run until PC passes 0x0C (3 instructions + start of 4th)
    int cycles = 0;
    while (cpu->o_pc <= 0x0C && cycles < 50) {
        tick(cpu);
        cycles++;
    }

    printf("  3 instructions executed in %d cycles (%.1f CPI)\n",
           cycles, cycles / 3.0);

    // Verify register values after: LLI R1,#5 → LLI R2,#3 → ADD R1,R2
    check("r0_zero",   read_reg(cpu, 0),  0);
    check("r1_result", read_reg(cpu, 1),  8);   // 5 + 3
    check("r2_value",  read_reg(cpu, 2),  3);
    check("r3_clear",  read_reg(cpu, 3),  0);
    check("r4_zero",   read_reg(cpu, 4),  0);   // Never written

    // Run a few more to complete 4th instruction
    for (int i = 0; i < 8; i++) tick(cpu);
    check("r3_after_lli", read_reg(cpu, 3), 0); // LLI R3, #0

    printf("\ncpu_top: %d/%d tests passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);

    delete cpu;
    return errors ? 1 : 0;
}
