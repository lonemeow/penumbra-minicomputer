// Verilator testbench for the Penumbra register file
//
// Tests:
//   - R0 always reads as zero, writes are discarded
//   - R1–R13 read/write
//   - R14 banking (USP vs KSP based on supervisor mode)
//   - R15 reads return PC input
//   - Write enable gating
//   - Both read ports are independent
//   - Reset clears all registers

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include "Vregfile.h"

static int errors = 0;
static int tests = 0;

// Helper: advance one clock cycle
static void tick(Vregfile* dut) {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
}

static void check(Vregfile* dut, const char* name,
                   uint32_t got, uint32_t expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", name, got, expected);
        errors++;
    }
}

// Write a register and tick the clock
static void write_reg(Vregfile* dut, int addr, uint32_t data, bool supervisor = false) {
    dut->i_wr_addr = addr;
    dut->i_wr_data = data;
    dut->i_wr_en = 1;
    dut->i_supervisor = supervisor ? 1 : 0;
    tick(dut);
    dut->i_wr_en = 0;
}

// Read via port A (combinational — just set address and eval)
static uint32_t read_a(Vregfile* dut, int addr, bool supervisor = false) {
    dut->i_rd_addr_a = addr;
    dut->i_supervisor = supervisor ? 1 : 0;
    dut->eval();
    return dut->o_rd_data_a;
}

// Read via port B
static uint32_t read_b(Vregfile* dut, int addr, bool supervisor = false) {
    dut->i_rd_addr_b = addr;
    dut->i_supervisor = supervisor ? 1 : 0;
    dut->eval();
    return dut->o_rd_data_b;
}

int main(int argc, char** argv) {
    Vregfile* dut = new Vregfile;

    // ── Reset ────────────────────────────────────────────────────
    dut->i_rst = 1;
    dut->i_wr_en = 0;
    dut->i_pc = 0;
    dut->i_supervisor = 0;
    tick(dut);
    dut->i_rst = 0;

    // ── R0 always reads zero ─────────────────────────────────────
    check(dut, "r0_reads_zero", read_a(dut, 0), 0x00000000);

    // Write to R0 should be discarded
    write_reg(dut, 0, 0xDEADBEEF);
    check(dut, "r0_write_discarded", read_a(dut, 0), 0x00000000);

    // ── R1–R13 basic read/write ──────────────────────────────────
    for (int r = 1; r <= 13; r++) {
        uint32_t val = 0x100000 * r + 0x12345;
        write_reg(dut, r, val);
        char name[32];
        snprintf(name, sizeof(name), "r%d_readback", r);
        check(dut, name, read_a(dut, r), val);
    }

    // ── Both read ports are independent ──────────────────────────
    dut->i_rd_addr_a = 1;
    dut->i_rd_addr_b = 2;
    dut->eval();
    check(dut, "port_a_reads_r1", dut->o_rd_data_a, 0x00112345);
    check(dut, "port_b_reads_r2", dut->o_rd_data_b, 0x00212345);

    // Both ports can read the same register
    dut->i_rd_addr_a = 5;
    dut->i_rd_addr_b = 5;
    dut->eval();
    check(dut, "both_ports_same_reg", dut->o_rd_data_a, dut->o_rd_data_b);

    // ── R15 returns PC ───────────────────────────────────────────
    dut->i_pc = 0x00001000;
    check(dut, "r15_reads_pc", read_a(dut, 15), 0x00001000);

    dut->i_pc = 0x0000FFFC;
    check(dut, "r15_tracks_pc", read_a(dut, 15), 0x0000FFFC);

    // Write to R15 should be discarded
    write_reg(dut, 15, 0xBAAAAAAD);
    dut->i_pc = 0x00002000;
    check(dut, "r15_write_discarded", read_a(dut, 15), 0x00002000);

    // ── R14 banking (USP/KSP) ────────────────────────────────────
    // Write USP in user mode
    write_reg(dut, 14, 0xAAAA0000, /*supervisor=*/false);
    check(dut, "r14_usp_readback", read_a(dut, 14, /*supervisor=*/false), 0xAAAA0000);

    // Switch to supervisor — R14 should now read KSP (which is 0 from reset)
    check(dut, "r14_ksp_after_switch", read_a(dut, 14, /*supervisor=*/true), 0x00000000);

    // Write KSP in supervisor mode
    write_reg(dut, 14, 0xBBBB0000, /*supervisor=*/true);
    check(dut, "r14_ksp_readback", read_a(dut, 14, /*supervisor=*/true), 0xBBBB0000);

    // USP should still be intact
    check(dut, "r14_usp_preserved", read_a(dut, 14, /*supervisor=*/false), 0xAAAA0000);

    // ── Write enable gating ──────────────────────────────────────
    // With wr_en=0, writes should not take effect
    uint32_t old_r1 = read_a(dut, 1);
    dut->i_wr_addr = 1;
    dut->i_wr_data = 0xFFFFFFFF;
    dut->i_wr_en = 0;
    tick(dut);
    check(dut, "wr_en_gating", read_a(dut, 1), old_r1);

    // ── Reset clears all registers ───────────────────────────────
    dut->i_rst = 1;
    tick(dut);
    dut->i_rst = 0;

    for (int r = 1; r <= 13; r++) {
        char name[32];
        snprintf(name, sizeof(name), "reset_r%d", r);
        check(dut, name, read_a(dut, r), 0x00000000);
    }
    check(dut, "reset_usp", read_a(dut, 14, /*supervisor=*/false), 0x00000000);
    check(dut, "reset_ksp", read_a(dut, 14, /*supervisor=*/true), 0x00000000);

    // ── Summary ──────────────────────────────────────────────────
    printf("regfile: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}
