// Verilator testbench for Penumbra CPU — Load/Store integration
//
// Tests LDW and STW instructions using test_mem.s:
//   Phase 1: Basic store + load (STW then LDW at same address)
//   Phase 2: Offset addressing ([Rb + #4])
//   Phase 3: Overwrite (STW overwrites previous value)

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

static int run_until_pc(Vcpu_top* cpu, uint32_t target, int limit) {
    int cycles = 0;
    while (cpu->o_pc < target && cycles < limit) {
        tick(cpu);
        cycles++;
    }
    return cycles;
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

    printf("── Load/Store Integration Test ──\n");
    reset(cpu);
    check("reset_pc", cpu->o_pc, 0x00000000);

    // test_mem.s layout (12 instructions, 0x00–0x2C):
    //   0x00: LLI R1, #42
    //   0x04: LLI R2, #0x100
    //   0x08: STW R1, [R2]          ; mem[0x100] = 42
    //   0x0C: LLI R1, #0
    //   0x10: LDW R3, [R2]          ; R3 = 42
    //   0x14: LLI R4, #99
    //   0x18: STW R4, [R2 + #4]     ; mem[0x104] = 99
    //   0x1C: LDW R5, [R2 + #4]     ; R5 = 99
    //   0x20: LDW R6, [R2]          ; R6 = 42
    //   0x24: LLI R7, #0xFF
    //   0x28: STW R7, [R2]          ; mem[0x100] = 255
    //   0x2C: LDW R8, [R2]          ; R8 = 255
    // End at PC = 0x30

    // ── Phase 1: Basic store + load ────────────────────────
    printf("\n── Phase 1: Basic STW + LDW ──\n");

    // Run through STW R1,[R2] + LLI R1,0 + LDW R3,[R2] → PC=0x14
    int cycles = run_until_pc(cpu, 0x14, 200);
    printf("  Phase 1 in %d cycles\n", cycles);

    check("r1_cleared",  read_reg(cpu, 1),  0);      // LLI R1, #0
    check("r2_base",     read_reg(cpu, 2),  0x100);   // base address
    check("r3_loaded",   read_reg(cpu, 3),  42);      // LDW from mem[0x100]

    // ── Phase 2: Offset addressing ─────────────────────────
    printf("\n── Phase 2: Offset addressing ──\n");

    cycles = run_until_pc(cpu, 0x24, 200);
    printf("  Phase 2 in %d cycles\n", cycles);

    check("r4_value",    read_reg(cpu, 4),  99);
    check("r5_offset",   read_reg(cpu, 5),  99);      // LDW [R2 + #4]
    check("r6_original", read_reg(cpu, 6),  42);      // LDW [R2] still 42

    // ── Phase 3: Overwrite ─────────────────────────────────
    printf("\n── Phase 3: Overwrite ──\n");

    cycles = run_until_pc(cpu, 0x30, 200);
    printf("  Phase 3 in %d cycles\n", cycles);

    check("r7_value",      read_reg(cpu, 7),  0xFF);
    check("r8_overwritten", read_reg(cpu, 8), 0xFF);   // mem[0x100] now 255

    // ── Summary ────────────────────────────────────────────
    printf("\ncpu_mem: %d/%d tests passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);

    delete cpu;
    return errors ? 1 : 0;
}
