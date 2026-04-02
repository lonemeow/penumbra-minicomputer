// Verilator testbench for autoconfig_dev wrapper
//
// Tests config chain, config space reads, base address latching,
// and normal bus access after configuration. Uses autoconfig_test
// wrapper with two devices: SPI (4 KB) and RAM (64 bytes).

#include <cstdio>
#include <cstdint>
#include "Vautoconfig_test.h"

static int errors = 0, tests = 0;

static void tick(Vautoconfig_test* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vautoconfig_test* d) {
    d->i_rst = 1;
    d->i_bus_rst = 0;
    d->i_cfg_en = 0;
    d->i_addr = 0;
    d->i_wdata = 0;
    d->i_byte_en = 0xF;
    d->i_we = 0;
    d->i_re = 0;
    tick(d); tick(d);
    d->i_rst = 0;
}

// Read a word from the bus. Returns data after 1-cycle read latency.
static uint32_t bus_read(Vautoconfig_test* d, uint32_t addr) {
    d->i_addr = addr;
    d->i_re = 1;
    tick(d);
    d->i_re = 0;
    // Wait for busy to clear
    while (d->o_busy) tick(d);
    uint32_t val = d->o_rdata;
    tick(d);  // drain
    return val;
}

// Write a word to the bus.
static void bus_write(Vautoconfig_test* d, uint32_t addr, uint32_t data) {
    d->i_addr = addr;
    d->i_wdata = data;
    d->i_we = 1;
    d->i_byte_en = 0xF;
    tick(d);
    d->i_we = 0;
    while (d->o_busy) tick(d);
    tick(d);  // drain
}

// Read config space register (offset 0x00–0x1C from 0xFE000000)
static uint32_t cfg_read(Vautoconfig_test* d, uint32_t offset) {
    return bus_read(d, 0xFE000000 + offset);
}

// Write config space register
static void cfg_write(Vautoconfig_test* d, uint32_t offset, uint32_t data) {
    bus_write(d, 0xFE000000 + offset, data);
}

#define CHECK(name, cond) do { \
    tests++; \
    if (!(cond)) { errors++; printf("  FAIL: %s\n", name); } \
} while (0)

#define CHECK_EQ(name, got, expected) do { \
    tests++; \
    if ((got) != (expected)) { \
        errors++; \
        printf("  FAIL: %s: got 0x%08x, expected 0x%08x\n", \
               name, (uint32_t)(got), (uint32_t)(expected)); \
    } \
} while (0)

static void test_initial_state(Vautoconfig_test* d) {
    printf("test_initial_state\n");
    reset(d);

    // Both devices unconfigured, chain blocked
    CHECK("dev0 cfg_out low (unconfigured)", d->o_dev0_cfg_out == 0);
    CHECK("dev1 cfg_out low (unconfigured)", d->o_dev1_cfg_out == 0);

    // Without cfg_en, config space should not respond
    CHECK("no selection without cfg_en", d->o_sel == 0);
}

static void test_config_read_dev0(Vautoconfig_test* d) {
    printf("test_config_read_dev0\n");
    reset(d);
    d->i_cfg_en = 1;
    tick(d);

    // First device in chain should respond
    CHECK_EQ("dev0 CLASS = SPI", cfg_read(d, 0x00), 3);       // ACFG_CLASS_SPI
    CHECK_EQ("dev0 SIZE = 4096", cfg_read(d, 0x04), 4096);
    CHECK_EQ("dev0 ID = 0",     cfg_read(d, 0x08), 0);
    CHECK_EQ("dev0 NAME0 = SPI", cfg_read(d, 0x0C), 0x00495053);  // "SPI\0"
    CHECK_EQ("dev0 NAME1 = 0",  cfg_read(d, 0x10), 0);

    // dev1 should not be visible yet (dev0 blocks the chain)
    CHECK("dev0 cfg_out still low", d->o_dev0_cfg_out == 0);
}

static void test_configure_dev0(Vautoconfig_test* d) {
    printf("test_configure_dev0\n");
    reset(d);
    d->i_cfg_en = 1;
    tick(d);

    // Assign base address 0x10000000 to dev0
    cfg_write(d, 0x1C, 0x10000000);

    // dev0 should now be configured, chain passes through
    CHECK("dev0 cfg_out high (configured)", d->o_dev0_cfg_out == 1);
    CHECK("dev1 cfg_out low (still unconfigured)", d->o_dev1_cfg_out == 0);
}

static void test_config_read_dev1_after_dev0(Vautoconfig_test* d) {
    printf("test_config_read_dev1_after_dev0\n");
    reset(d);
    d->i_cfg_en = 1;
    tick(d);

    // Configure dev0
    cfg_write(d, 0x1C, 0x10000000);

    // Now config reads should hit dev1
    CHECK_EQ("dev1 CLASS = MEMORY", cfg_read(d, 0x00), 1);    // ACFG_CLASS_MEMORY
    CHECK_EQ("dev1 SIZE = 64",     cfg_read(d, 0x04), 64);
    CHECK_EQ("dev1 NAME0 = RAM",   cfg_read(d, 0x0C), 0x004D4152);  // "RAM\0"
}

