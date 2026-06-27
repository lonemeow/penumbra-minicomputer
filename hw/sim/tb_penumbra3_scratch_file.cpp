// Verilator testbench for penumbra3_scratch_file.
//
// SCR0..SCR3 are plain supervisor scratch SPRs (one WB write port, one
// combinational RDSPR read port). Checks: each SCRn writes and reads back; the
// four entries do not cross-wire (distinct per-entry patterns); a non-SCRn read
// returns 0; write-enable gating holds the entry. (A non-SCRn *write* is barred
// by the module assertion -- the spine gates i_we to SCRn -- so it is not
// driven here.)

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_scratch_file.h"

// SPR numbers (penumbra_pkg)
enum { SPR_EPC = 1, SPR_SCR0 = 4, SPR_SCR1 = 5, SPR_SCR2 = 6, SPR_SCR3 = 7 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_scratch_file* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

static void write_scr(Vpenumbra3_scratch_file* dut, int sel, uint32_t data) {
    dut->i_we = 1; dut->i_w_sel = sel; dut->i_w_value = data;
    tick(dut);
    dut->i_we = 0;
}

static uint32_t read_scr(Vpenumbra3_scratch_file* dut, int sel) {
    dut->i_rd_sel = sel; dut->eval();
    return dut->o_rd_value;
}

int main() {
    Vpenumbra3_scratch_file* dut = new Vpenumbra3_scratch_file;

    dut->i_rst = 1; dut->i_we = 0; tick(dut); dut->i_rst = 0;

    // Distinct per-entry patterns, written then read back individually.
    const int  sel[4] = {SPR_SCR0, SPR_SCR1, SPR_SCR2, SPR_SCR3};
    const uint32_t pat[4] = {0x5C500000, 0x5C511111, 0x5C522222, 0x5C533333};

    for (int i = 0; i < 4; i++) write_scr(dut, sel[i], pat[i]);

    // No cross-wiring: every entry holds exactly its own pattern.
    for (int i = 0; i < 4; i++) {
        char name[24];
        snprintf(name, sizeof(name), "scr%d_readback", i);
        check(name, read_scr(dut, sel[i]), pat[i]);
    }

    // A non-SCRn read returns 0.
    check("non_scr_reads_zero", read_scr(dut, SPR_EPC), 0);

    // Write-enable gating: a quiet cycle leaves the entry untouched.
    uint32_t old = read_scr(dut, SPR_SCR2);
    dut->i_we = 0; dut->i_w_sel = SPR_SCR2; dut->i_w_value = 0xFFFFFFFF;
    tick(dut);
    check("wr_en_gating", read_scr(dut, SPR_SCR2), old);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}
