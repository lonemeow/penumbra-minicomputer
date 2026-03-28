// Debug testbench for WRSYS/RDSYS — traces PC, registers, and state
#include <cstdio>
#include <cstdint>
#include "Vcpu_top.h"
#include "verilated_vcd_c.h"

static VerilatedVcdC* tfp;
static uint64_t sim_time = 0;

static void tick(Vcpu_top* d) {
    d->i_clk = 0; d->eval(); tfp->dump(sim_time++);
    d->i_clk = 1; d->eval(); tfp->dump(sim_time++);
}

static uint32_t read_reg(Vcpu_top* cpu, int reg) {
    cpu->i_dbg_reg_addr = reg;
    cpu->eval();
    return cpu->o_dbg_reg_data;
}

int main() {
    Vcpu_top* cpu = new Vcpu_top;
    Verilated::traceEverOn(true);
    tfp = new VerilatedVcdC;
    cpu->trace(tfp, 99);
    tfp->open("waves/sysreg_debug.vcd");
    cpu->i_rst = 1; cpu->i_irq = 0; cpu->i_dbg_reg_addr = 0;
    tick(cpu); tick(cpu);
    cpu->i_rst = 0;

    printf("Cyc  PC        R1        R2\n");
    printf("---  --------  --------  --------\n");

    for (int i = 0; i < 40; i++) {
        tick(cpu);
        uint32_t pc = cpu->o_pc;
        uint32_t r1 = read_reg(cpu, 1);
        uint32_t r2 = read_reg(cpu, 2);
        printf("%3d  %08X  %08X  %08X\n", i+1, pc, r1, r2);

        // Stop when PC is stable (halt loop)
        static uint32_t prev_pc = 0xFFFFFFFF;
        static int stable = 0;
        if (pc == prev_pc) { stable++; if (stable >= 5) break; }
        else { stable = 0; prev_pc = pc; }
    }

    printf("\nFinal: R1=%08X R2=%08X\n", read_reg(cpu, 1), read_reg(cpu, 2));
    tfp->close();
    delete cpu;
    return 0;
}
