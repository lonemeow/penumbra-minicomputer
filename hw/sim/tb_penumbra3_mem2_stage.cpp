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
// miss committing as a fault, a store write-through, the MEM2->EX forward, and
// a slot held in MEM2 taking effect only as it leaves: behind a parked load,
// under WB back-pressure, and squashed by a fault commit.

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
static uint32_t enc_r(int op, int rd) {
    return ((op & 0x1F) << 25) | ((rd & 0xF) << 21);
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
    check("ldh_no_fwd",     d->o_fwd_valid, 0);          // forwards from MEM2/WB, not MEM2
    tick(d); d->eval();                        // B -> C
    check("ldh_wb_valid",  d->o_wb_valid, 1);
    check("ldh_wb_class",  d->o_wb_op_class, OPC_LOAD);
    check("ldh_wb_value",  d->o_wb_value, 0x12345678);
    check("ldh_wb_dst",    d->o_wb_phys_dst, 3);
    check("ldh_wb_nofault", d->o_wb_fault_pending, 0);

    // ── ALU result: forwarded from MEM2 as it entered ──
    idle(d);
    d->i_ir = enc_r(0 /* ADD */, 4); d->i_ea = 0xCAFE0010;   // the carried result
    d->i_phys_dst = 4; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);
    d->i_valid = 0; d->eval();
    check("alu_fwd_valid",  d->o_fwd_valid, 1);
    check("alu_fwd_result", d->o_fwd_result, 0xCAFE0010);
    check("alu_fwd_dst",    d->o_fwd_dst, 4);
    tick(d);

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

    tick(d);

    // The slot in MEM2 takes effect -- commits, parks, writes the L1 -- in
    // the one cycle it leaves MEM2. The three sequences below hold it there
    // in each of the ways the spine can: behind a parked load, under WB
    // back-pressure, and squashed by a fault commit.

    // ── An ALU op behind a parked load commits once, after the completion ──
    idle(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 5); d->i_ea = 0x1000;
    d->i_phys_dst = 5; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);                        // the load is in MEM2
    d->i_ir = enc_r(0 /* ADD */, 4); d->i_ea = 0x11110000;
    d->i_phys_dst = 4; d->i_dcache_hit = 0;    // the load misses; the ADD is in MEM1
    d->eval(); tick(d);                        // the load parks; the ADD is in MEM2
    d->i_valid = 0; d->eval();
    int alu_commits = 0;
    tick(d); d->eval();
    check("behind_alu_waits_1", d->o_wb_valid, 0);
    alu_commits += d->o_wb_valid && d->o_wb_phys_dst == 4;
    tick(d); d->eval();
    check("behind_alu_waits_2", d->o_wb_valid, 0);
    alu_commits += d->o_wb_valid && d->o_wb_phys_dst == 4;
    d->i_fill_done = 1; d->i_fill_data = 0x0BADCAFE; d->eval(); tick(d);
    d->i_fill_done = 0; d->eval();
    check("behind_alu_load_done", d->o_complete, 1);
    alu_commits += d->o_wb_valid && d->o_wb_phys_dst == 4;
    for (int i = 0; i < 3; i++) {
        tick(d); d->eval();
        alu_commits += d->o_wb_valid && d->o_wb_phys_dst == 4;
    }
    check("behind_alu_commits_once", alu_commits, 1);

    // ── A store behind a parked load writes the L1 once, after the completion ──
    idle(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 5); d->i_ea = 0x1000;
    d->i_phys_dst = 5; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);                        // the load is in MEM2
    d->i_ir = enc_m(0, MEM_SZ_WORD, 0, 7); d->i_ea = 0x1004;
    d->i_store_data = 0x55AA55AA; d->i_phys_dst_we = 0;
    d->i_dcache_hit = 0;                       // the load misses; the store is in MEM1
    d->eval(); tick(d);                        // the load parks; the store is in MEM2
    d->i_valid = 0; d->eval();
    int store_writes = d->o_dcache_we;
    check("behind_store_waits_1", d->o_dcache_we, 0);
    tick(d); d->eval();
    store_writes += d->o_dcache_we;
    check("behind_store_waits_2", d->o_dcache_we, 0);
    d->i_fill_done = 1; d->i_fill_data = 0; d->eval(); tick(d);
    d->i_fill_done = 0; d->eval();
    for (int i = 0; i < 3; i++) {
        store_writes += d->o_dcache_we;
        tick(d); d->eval();
    }
    check("behind_store_writes_once", store_writes, 1);

    // ── A load that misses under WB back-pressure parks when it leaves MEM2 ──
    idle(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 5); d->i_ea = 0x1000;
    d->i_phys_dst = 5; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);                        // the load is in MEM2
    d->i_valid = 0; d->i_dcache_hit = 0; d->i_ext_stall = 1;
    d->eval();
    int fills = d->o_launch_fill, completions = d->o_complete;
    tick(d); d->eval();                        // held: the load is still in MEM2
    check("held_load_not_parked", d->o_load_pending, 0);
    fills += d->o_launch_fill; completions += d->o_complete;
    d->i_ext_stall = 0; d->eval();             // released: the load leaves MEM2
    tick(d); d->eval();
    check("held_load_parks_on_release", d->o_load_pending, 1);
    fills += d->o_launch_fill; completions += d->o_complete;
    d->i_fill_done = 1; d->i_fill_data = 0x600DF00D; d->eval(); tick(d);
    d->i_fill_done = 0; d->eval();
    for (int i = 0; i < 4; i++) {
        fills += d->o_launch_fill; completions += d->o_complete;
        // A second park would wait here for ever: answer it so the count shows it.
        d->i_fill_done = d->o_load_pending; d->eval();
        tick(d); d->i_fill_done = 0; d->eval();
    }
    check("held_load_fills_once",     fills, 1);
    check("held_load_completes_once", completions, 1);

    // ── A load squashed in MEM2 by a fault commit does not park ──
    idle(d);
    d->i_ir = enc_m(1, MEM_SZ_WORD, 0, 5); d->i_ea = 0x1000;
    d->i_phys_dst = 5; d->i_phys_dst_we = 1; d->i_valid = 1;
    d->eval(); tick(d);                        // the load is in MEM2
    d->i_valid = 0; d->i_dcache_hit = 0; d->i_bubble = 1;
    d->eval(); tick(d);
    d->i_bubble = 0; d->eval();
    check("squashed_load_not_parked",  d->o_load_pending, 0);
    check("squashed_load_no_fill",     d->o_launch_fill, 0);
    check("squashed_load_no_commit",   d->o_wb_valid, 0);
    // Answer a park if there is one, so a failure here does not hang the rest.
    d->i_fill_done = d->o_load_pending; d->eval(); tick(d);
    d->i_fill_done = 0; d->eval();
    check("squashed_load_no_complete", d->o_complete, 0);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
