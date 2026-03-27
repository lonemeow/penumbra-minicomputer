// Verilator testbench for the Penumbra CPU Top
//
// Phase 1: Basic ALU test (no interrupts)
// Phase 2: Interrupt entry test (EI, ei_shadow, IRQ dispatch)

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

// Run until PC reaches or passes target, with cycle limit
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

    // ── Phase 1: Basic ALU (no IRQ) ────────────────────────
    printf("── Phase 1: Basic ALU ──\n");
    reset(cpu);
    check("reset_pc", cpu->o_pc, 0x00000000);

    // Run first 5 instructions: LLI R1,5 / LLI R5,0xFF / LLI R2,3 / ADD / LLI R3,0
    // PC should reach 0x14 (past 5th instruction at 0x10)
    int cycles = run_until_pc(cpu, 0x14, 60);
    printf("  5 instructions in %d cycles (%.1f CPI)\n", cycles, cycles / 5.0);

    check("r0_zero",   read_reg(cpu, 0),  0);
    check("r1_result", read_reg(cpu, 1),  8);    // 5 + 3
    check("r2_value",  read_reg(cpu, 2),  3);
    check("r3_clear",  read_reg(cpu, 3),  0);
    check("r5_imm",    read_reg(cpu, 5),  0xFF);

    // ── Phase 2: IRQ disabled after reset ──────────────────
    printf("\n── Phase 2: IRQ blocked when disabled ──\n");
    reset(cpu);

    // Assert IRQ immediately — should be ignored (SR.I=0 after reset)
    cpu->i_irq = 1;
    run_until_pc(cpu, 0x14, 60);

    // Should reach 0x14 normally despite i_irq=1
    check("irq_blocked_pc", cpu->o_pc, 0x14);
    check("irq_blocked_r1", read_reg(cpu, 1), 8);

    // ── Phase 3: IRQ taken after EI + ei_shadow ────────────
    printf("\n── Phase 3: IRQ after EI ──\n");
    reset(cpu);

    // Run normally to just before EI (PC=0x14, EI at 0x14 about to execute)
    run_until_pc(cpu, 0x14, 60);
    check("pre_ei_r4", read_reg(cpu, 4), 0);   // R4 not yet written

    // Assert IRQ, then let EI execute
    cpu->i_irq = 1;

    // Run enough cycles for: EI executes, ei_shadow instr executes,
    // then IRQ fires at next dispatch, handler runs from 0x04.
    // Watch for PC to jump back to 0x04 (handler entry).
    uint32_t prev_pc = cpu->o_pc;
    int irq_cycle = -1;
    for (int i = 0; i < 50; i++) {
        tick(cpu);
        if (cpu->o_pc == 0x04 && prev_pc > 0x04) {
            irq_cycle = i;
            // Deassert IRQ after taken (edge-triggered behavior)
            cpu->i_irq = 0;
        }
        prev_pc = cpu->o_pc;
    }

    check("irq_was_taken", (irq_cycle >= 0) ? 1u : 0u, 1u);
    printf("  IRQ taken at cycle +%d\n", irq_cycle);

    // ei_shadow should have let LLI R4,#0x42 execute before IRQ
    check("ei_shadow_r4", read_reg(cpu, 4), 0x42);

    // Handler at 0x04 re-executes forward: R5=0xFF, R2=3, ADD R1,R2
    // R1 was 8, now 8+3=11
    check("handler_r5", read_reg(cpu, 5), 0xFF);

    // Run more to let handler path finish through 0x1C, 0x20
    for (int i = 0; i < 40; i++) tick(cpu);
    check("r6_after_handler", read_reg(cpu, 6), 0xDD);
    check("r7_after_handler", read_reg(cpu, 7), 0xEE);
    check("r1_second_add", read_reg(cpu, 1), 11);

    // ── Summary ────────────────────────────────────────────
    printf("\ncpu_top: %d/%d tests passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);

    delete cpu;
    return errors ? 1 : 0;
}
