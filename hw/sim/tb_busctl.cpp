// Verilator testbench for Penumbra Bus Controller (busctl)
//
// Tests the sysreg interface for bus reset and autoconfig control.

#include <cstdio>
#include <cstdint>
#include "Vbusctl.h"

static int errors = 0, tests = 0;

static void tick(Vbusctl* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vbusctl* d) {
    d->i_rst = 1;
    d->i_sys_reg = 0;
    d->i_sys_wdata = 0;
    d->i_sys_we = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
}

static void sys_write(Vbusctl* d, uint32_t reg, uint32_t data) {
    d->i_sys_reg = reg;
    d->i_sys_wdata = data;
    d->i_sys_we = 1;
    tick(d);
    d->i_sys_we = 0;
}

static uint32_t sys_read(Vbusctl* d, uint32_t reg) {
    d->i_sys_reg = reg;
    d->eval();
    return d->o_sys_rdata;
}

#define CHECK(name, cond) do { \
    tests++; \
    if (!(cond)) { errors++; printf("  FAIL: %s\n", name); } \
} while (0)

static void test_reset_state(Vbusctl* d) {
    printf("test_reset_state\n");
    reset(d);
    CHECK("bus_rst deasserted after reset", d->o_bus_rst == 0);
    CHECK("cfg_en deasserted after reset",  d->o_cfg_en == 0);
    CHECK("BUSCTL reads 0 after reset",     sys_read(d, 0) == 0);
}

static void test_cfg_en(Vbusctl* d) {
    printf("test_cfg_en\n");
    reset(d);

    // Enable config mode
    sys_write(d, 0, 0x2);  // CFG_EN = 1
    CHECK("cfg_en asserted after write",   d->o_cfg_en == 1);
    CHECK("bus_rst not asserted",          d->o_bus_rst == 0);
    CHECK("BUSCTL reads back CFG_EN",     sys_read(d, 0) == 0x2);

    // Disable config mode
    sys_write(d, 0, 0x0);
    CHECK("cfg_en deasserted after clear", d->o_cfg_en == 0);
    CHECK("BUSCTL reads 0 after clear",   sys_read(d, 0) == 0);
}

static void test_rst_sticky(Vbusctl* d) {
    printf("test_rst_sticky\n");
    reset(d);

    // Assert RST
    sys_write(d, 0, 0x1);  // RST = 1
    CHECK("bus_rst asserted after write",  d->o_bus_rst == 1);
    CHECK("BUSCTL reads back RST",        sys_read(d, 0) == 0x1);

    // RST stays asserted (sticky, not auto-clearing)
    tick(d);
    CHECK("bus_rst still asserted",        d->o_bus_rst == 1);
    tick(d);
    CHECK("bus_rst still asserted (2)",    d->o_bus_rst == 1);

    // Software deasserts RST
    sys_write(d, 0, 0x0);
    CHECK("bus_rst deasserted after clear", d->o_bus_rst == 0);
    CHECK("BUSCTL reads 0",               sys_read(d, 0) == 0);
}

static void test_rst_with_cfg_en(Vbusctl* d) {
    printf("test_rst_with_cfg_en\n");
    reset(d);

    // Assert RST, then clear RST and set CFG_EN
    sys_write(d, 0, 0x1);  // RST only
    CHECK("bus_rst asserted",              d->o_bus_rst == 1);
    CHECK("cfg_en not asserted",           d->o_cfg_en == 0);

    // Clear RST, enable config
    sys_write(d, 0, 0x2);  // CFG_EN only
    CHECK("bus_rst deasserted",            d->o_bus_rst == 0);
    CHECK("cfg_en asserted",               d->o_cfg_en == 1);
    CHECK("BUSCTL reads CFG_EN only",     sys_read(d, 0) == 0x2);
}

static void test_rst_and_cfg_en_simultaneous(Vbusctl* d) {
    printf("test_rst_and_cfg_en_simultaneous\n");
    reset(d);

    // Set both bits at once
    sys_write(d, 0, 0x3);  // RST | CFG_EN
    CHECK("bus_rst asserted",              d->o_bus_rst == 1);
    CHECK("cfg_en asserted",               d->o_cfg_en == 1);
    CHECK("BUSCTL reads 0x3",             sys_read(d, 0) == 0x3);

    // Both persist (both sticky)
    tick(d);
    CHECK("bus_rst still asserted",        d->o_bus_rst == 1);
    CHECK("cfg_en still asserted",         d->o_cfg_en == 1);

    // Clear just RST, keep CFG_EN
    sys_write(d, 0, 0x2);
    CHECK("bus_rst deasserted",            d->o_bus_rst == 0);
    CHECK("cfg_en still asserted",         d->o_cfg_en == 1);
}

static void test_reserved_reg_reads_zero(Vbusctl* d) {
    printf("test_reserved_reg_reads_zero\n");
    reset(d);

    // Enable config mode so we have a non-zero state
    sys_write(d, 0, 0x2);

    // Read non-existent registers
    for (int reg = 1; reg <= 15; reg++) {
        uint32_t val = sys_read(d, reg);
        if (val != 0) {
            errors++;
            printf("  FAIL: reg %d reads 0x%08x (expected 0)\n", reg, val);
        }
        tests++;
    }
}

static void test_write_to_nonexistent_reg(Vbusctl* d) {
    printf("test_write_to_nonexistent_reg\n");
    reset(d);

    // Writing to reg 1 should not affect BUSCTL
    sys_write(d, 1, 0xFFFFFFFF);
    CHECK("cfg_en unchanged by bad write", d->o_cfg_en == 0);
    CHECK("bus_rst unchanged by bad write", d->o_bus_rst == 0);
}

int main() {
    Vbusctl* d = new Vbusctl;

    test_reset_state(d);
    test_cfg_en(d);
    test_rst_sticky(d);
    test_rst_with_cfg_en(d);
    test_rst_and_cfg_en_simultaneous(d);
    test_reserved_reg_reads_zero(d);
    test_write_to_nonexistent_reg(d);

    printf("\nbusctl: %d/%d passed\n", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
