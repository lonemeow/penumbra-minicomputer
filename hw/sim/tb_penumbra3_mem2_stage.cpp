// Verilator testbench for penumbra3_mem2_stage, composed with MEM1 + dtranslate
// (via penumbra3_mem2_stage_test). Each access flows MEM1 (launch) -> MEM2
// (resolve), so the launch/verdict alignment is exercised end to end.
//
// Timing: an access driven in cycle A is in MEM1; after one tick it is in MEM2
// (cycle B) with the translation verdict resolved; the testbench drives the
// D-cache resolve in B; after another tick it commits in WB (cycle C).
//
// Covers: load hit (+ sub-word), the miss -> hold -> registered completion
// sequence, an uncached load taking the fill path despite a cache "hit", a TLB
// miss committing as a fault, a store write-through, and the MEM2->EX forward.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_mem2_stage_test.h"
#include "verilated.h"

enum { OPC_LOAD = 1, OPC_STORE = 2 };
enum { MEM_SZ_BYTE = 0, MEM_SZ_WORD = 2 };
enum { VEC_TLB_MISS = 2 };
// TLB PTE flag bits
enum { F_V = 0x01, F_C = 0x04, F_R = 0x08, F_W = 0x10 };

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}
static void tick(Vpenumbra3_mem2_stage_test* d) {
    d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval();
}
static uint32_t enc_m(int L, int sz, int se, int rd) {
    return (2u << 30) | ((L & 1) << 29) | ((sz & 3) << 27) | ((se & 1) << 26)
         | ((rd & 0xF) << 22);
}
static void idle(Vpenumbra3_mem2_stage_test* d) {
    d->i_ir = 0; d->i_ea = 0; d->i_phys_dst = 0; d->i_phys_dst_we = 0;
    d->i_fault_pending = 0; d->i_store_data = 0; d->i_valid = 0;
    d->i_dcache_hit = 0; d->i_dcache_rdata = 0;
    d->i_tlb_wr_en = 0; d->i_fill_done = 0; d->i_fill_data = 0; d->i_fill_fault = 0;
    d->i_ext_stall = 0; d->i_bubble = 0;
}
// Install a TLB entry mapping vaddr's page -> ppn with the given flags (ASID 0).
static void tlb_install(Vpenumbra3_mem2_stage_test* d, uint32_t vaddr, uint32_t ppn, uint32_t flags) {
    d->i_tlb_wr_en = 1;
    d->i_tlb_wr_set = (vaddr >> 12) & 0x1F;
    d->i_tlb_wr_way = 0;
    d->i_tlb_wr_valid = 1;
    d->i_tlb_wr_vpn_word = (vaddr >> 12) << 8;          // VPN<<8 | ASID(0)
    d->i_tlb_wr_pte_word = (ppn << 12) | flags;
    d->eval(); tick(d);
    d->i_tlb_wr_en = 0; d->eval();
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_mem2_stage_test* d = new Vpenumbra3_mem2_stage_test;

    idle(d); d->i_rst = 1; tick(d); tick(d); d->i_rst = 0; d->eval();
    check("rst_valid", d->o_wb_valid, 0);

    // 0x1000 -> paddr 0x2000, cacheable + RW.
    tlb_install(d, 0x1000, 0x2, F_V | F_C | F_R | F_W);
    // 0x3000 -> paddr 0x4000, UNCACHED, R.
    tlb_install(d, 0x3000, 0x4, F_V | F_R);

    // ── Load hit: word, with the MEM2->EX forward ──
    idle(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 3); d->i_ea = 0x1000;
    d->i_phys_dst = 3; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);                       // A -> B
    d->i_valid = 0; d->i_dcache_hit = 1; d->i_dcache_rdata = 0x12345678;
    d->eval();
    check("ldh_no_pending", d->o_load_pending, 0);
    check("ldh_fwd_valid",  d->o_fwd_valid, 1);          // resolved, forwardable in MEM2
    check("ldh_fwd_result", d->o_fwd_result, 0x12345678);
    check("ldh_fwd_dst",    d->o_fwd_dst, 3);
    tick(d); d->eval();                        // B -> C
    check("ldh_wb_valid",  d->o_wb_valid, 1);
    check("ldh_wb_class",  d->o_wb_op_class, OPC_LOAD);
    check("ldh_wb_value",  d->o_wb_value, 0x12345678);
    check("ldh_wb_dst",    d->o_wb_phys_dst, 3);
    check("ldh_wb_nofault", d->o_wb_fault_pending, 0);

    // ── Sub-word load hit: byte at offset 2, zero-extended ──
    idle(d);
    d->i_ir = enc_m(1, MEM_SZ_BYTE, 0, 3); d->i_ea = 0x1002;
    d->i_phys_dst = 3; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);
    d->i_valid = 0; d->i_dcache_hit = 1; d->i_dcache_rdata = 0x12345678;
    d->eval(); tick(d); d->eval();
    check("ldb_wb_value", d->o_wb_value, 0x34);          // byte [23:16]

    // ── Load miss -> hold -> registered completion ──
    idle(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 5); d->i_ea = 0x1000;
    d->i_phys_dst = 5; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);                        // A -> B
    d->i_valid = 0; d->i_dcache_hit = 0;       // MISS in MEM2
    d->eval();
    check("ldm_pending_b", d->o_load_pending, 0);        // registers next cycle
    tick(d); d->eval();                        // B -> C
    check("ldm_pending",   d->o_load_pending, 1);        // pipe frozen off the flop
    check("ldm_launch",    d->o_launch_fill, 1);
    check("ldm_fill_pa",   d->o_fill_paddr, 0x2000);
    check("ldm_wb_evicted", d->o_wb_valid, 0);           // the missing load did not commit
    // Deliver the fill.
    d->i_fill_done = 1; d->i_fill_data = 0xCAFEF00D; d->i_fill_fault = 0;
    d->eval(); tick(d); d->eval();             // completion registers
    check("ldm_complete",      d->o_complete, 1);
    check("ldm_complete_val",  d->o_complete_value, 0xCAFEF00D);
    check("ldm_complete_dest", d->o_complete_dest, 5);
    check("ldm_pending_clr",   d->o_load_pending, 0);
    d->i_fill_done = 0; d->eval();

    // ── Uncached load: takes the fill path even though the cache "hits" ──
    idle(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 6); d->i_ea = 0x3000;   // uncached page
    d->i_phys_dst = 6; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);
    d->i_valid = 0; d->i_dcache_hit = 1;       // cache claims a (stale) hit
    d->eval(); tick(d); d->eval();
    check("unc_pending", d->o_load_pending, 1);          // uncached -> fill, not a hit
    check("unc_fill_pa", d->o_fill_paddr, 0x4000);
    // drain it so the pipe unfreezes
    d->i_fill_done = 1; d->i_fill_data = 0; d->eval(); tick(d); d->i_fill_done = 0; d->eval();

    // ── TLB miss: commits to WB carrying the fault ──
    idle(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 4); d->i_ea = 0x9000;   // no entry
    d->i_phys_dst = 4; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);
    d->i_valid = 0; d->eval(); tick(d); d->eval();
    check("tlbm_wb_valid", d->o_wb_valid, 1);            // a fault commits, not evicted
    check("tlbm_fault",    d->o_wb_fault_pending, 1);
    check("tlbm_vec",      d->o_wb_fault_vec, VEC_TLB_MISS);
    check("tlbm_no_pending", d->o_load_pending, 0);

    // ── Store: write-through in MEM2 ──
    idle(d);
    d->i_ir = enc_m(0, MEM_SZ_WORD, 0, 7); d->i_ea = 0x1000;
    d->i_store_data = 0xAABBCCDD; d->i_valid = 1;
    d->eval(); tick(d);
    d->i_valid = 0; d->eval();
    check("st_we",    d->o_dcache_we, 1);
    check("st_paddr", d->o_dcache_paddr, 0x2000);
    check("st_wdata", d->o_dcache_wdata, 0xAABBCCDD);
    tick(d); d->eval();
    check("st_wb_class", d->o_wb_op_class, OPC_STORE);
    check("st_wb_valid", d->o_wb_valid, 1);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
