// Verilator testbench for penumbra3_mem1_stage (via penumbra3_mem1_stage_test).
//
// Drives accesses and checks MEM1's launch + register:
//   - a load/store launches the translation + cache index with the right
//     direction, and latches the MEM1/MEM2 register
//   - a misaligned access suppresses the launch and flags align_fault
//   - a store prepares the byte-enable + lane-replicated data
//   - RDSYS drives the sysreg read launch
//   - the handshake: i_hold holds the register, i_bubble flushes it

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_mem1_stage_test.h"
#include "verilated.h"

enum { OPC_LOAD = 1, OPC_STORE = 2, OPC_RDSYS = 8 };
enum { ACC_READ = 1, ACC_WRITE = 2 };
enum { MEM_SZ_BYTE = 0, MEM_SZ_HALF = 1, MEM_SZ_WORD = 2 };

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}
static void tick(Vpenumbra3_mem1_stage_test* d) {
    d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval();
}
static uint32_t enc_m(int L, int sz, int se, int rd, int rb, uint16_t off) {
    return (2u << 30) | ((L & 1) << 29) | ((sz & 3) << 27) | ((se & 1) << 26)
         | ((rd & 0xF) << 22) | ((rb & 0xF) << 18) | ((uint32_t)off << 2);
}
static uint32_t enc_r(int op, int rd, int rs, int f) {
    return (0u << 30) | ((op & 0x1F) << 25) | ((rd & 0xF) << 21)
         | ((rs & 0xF) << 17) | ((f & 1) << 16);
}
static void clr(Vpenumbra3_mem1_stage_test* d) {
    d->i_ir = 0; d->i_ea = 0; d->i_phys_dst = 0; d->i_phys_dst_we = 0;
    d->i_fault_pending = 0; d->i_store_data = 0; d->i_valid = 0;
    d->i_hold = 0; d->i_bubble = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_mem1_stage_test* d = new Vpenumbra3_mem1_stage_test;

    clr(d); d->i_rst = 1; tick(d); tick(d); d->i_rst = 0; d->eval();
    check("rst_valid", d->o_mem2_valid, 0);

    // ── Load: launch read translation + cache index, latch register ──
    clr(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 3, 5, 0);   // LDW R3, [R5]
    d->i_ea = 0x0000A000; d->i_phys_dst = 3; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval();
    check("ld_tr_en",   d->o_translate_en, 1);
    check("ld_tr_va",   d->o_translate_vaddr, 0x0000A000);
    check("ld_tr_acc",  d->o_translate_acc_type, ACC_READ);
    check("ld_dc_en",   d->o_dcache_en, 1);
    check("ld_dc_va",   d->o_dcache_vaddr, 0x0000A000);
    tick(d); d->eval();
    check("ld_m2_valid", d->o_mem2_valid, 1);
    check("ld_m2_class", d->o_mem2_op_class, OPC_LOAD);
    check("ld_m2_ea",    d->o_mem2_ea, 0x0000A000);
    check("ld_m2_align", d->o_mem2_align_fault, 0);

    // ── Store: write direction + byte-enable + lane replication ──
    clr(d);
    d->i_ir = enc_m(0, MEM_SZ_WORD, 0, 7, 5, 0);   // STW R7, [R5]
    d->i_ea = 0x0000B000; d->i_store_data = 0xDEADBEEF; d->i_valid = 1;
    d->eval();
    check("st_tr_acc",  d->o_translate_acc_type, ACC_WRITE);
    check("st_dc_en",   d->o_dcache_en, 1);
    tick(d); d->eval();
    check("st_m2_class",   d->o_mem2_op_class, OPC_STORE);
    check("st_m2_byte_en", d->o_mem2_byte_en, 0xF);          // word: all lanes
    check("st_m2_wdata",   d->o_mem2_store_wdata, 0xDEADBEEF);

    // ── Byte store at offset 1: single lane, replicated ──
    clr(d);
    d->i_ir = enc_m(0, MEM_SZ_BYTE, 0, 7, 5, 0);   // STB R7, [R5]
    d->i_ea = 0x0000B001; d->i_store_data = 0x000000A5; d->i_valid = 1;
    d->eval(); tick(d); d->eval();
    check("stb_byte_en", d->o_mem2_byte_en, 0x2);            // offset 1 -> lane 1
    check("stb_wdata",   d->o_mem2_store_wdata, 0xA5A5A5A5); // replicated

    // ── Misaligned word load: no launch, align_fault latched ──
    clr(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 3, 5, 0);
    d->i_ea = 0x0000A002; d->i_phys_dst = 3; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval();
    check("mis_tr_en", d->o_translate_en, 0);               // suppressed
    check("mis_dc_en", d->o_dcache_en, 0);
    tick(d); d->eval();
    check("mis_m2_valid", d->o_mem2_valid, 1);              // still advances
    check("mis_m2_align", d->o_mem2_align_fault, 1);

    // ── RDSYS: sysreg read launch ──
    clr(d);
    d->i_ir = enc_r(24, 4, 0, 0);   // RDSYS R4 (Format-R op 24)
    d->i_valid = 1; d->eval();
    check("rdsys_sys_re", d->o_sys_re, 1);
    check("rdsys_tr_en",  d->o_translate_en, 0);            // not a memory access

    // ── Handshake: hold holds the register, bubble flushes ──
    clr(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 3, 5, 0);
    d->i_ea = 0x0000C000; d->i_valid = 1; d->eval();
    tick(d); d->eval();
    check("hs_valid", d->o_mem2_valid, 1);
    d->i_hold = 1; d->i_valid = 0; d->eval();
    tick(d); d->eval();
    check("hs_held", d->o_mem2_valid, 1);                   // held under stall
    d->i_hold = 0; d->i_bubble = 1; d->eval();
    tick(d); d->eval();
    check("hs_flushed", d->o_mem2_valid, 0);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