static void test_configure_both(Vautoconfig_test* d) {
    printf("test_configure_both\n");
    reset(d);
    d->i_cfg_en = 1;
    tick(d);

    // Configure dev0 at 0x10000000
    cfg_write(d, 0x1C, 0x10000000);
    CHECK("dev0 configured", d->o_dev0_cfg_out == 1);

    // Configure dev1 at 0x20000000
    cfg_write(d, 0x1C, 0x20000000);
    CHECK("dev1 configured", d->o_dev1_cfg_out == 1);

    // Disable config mode
    d->i_cfg_en = 0;
    tick(d);
}

static void test_device_access_after_config(Vautoconfig_test* d) {
    printf("test_device_access_after_config\n");
    reset(d);
    d->i_cfg_en = 1;
    tick(d);

    // Configure dev0 at 0x10000000, dev1 at 0x20000000
    cfg_write(d, 0x1C, 0x10000000);
    cfg_write(d, 0x1C, 0x20000000);
    d->i_cfg_en = 0;
    tick(d);

    // Write to dev0 (offset 0 within its 4KB space)
    bus_write(d, 0x10000000, 0xDEADBEEF);
    CHECK_EQ("dev0 read back", bus_read(d, 0x10000000), 0xDEADBEEF);

    // Write to dev1 (offset 0 within its 64-byte space)
    bus_write(d, 0x20000000, 0xCAFEBABE);
    CHECK_EQ("dev1 read back", bus_read(d, 0x20000000), 0xCAFEBABE);

    // Devices don't interfere
    CHECK_EQ("dev0 still intact", bus_read(d, 0x10000000), 0xDEADBEEF);
}

static void test_bus_rst_reconfigures(Vautoconfig_test* d) {
    printf("test_bus_rst_reconfigures\n");
    reset(d);
    d->i_cfg_en = 1;
    tick(d);

    // Configure both devices
    cfg_write(d, 0x1C, 0x10000000);
    cfg_write(d, 0x1C, 0x20000000);
    d->i_cfg_en = 0;
    tick(d);

    // Verify devices respond at their assigned addresses
    bus_write(d, 0x10000000, 0x11111111);
    CHECK_EQ("dev0 accessible", bus_read(d, 0x10000000), 0x11111111);
    bus_write(d, 0x20000000, 0x22222222);
    CHECK_EQ("dev1 accessible", bus_read(d, 0x20000000), 0x22222222);

    // Assert bus reset
    d->i_bus_rst = 1;
    tick(d);
    d->i_bus_rst = 0;
    tick(d);

    // Both should be unconfigured again
    CHECK("dev0 unconfigured after rst", d->o_dev0_cfg_out == 0);
    CHECK("dev1 unconfigured after rst", d->o_dev1_cfg_out == 0);

    // Config reads should hit dev0 again
    d->i_cfg_en = 1;
    tick(d);
    CHECK_EQ("dev0 visible again", cfg_read(d, 0x00), 3);  // CLASS_SPI
}

static void test_no_sel_when_unconfigured(Vautoconfig_test* d) {
    printf("test_no_sel_when_unconfigured\n");
    reset(d);

    // Without cfg_en, reading any address should not select
    d->i_addr = 0x10000000;
    d->i_re = 1;
    d->eval();
    CHECK("no sel at random addr", d->o_sel == 0);
    d->i_re = 0;

    // Even the config space address, without cfg_en
    d->i_addr = 0xFE000000;
    d->i_re = 1;
    d->eval();
    CHECK("no sel at config addr without cfg_en", d->o_sel == 0);
    d->i_re = 0;
}

static void test_config_at_different_bases(Vautoconfig_test* d) {
    printf("test_config_at_different_bases\n");
    reset(d);
    d->i_cfg_en = 1;
    tick(d);

    // Configure dev0 at a different address
    cfg_write(d, 0x1C, 0xFF001000);
    cfg_write(d, 0x1C, 0x00100000);
    d->i_cfg_en = 0;
    tick(d);

    // Access dev0 at its new base
    bus_write(d, 0xFF001000, 0x12345678);
    CHECK_EQ("dev0 at alt base", bus_read(d, 0xFF001000), 0x12345678);

    // Access dev1 at its new base
    bus_write(d, 0x00100000, 0xABCD0000);
    CHECK_EQ("dev1 at alt base", bus_read(d, 0x00100000), 0xABCD0000);
}

int main() {
    Vautoconfig_test* d = new Vautoconfig_test;

    test_initial_state(d);
    test_config_read_dev0(d);
    test_configure_dev0(d);
    test_config_read_dev1_after_dev0(d);
    test_configure_both(d);
    test_device_access_after_config(d);
    test_bus_rst_reconfigures(d);
    test_no_sel_when_unconfigured(d);
    test_config_at_different_bases(d);

    printf("\nautoconfig: %d/%d passed\n", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
