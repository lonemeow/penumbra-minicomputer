// Verilator testbench for penumbra3_mmu.
//
// Exercises the gen3 MMU against the architectural contract (doc/system/mmu.md):
//   - flat/bypass at reset (M=0): identity, uncacheable, no checks (I and D)
//   - install a main-TLB entry, enable M=1, translate a hit (paddr + offset)
//   - a TLB miss and a protection fault (R-only page, write access)
//   - a pinned entry overriding the main TLB (pinned-hit-wins)
//   - the I-side translate and per-fetch force_bypass (vector fetch)
//   - sysreg TLB read-back (RDSYS TLB_VPN / TLB_PTE)
//   - commit-time fault registers (FADDR / FSTAT)
//
// Translate is launch/resolve: a query driven at cycle T resolves at T+1, so
// every query drives its inputs, ticks once, then reads the verdict.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_mmu.h"
#include "verilated.h"

enum { ACC_READ = 1, ACC_WRITE = 2, ACC_EXEC = 4 };
enum { CR = 0, FADDR = 1, FSTAT = 2, TVPN = 3, TPTE = 4, TIDX = 5 };
// PTE flags
enum { F_V = 0x01, F_C = 0x04, F_R = 0x08, F_W = 0x10, F_X = 0x20, F_U = 0x40, F_G = 0x80 };

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

