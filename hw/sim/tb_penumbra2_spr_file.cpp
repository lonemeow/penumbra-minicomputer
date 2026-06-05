// Verilator testbench for penumbra2_spr_file.
//
// Drives the SR/EPC/ESR write sources and the RDSPR read mux across clock
// edges, checking the privileged save-state contract in
// doc/internals/penumbra2/exception-flow.md and the SR bit layout in
// architecture.md:
//   - reset: SR = {S=1, I=0, NZCV=0}; EPC = ESR = 0
//   - NZCV flag commit updates only SR[3:0], leaving S/I
//   - WRSPR SR bulk-loads, sanitised to the live bits (reserved dropped)
//   - WRSPR EPC / ESR land and read back via RDSPR
//   - save-state: EPC<-save_pc, ESR<-pre-entry SR, SR.S=1/I=0, NZCV preserved
//   - ERET restores SR from ESR (re-banking S/I)
//   - priority: save-state wins over a coincident flag commit
//   - RDSPR read mux selects EPC/ESR/SR, else 0
//
// SR's `--assert` invariants (entry → S=1/I=0, EPC/ESR latch, bulk-writer
// mutual exclusion) ride along: a failed assertion aborts the sim (exit 1).

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_spr_file.h"
#include "verilated.h"

// SPR numbers (penumbra_pkg)
enum { SPR_ESR = 0, SPR_EPC = 1, SPR_USP = 2, SPR_SR = 3 };

static int errors = 0, tests = 0;

static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra2_spr_file* dut) {
    dut->i_clk = 0; dut->eval();
    dut->i_clk = 1; dut->eval();
}

// Quiet baseline: no write source active, RDSPR reading SR.
static void clear(Vpenumbra2_spr_file* dut) {
    dut->i_flag_we = 0; dut->i_flag_value = 0;
    dut->i_save_state = 0; dut->i_save_pc = 0;
    dut->i_eret = 0;
    dut->i_spr_we = 0; dut->i_spr_sel = 0; dut->i_spr_value = 0;
    dut->i_rd_sel = SPR_SR;
}

// Bulk-load SR via WRSPR SR (the only way to set an arbitrary SR for setup).
static void load_sr(Vpenumbra2_spr_file* dut, uint32_t v) {
    clear(dut);
    dut->i_spr_we = 1; dut->i_spr_sel = SPR_SR; dut->i_spr_value = v;
    dut->eval(); tick(dut);
    clear(dut); dut->eval();
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra2_spr_file* dut = new Vpenumbra2_spr_file;

    // ── Reset ────────────────────────────────────────────────────
    clear(dut);
    dut->i_rst = 1; tick(dut); dut->i_rst = 0;
    dut->eval();
    check("reset_sr",    dut->o_sr_read, 0x80000000);  // S=1, I=0, NZCV=0
    check("reset_sr_s",  dut->o_sr_s, 1);
    check("reset_sr_i",  dut->o_sr_i, 0);
    check("reset_epc",   dut->o_epc, 0);
    check("reset_esr",   dut->o_esr, 0);

    // ── NZCV flag commit touches only SR[3:0] ────────────────────
    clear(dut);
    dut->i_flag_we = 1; dut->i_flag_value = 0xF;
    dut->eval(); tick(dut); clear(dut); dut->eval();
    check("flag_nzcv",   dut->o_sr_flags, 0xF);
    check("flag_keeps_s", dut->o_sr_s, 1);        // S untouched
    check("flag_sr_word", dut->o_sr_read, 0x8000000F);

    // ── WRSPR SR bulk-load is sanitised to the live bits ─────────
    load_sr(dut, 0xFFFFFFFF);                      // all ones in → only S,I,NZCV survive
    check("wrspr_sr_sanitised", dut->o_sr_read, 0xC000000F);  // S=1,I=1,NZCV=0xF
    check("wrspr_sr_i", dut->o_sr_i, 1);

    // ── WRSPR EPC / ESR land and read back ───────────────────────
    clear(dut);
    dut->i_spr_we = 1; dut->i_spr_sel = SPR_EPC; dut->i_spr_value = 0x12340000;
    dut->eval(); tick(dut); clear(dut); dut->eval();
    check("wrspr_epc", dut->o_epc, 0x12340000);
    dut->i_rd_sel = SPR_EPC; dut->eval();
    check("rdspr_epc", dut->o_rd_value, 0x12340000);

    clear(dut);
    dut->i_spr_we = 1; dut->i_spr_sel = SPR_ESR; dut->i_spr_value = 0xAABBCCDD;
    dut->eval(); tick(dut); clear(dut); dut->eval();
    check("wrspr_esr", dut->o_esr, 0xAABBCCDD);
    dut->i_rd_sel = SPR_ESR; dut->eval();
    check("rdspr_esr", dut->o_rd_value, 0xAABBCCDD);

    // RDSPR default (a non-held SPR) reads 0.
    dut->i_rd_sel = SPR_USP; dut->eval();
    check("rdspr_usp_zero", dut->o_rd_value, 0);

    // ── Save-state: snapshot pre-entry SR, enter S=1/I=0, keep NZCV ──
    load_sr(dut, 0x40000005);                      // S=0, I=1, NZCV=0x5
    clear(dut);
    dut->i_save_state = 1; dut->i_save_pc = 0xDEADBEEF;
    dut->eval(); tick(dut); clear(dut); dut->eval();
    check("entry_epc",      dut->o_epc, 0xDEADBEEF);
    check("entry_esr",      dut->o_esr, 0x40000005);   // pre-entry SR snapshot
    check("entry_sr_s",     dut->o_sr_s, 1);           // entered supervisor
    check("entry_sr_i",     dut->o_sr_i, 0);           // interrupts masked
    check("entry_nzcv_kept", dut->o_sr_flags, 0x5);    // flags survive entry
    check("entry_sr_word",  dut->o_sr_read, 0x80000005);

    // ── ERET restores SR from ESR (re-banks S/I) ─────────────────
    clear(dut);
    dut->i_eret = 1;
    dut->eval(); tick(dut); clear(dut); dut->eval();
    check("eret_sr",   dut->o_sr_read, 0x40000005);    // == the ESR we snapshotted
    check("eret_sr_s", dut->o_sr_s, 0);
    check("eret_sr_i", dut->o_sr_i, 1);

    // ── Priority: save-state wins over a coincident flag commit ──
    load_sr(dut, 0x00000003);                      // S=0, I=0, NZCV=0x3
    clear(dut);
    dut->i_save_state = 1; dut->i_save_pc = 0x1000;
    dut->i_flag_we = 1; dut->i_flag_value = 0xC;    // would set NZCV=0xC if it won
    dut->eval(); tick(dut); clear(dut); dut->eval();
    check("prio_entry_nzcv_held", dut->o_sr_flags, 0x3);  // flag commit lost; NZCV preserved
    check("prio_entry_s",         dut->o_sr_s, 1);

    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete dut;
    return errors ? 1 : 0;
}
