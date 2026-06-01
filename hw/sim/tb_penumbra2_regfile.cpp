// Verilator testbench for penumbra2_regfile.
//
// Exercises the contract in doc/internals/penumbra2/regfile.md:
//   - R0 reads as zero; writes to R0 are discarded (override wins).
//   - R1-R13 read/write through both ports.
//   - R15 reads return i_pc; writes to R15 are discarded.
//   - R14 banking: USP when SR.S=0, SSP when SR.S=1, independent.
//   - cross_bank: supervisor code (SR.S=1) with cross_bank=1 reaches
//     USP, the WRSPR/RDSPR USP path (sp_select = SR.S ^ cross_bank).
//   - No write-through: a read in the same cycle as a write to the
//     same register returns the old value (regfile.md §7).
//   - Write-enable gating.
//   - Reset drives USP and SSP to 0. R1-R13 are NOT reset (the
//     scoreboard owns liveness), so they are only read after a write.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_regfile.h"

static int errors = 0;
static int tests = 0;

static void tick(Vpenumbra2_regfile* dut) {
    dut->i_clk = 0;
    dut->eval();
    dut->i_clk = 1;
    dut->eval();
}

static void check(const char* name, uint32_t got, uint32_t expected) {
    tests++;
    if (got != expected) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", name, got, expected);
        errors++;
    }
}

// Drive a write and clock it in. cross_bank only matters for R14.
static void write_reg(Vpenumbra2_regfile* dut, int addr, uint32_t data,
                      bool supervisor = false, bool cross_bank = false) {
    dut->i_wr_addr = addr;
    dut->i_wr_data = data;
    dut->i_wr_en = 1;
    dut->i_supervisor = supervisor ? 1 : 0;
    dut->i_cross_bank = cross_bank ? 1 : 0;
    tick(dut);
    dut->i_wr_en = 0;
}

// Combinational read through port A.
static uint32_t read_a(Vpenumbra2_regfile* dut, int addr,
                       bool supervisor = false, bool cross_bank = false) {
    dut->i_rd_addr_a = addr;
    dut->i_supervisor = supervisor ? 1 : 0;
    dut->i_cross_bank = cross_bank ? 1 : 0;
    dut->eval();
    return dut->o_rd_data_a;
}

int main() {
    Vpenumbra2_regfile* dut = new Vpenumbra2_regfile;

    // ── Reset ────────────────────────────────────────────────────
    dut->i_rst = 1;
    dut->i_wr_en = 0;
    dut->i_pc = 0;
    dut->i_supervisor = 0;
    dut->i_cross_bank = 0;
    tick(dut);
    dut->i_rst = 0;

    // ── R0 always reads zero, writes discarded ───────────────────
    check("r0_reads_zero", read_a(dut, 0), 0x00000000);
    write_reg(dut, 0, 0xDEADBEEF);
    check("r0_write_discarded", read_a(dut, 0), 0x00000000);

    // ── R1-R13 basic read/write ──────────────────────────────────
    for (int r = 1; r <= 13; r++) {
        uint32_t val = 0x100000 * r + 0x12345;
        write_reg(dut, r, val);
        char name[32];
        snprintf(name, sizeof(name), "r%d_readback", r);
        check(name, read_a(dut, r), val);
    }

    // ── Both read ports are independent ──────────────────────────
    dut->i_rd_addr_a = 1;
    dut->i_rd_addr_b = 2;
    dut->eval();
    check("port_a_reads_r1", dut->o_rd_data_a, 0x00112345);
    check("port_b_reads_r2", dut->o_rd_data_b, 0x00212345);

    dut->i_rd_addr_a = 5;
    dut->i_rd_addr_b = 5;
    dut->eval();
    check("both_ports_same_reg", dut->o_rd_data_a, dut->o_rd_data_b);

    // ── R15 returns PC; writes discarded ─────────────────────────
    dut->i_pc = 0x00001000;
    check("r15_reads_pc", read_a(dut, 15), 0x00001000);
    dut->i_pc = 0x0000FFFC;
    check("r15_tracks_pc", read_a(dut, 15), 0x0000FFFC);
    write_reg(dut, 15, 0xBAAAAAAD);
    dut->i_pc = 0x00002000;
    check("r15_write_discarded", read_a(dut, 15), 0x00002000);

    // ── R14 banking (USP/SSP) ────────────────────────────────────
    write_reg(dut, 14, 0xAAAA0000, /*supervisor=*/false);
    check("r14_usp_readback", read_a(dut, 14, /*supervisor=*/false), 0xAAAA0000);
    // SSP is still 0 from reset.
    check("r14_ssp_after_switch", read_a(dut, 14, /*supervisor=*/true), 0x00000000);
    write_reg(dut, 14, 0xBBBB0000, /*supervisor=*/true);
    check("r14_ssp_readback", read_a(dut, 14, /*supervisor=*/true), 0xBBBB0000);
    check("r14_usp_preserved", read_a(dut, 14, /*supervisor=*/false), 0xAAAA0000);

    // ── Cross-bank (RDSPR/WRSPR USP from supervisor) ─────────────
    // sp_select = SR.S ^ cross_bank, so supervisor + cross_bank=1
    // reaches USP while SR.S=1.
    check("crossbank_read_usp",
          read_a(dut, 14, /*supervisor=*/true, /*cross_bank=*/true), 0xAAAA0000);
    // A cross-bank write from supervisor lands in USP, not SSP.
    write_reg(dut, 14, 0xCCCC0000, /*supervisor=*/true, /*cross_bank=*/true);
    check("crossbank_write_hits_usp",
          read_a(dut, 14, /*supervisor=*/false), 0xCCCC0000);
    check("crossbank_ssp_untouched",
          read_a(dut, 14, /*supervisor=*/true), 0xBBBB0000);

    // ── No write-through (regfile.md §7) ─────────────────────────
    // While a write to R3 is asserted but not yet clocked, a read of
    // R3 in the same cycle must still return the OLD value.
    uint32_t old_r3 = read_a(dut, 3);
    dut->i_wr_addr = 3;
    dut->i_wr_data = 0x33330000;
    dut->i_wr_en = 1;
    dut->i_supervisor = 0;
    dut->i_cross_bank = 0;
    dut->i_rd_addr_a = 3;
    dut->eval();                                  // same cycle as the write
    check("no_write_through_old_value", dut->o_rd_data_a, old_r3);
    tick(dut);                                    // clock the write in
    dut->i_wr_en = 0;
    check("write_visible_next_cycle", read_a(dut, 3), 0x33330000);

    // ── Write-enable gating ──────────────────────────────────────
    uint32_t old_r1 = read_a(dut, 1);
    dut->i_wr_addr = 1;
    dut->i_wr_data = 0xFFFFFFFF;
    dut->i_wr_en = 0;
    tick(dut);
    check("wr_en_gating", read_a(dut, 1), old_r1);

    // ── Reset drives USP/SSP to 0 ────────────────────────────────
    dut->i_rst = 1;
    tick(dut);
    dut->i_rst = 0;
    check("reset_usp", read_a(dut, 14, /*supervisor=*/false), 0x00000000);
    check("reset_ssp", read_a(dut, 14, /*supervisor=*/true), 0x00000000);

    // ── Summary ──────────────────────────────────────────────────
    printf("penumbra2_regfile: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete dut;
    return (errors > 0) ? 1 : 0;
}