static void tick(Vpenumbra3_mmu* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void clear(Vpenumbra3_mmu* d) {
    d->i_i_lookup_en = 0; d->i_i_vaddr = 0; d->i_i_user_mode = 0;
    d->i_i_force_bypass = 0; d->i_i_hold = 0;
    d->i_d_lookup_en = 0; d->i_d_vaddr = 0; d->i_d_access_type = 0;
    d->i_d_user_mode = 0; d->i_d_hold = 0;
    d->i_fault_commit = 0; d->i_fault_vaddr = 0; d->i_fault_status = 0;
    d->i_sys_reg = 0; d->i_sys_wdata = 0; d->i_sys_we = 0; d->i_sys_re = 0;
}

static void wrsys(Vpenumbra3_mmu* d, int reg, uint32_t val) {
    d->i_sys_we = 1; d->i_sys_reg = reg; d->i_sys_wdata = val; d->eval();
    tick(d);
    d->i_sys_we = 0; d->eval();
}

// Install a main-TLB entry: index, then VPN word, then PTE (commits).
static void install(Vpenumbra3_mmu* d, uint32_t idx, uint32_t vpn_word, uint32_t pte_word) {
    wrsys(d, TIDX, idx);
    wrsys(d, TVPN, vpn_word);
    wrsys(d, TPTE, pte_word);
}

// vpn_word = {VPN[19:0]<<8, ASID}; pte_word = {PPN[19:0]<<12, SW<<8, flags}
static uint32_t vpnw(uint32_t vpn, uint32_t asid) { return (vpn << 8) | (asid & 0xFF); }
static uint32_t ptew(uint32_t ppn, uint32_t flags) { return (ppn << 12) | (flags & 0xFF); }

// RDSYS: launch at T, response at T+1.
static uint32_t rdsys(Vpenumbra3_mmu* d, int reg) {
    d->i_sys_re = 1; d->i_sys_reg = reg; d->eval();
    tick(d);
    d->i_sys_re = 0; d->eval();
    return d->o_sys_rdata;
}

// Drive a D-side query, resolve, return after the tick (verdict valid).
static void query_d(Vpenumbra3_mmu* d, uint32_t va, int acc, int user) {
    d->i_d_lookup_en = 1; d->i_d_vaddr = va; d->i_d_access_type = acc;
    d->i_d_user_mode = user; d->eval();
    tick(d);
    d->i_d_lookup_en = 0;   // verdict is from the registered query; safe to drop
}

static void query_i(Vpenumbra3_mmu* d, uint32_t va, int user, int force_bypass) {
    d->i_i_lookup_en = 1; d->i_i_vaddr = va; d->i_i_user_mode = user;
    d->i_i_force_bypass = force_bypass; d->eval();
    tick(d);
    d->i_i_lookup_en = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_mmu* d = new Vpenumbra3_mmu;

    clear(d);
    d->i_rst = 1; tick(d); tick(d); d->i_rst = 0; d->eval();

    // ── Flat/bypass at reset (M=0): identity, uncacheable ────────
    query_d(d, 0x12345678, ACC_READ, 0);
    check("flat_d_hit",   d->o_d_hit, 1);
    check("flat_d_paddr", d->o_d_paddr, 0x12345678);
    check("flat_d_cache", d->o_d_cacheable, 0);
    check("flat_d_miss",  d->o_d_miss_fault, 0);
    check("flat_d_prot",  d->o_d_prot_fault, 0);
    query_i(d, 0xFFFF0000, 0, 0);
    check("flat_i_paddr", d->o_i_paddr, 0xFFFF0000);
    check("flat_i_fault", d->o_i_fault, 0);

    // ── Install a cacheable RW page, enable the MMU, translate a hit ─
    // VA 0x00010xxx (VPN 0x10, set = vaddr[16:12] = 0x10), PPN 0x20.
    install(d, /*idx=set 0x10, way 0*/ 0x10, vpnw(0x10, 0), ptew(0x20, F_V|F_C|F_R|F_W|F_X));
    wrsys(d, CR, 0x1);                 // M=1, ASID=0
    query_d(d, 0x00010ABC, ACC_READ, 0);
    check("hit_hit",   d->o_d_hit, 1);
    check("hit_miss",  d->o_d_miss_fault, 0);
    check("hit_paddr", d->o_d_paddr, 0x00020ABC);   // PPN<<12 | offset
    check("hit_cache", d->o_d_cacheable, 1);

    // ── TLB miss: an unmapped page ───────────────────────────────
    query_d(d, 0x00099000, ACC_READ, 0);
    check("miss_hit",  d->o_d_hit, 0);
    check("miss_miss", d->o_d_miss_fault, 1);

    // ── Protection fault: R-only page, write access ──────────────
    install(d, 0x11, vpnw(0x11, 0), ptew(0x21, F_V|F_C|F_R));   // no W
    query_d(d, 0x00011000, ACC_WRITE, 0);
    check("prot_hit",  d->o_d_hit, 1);
    check("prot_prot", d->o_d_prot_fault, 1);
    check("prot_miss", d->o_d_miss_fault, 0);
    query_d(d, 0x00011000, ACC_READ, 0);                        // read still OK
    check("prot_read_ok", d->o_d_prot_fault, 0);

    // ── Pinned entry overrides the main TLB ──────────────────────
    // Pinned slot 0 (TLB_INDEX bit 6 set): VPN 0x30 -> PPN 0x77.
    install(d, 0x40, vpnw(0x30, 0), ptew(0x77, F_V|F_C|F_R|F_W));
    query_d(d, 0x00030100, ACC_READ, 0);
    check("pin_hit",   d->o_d_hit, 1);
    check("pin_paddr", d->o_d_paddr, 0x00077100);
    check("pin_cache", d->o_d_cacheable, 1);

    // ── I-side translate + force_bypass ──────────────────────────
    query_i(d, 0x00010000, 0, 0);                  // mapped (VPN 0x10 -> PPN 0x20)
    check("i_hit_paddr", d->o_i_paddr, 0x00020000);
    check("i_hit_fault", d->o_i_fault, 0);
    query_i(d, 0x00010000, 0, 1);                  // vector fetch: physical
    check("i_bypass_paddr", d->o_i_paddr, 0x00010000);
    check("i_bypass_fault", d->o_i_fault, 0);

    // ── Sysreg TLB read-back (main entry at set 0x10, way 0) ─────
    wrsys(d, TIDX, 0x10);
    check("rb_vpn", rdsys(d, TVPN), vpnw(0x10, 0));
    check("rb_pte", rdsys(d, TPTE), ptew(0x20, F_V|F_C|F_R|F_W|F_X));

    // ── Commit-time fault registers ──────────────────────────────
    d->i_fault_commit = 1; d->i_fault_vaddr = 0xCAFE0000; d->i_fault_status = 0x00000A12;
    d->eval(); tick(d); d->i_fault_commit = 0; d->eval();
    check("faddr", rdsys(d, FADDR), 0xCAFE0000);
    check("fstat", rdsys(d, FSTAT), 0x00000A12);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
