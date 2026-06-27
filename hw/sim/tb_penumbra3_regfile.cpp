// Verilator testbench for penumbra3_regfile.
//
// The file is physically indexed (penumbra3_regmap did the ISA->phys mapping),
// so the testbench drives physical entry indices directly:
//   0      R0  -- reads 0, writes land in a dead slot
//   1-13   R1-R13 array storage
//   14     USP flop
//   15     SSP flop
//
// Checks: entry 0 reads 0 regardless of writes; entries 1-13 read/write through
// both ports independently; USP/SSP are independent flops; no write-through (a
// read in the write cycle returns the old value); write-enable gating; reset
// drives USP/SSP to 0.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_regfile.h"

enum { SB_USP = 14, SB_SSP = 15 };

static int errors = 0, tests = 0;

static void tick(Vpenumbra3_regfile* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static void check(const char* name, uint32_t got, uint32_t expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", name, got, expected);
        errors++;
    }
}

static void write_reg(Vpenumbra3_regfile* dut, int idx, uint32_t data) {
    dut->i_wr_idx = idx; dut->i_wr_data = data; dut->i_wr_en = 1;
    tick(dut);
    dut->i_wr_en = 0;
}

static uint32_t read_a(Vpenumbra3_regfile* dut, int idx) {
    dut->i_rd_idx_a = idx; dut->eval();
    return dut->o_rd_data_a;
}

int main() {
    Vpenumbra3_regfile* dut = new Vpenumbra3_regfile;

    // ── Reset ────────────────────────────────────────────────────
    dut->i_rst = 1; dut->i_wr_en = 0; tick(dut); dut->i_rst = 0;

    // ── Entry 0 (R0) always reads zero, writes discarded ─────────
    check("r0_reads_zero", read_a(dut, 0), 0x00000000);
    write_reg(dut, 0, 0xDEADBEEF);
    check("r0_write_discarded", read_a(dut, 0), 0x00000000);

    // ── Entries 1-13 basic read/write ────────────────────────────
    for (int r = 1; r <= 13; r++) {
        uint32_t val = 0x100000 * r + 0x12345;
        write_reg(dut, r, val);
        char name[32];
        snprintf(name, sizeof(name), "r%d_readback", r);
        check(name, read_a(dut, r), val);
    }

    // ── Both read ports are independent ──────────────────────────
    dut->i_rd_idx_a = 1; dut->i_rd_idx_b = 2; dut->eval();
    check("port_a_reads_r1", dut->o_rd_data_a, 0x00112345);
    check("port_b_reads_r2", dut->o_rd_data_b, 0x00212345);

    dut->i_rd_idx_a = 5; dut->i_rd_idx_b = 5; dut->eval();
    check("both_ports_same_reg", dut->o_rd_data_a, dut->o_rd_data_b);

    // ── USP (14) and SSP (15) are independent ────────────────────
    write_reg(dut, SB_USP, 0xAAAA0000);
    check("usp_readback", read_a(dut, SB_USP), 0xAAAA0000);
    check("ssp_after_usp_write", read_a(dut, SB_SSP), 0x00000000);  // SSP still 0 from reset
    write_reg(dut, SB_SSP, 0xBBBB0000);
    check("ssp_readback", read_a(dut, SB_SSP), 0xBBBB0000);
    check("usp_preserved", read_a(dut, SB_USP), 0xAAAA0000);

    // ── No write-through ─────────────────────────────────────────
    uint32_t old_r3 = read_a(dut, 3);
    dut->i_wr_idx = 3; dut->i_wr_data = 0x33330000; dut->i_wr_en = 1;
    dut->i_rd_idx_a = 3; dut->eval();                  // same cycle as the write
    check("no_write_through_old_value", dut->o_rd_data_a, old_r3);
    tick(dut); dut->i_wr_en = 0;                       // clock the write in
    check("write_visible_next_cycle", read_a(dut, 3), 0x33330000);

    // ── Write-enable gating ──────────────────────────────────────
    uint32_t old_r1 = read_a(dut, 1);
    dut->i_wr_idx = 1; dut->i_wr_data = 0xFFFFFFFF; dut->i_wr_en = 0;
    tick(dut);
    check("wr_en_gating", read_a(dut, 1), old_r1);

    // ── Reset drives USP/SSP to 0 ────────────────────────────────
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    check("reset_usp", read_a(dut, SB_USP), 0x00000000);
    check("reset_ssp", read_a(dut, SB_SSP), 0x00000000);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return (errors > 0) ? 1 : 0;
}
