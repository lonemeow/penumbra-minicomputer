// Verilator testbench for penumbra3_mmu.
//
// Exercises the gen3 MMU against the architectural contract
// (doc/system/mmu.md), MMU_CONFIG version 2 at the module's default geometry
// (256 sets of 2 ways, 8 pinned slots):
//   - no fetch hold out of reset, and MMU_CONFIG
//   - software's initialization of a TLB undefined after reset
//   - flat/bypass at reset (M=0): identity, uncacheable, no checks (I and D)
//   - install a main-TLB entry, enable M=1, translate a hit (paddr + offset)
//   - a TLB miss and a protection fault (R-only page, write access)
//   - a pinned entry overriding the main TLB (pinned-hit-wins)
//   - the I-side translate
//   - sysreg TLB read-back (RDSYS TLB_VPN / TLB_PTE)
//   - commit-time fault registers (FADDR / FSTAT)
//   - TLB_INDEX: way 1's slots, the pinned select in bit 31, slots past
//     either TLB's end selecting nothing
//   - TLB_INVAL_VA, TLB_INVAL_ASID and TLB_INVAL_ALL: what each invalidates
//     and keeps, how long each holds fetch, and the state it leaves alone
//
// Translate is launch/resolve: a query driven at cycle T resolves at T+1, so
// every query drives its inputs, ticks once, then reads the verdict.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_mmu.h"
#include "verilated.h"

enum { ACC_READ = 1, ACC_WRITE = 2, ACC_EXEC = 4 };
enum { CR = 0, FADDR = 1, FSTAT = 2, TVPN = 3, TPTE = 4, TIDX = 5,
       INVAL_VA = 6, INVAL_ASID = 7, INVAL_ALL = 8, CONFIG = 15 };
enum { SETS = 256, PINNED = 0x80000000u };
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
    d->i_i_hold = 0;
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

// Cycles until the MMU stops holding fetch.
static int wait_idle(Vpenumbra3_mmu* d) {
    int n = 0;
    while (d->o_sys_busy && n < 100000) { tick(d); n++; }
    return n;
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

// An invalidation register's WRSYS; returns the cycles fetch is held.
static int inval(Vpenumbra3_mmu* d, int reg, uint32_t val) {
    wrsys(d, reg, val);
    return wait_idle(d);
}

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

static void query_i(Vpenumbra3_mmu* d, uint32_t va, int user) {
    d->i_i_lookup_en = 1; d->i_i_vaddr = va; d->i_i_user_mode = user; d->eval();
    tick(d);
    d->i_i_lookup_en = 0;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vpenumbra3_mmu* d = new Vpenumbra3_mmu;

    clear(d);
    d->i_rst = 1; tick(d); tick(d); d->i_rst = 0; d->eval();

    // ── Out of reset: fetch free, the TLB undefined ──────────────
    check("reset_busy", d->o_sys_busy, 0);
    // Version 2, 8 pinned slots, 256x2, 4 KB pages, an 8-bit ASID
    check("config",     rdsys(d, CONFIG), 0x10C12082);
    // The TLB may hold anything out of reset. Stand in for that with a valid
    // global entry in every slot, main and pinned, each mapping a page a
    // later check expects to miss or to map elsewhere -- VPN 0x99, the miss
    // check's, in every pinned slot, where a hit would win over everything.
    for (uint32_t slot = 0; slot < 2 * SETS; slot++)
        install(d, slot, vpnw(slot % SETS, 0), ptew(0xBAD00 | slot, F_V|F_R|F_W|F_X|F_G));
    for (uint32_t n = 0; n < 8; n++)
        install(d, PINNED + n, vpnw(0x99, 0), ptew(0xBAD, F_V|F_R|F_W|F_X|F_G));
    // As software must before it sets M: invalidate the main TLB, clear
    // every pinned slot.
    check("init_hold", inval(d, INVAL_ALL, 0), 2 * SETS);
    for (uint32_t n = 0; n < 8; n++) {
        wrsys(d, TIDX, PINNED + n);
        wrsys(d, TPTE, 0);
    }

    // ── Flat/bypass at reset (M=0): identity, uncacheable ────────
    query_d(d, 0x12345678, ACC_READ, 0);
    check("flat_d_hit",   d->o_d_hit, 1);
    check("flat_d_paddr", d->o_d_paddr, 0x12345678);
    check("flat_d_cache", d->o_d_cacheable, 0);
    check("flat_d_miss",  d->o_d_miss_fault, 0);
    check("flat_d_prot",  d->o_d_prot_fault, 0);
    query_i(d, 0xFFFF0000, 0);
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
    // Pinned slot 0 (TLB_INDEX bit 31 set): VPN 0x30 -> PPN 0x77.
    install(d, PINNED, vpnw(0x30, 0), ptew(0x77, F_V|F_C|F_R|F_W));
    query_d(d, 0x00030100, ACC_READ, 0);
    check("pin_hit",   d->o_d_hit, 1);
    check("pin_paddr", d->o_d_paddr, 0x00077100);
    check("pin_cache", d->o_d_cacheable, 1);

    // ── I-side translate ─────────────────────────────────────────
    query_i(d, 0x00010000, 0);                     // mapped (VPN 0x10 -> PPN 0x20)
    check("i_hit_paddr", d->o_i_paddr, 0x00020000);
    check("i_hit_fault", d->o_i_fault, 0);

    // ── Sysreg TLB read-back (main entry at set 0x10, way 0) ─────
    wrsys(d, TIDX, 0x10);
    check("rb_vpn", rdsys(d, TVPN), vpnw(0x10, 0));
    check("rb_pte", rdsys(d, TPTE), ptew(0x20, F_V|F_C|F_R|F_W|F_X));

    // ── Commit-time fault registers ──────────────────────────────
    d->i_fault_commit = 1; d->i_fault_vaddr = 0xCAFE0000; d->i_fault_status = 0x00000A12;
    d->eval(); tick(d); d->i_fault_commit = 0; d->eval();
    check("faddr", rdsys(d, FADDR), 0xCAFE0000);
    check("fstat", rdsys(d, FSTAT), 0x00000A12);

    // ── TLB_INDEX: the register, way 1, slots that do not exist ──
    wrsys(d, TIDX, 0x80000002);
    check("idx_readback", rdsys(d, TIDX), 0x80000002);
    // Way 1 of set 0x23 is slot 0x123: VA 0x01023000 (VPN 0x1023, set 0x23).
    install(d, SETS + 0x23, vpnw(0x1023, 0), ptew(0x44, F_V|F_C|F_R));
    check("way1_rb_pte", rdsys(d, TPTE), ptew(0x44, F_V|F_C|F_R));
    query_d(d, 0x01023010, ACC_READ, 0);
    check("way1_paddr", d->o_d_paddr, 0x00044010);
    // Slot 512 is past the main TLB: the write lands nowhere, reads are zero,
    // and slot 0 (what its low bits name) keeps its own entry.
    install(d, 0x10, vpnw(0x10, 0), ptew(0x20, F_V|F_C|F_R|F_W|F_X));
    install(d, 2 * SETS + 0x10, vpnw(0x210, 0), ptew(0x99, F_V|F_R));
    check("past_main_vpn", rdsys(d, TVPN), 0);
    check("past_main_pte", rdsys(d, TPTE), 0);
    wrsys(d, TIDX, 0x10);
    check("past_main_kept", rdsys(d, TPTE), ptew(0x20, F_V|F_C|F_R|F_W|F_X));
    // Pinned slot 8 is past the pinned TLB: pinned slot 0 keeps its entry.
    install(d, PINNED + 8, vpnw(0x31, 0), ptew(0x78, F_V|F_R));
    check("past_pin_pte", rdsys(d, TPTE), 0);
    wrsys(d, TIDX, PINNED);
    check("past_pin_kept", rdsys(d, TPTE), ptew(0x77, F_V|F_C|F_R|F_W));

    // ── TLB_INVAL_VA: the VPN's set, valid ways that hit ─────────
    //   e0  set 0x40 way 0, VPN 0x140 ASID 3   -- named: goes
    //   e1  set 0x40 way 1, VPN 0x240 ASID 3   -- same set: stays
    //   e2  set 0x41 way 0, VPN 0x141 ASID 4   -- named under ASID 3: stays
    //   e3  set 0x42 way 0, VPN 0x142 global   -- named under ASID 7: goes
    //   e4  set 0x43 way 1, VPN 0x343 ASID 3,
    //       way 0 an invalidated slot carrying the same VPN word -- stays
    install(d, 0x040,        vpnw(0x140, 3), ptew(0x140, F_V|F_R));
    install(d, SETS + 0x040, vpnw(0x240, 3), ptew(0x240, F_V|F_R));
    install(d, 0x041,        vpnw(0x141, 4), ptew(0x141, F_V|F_R));
    install(d, 0x042,        vpnw(0x142, 0), ptew(0x142, F_V|F_R|F_G));
    install(d, 0x043,        vpnw(0x343, 3), ptew(0x343, F_R));
    install(d, SETS + 0x043, vpnw(0x343, 3), ptew(0x343, F_V|F_R));
    wrsys(d, TIDX, 0x77);
    wrsys(d, TVPN, 0x0ABCDE05);
    check("va_hold",  inval(d, INVAL_VA, vpnw(0x140, 3)), 2);   // read, then write back
    check("va_hold_g", inval(d, INVAL_VA, vpnw(0x142, 7)), 2);
    inval(d, INVAL_VA, vpnw(0x141, 3));
    inval(d, INVAL_VA, vpnw(0x343, 3));
    wrsys(d, TIDX, 0x040);
    check("va_e0_pte", rdsys(d, TPTE), ptew(0x140, F_R));   // V cleared, the rest kept
    wrsys(d, TIDX, SETS + 0x040);
    check("va_e1_pte", rdsys(d, TPTE), ptew(0x240, F_V|F_R));
    wrsys(d, TIDX, 0x041);
    check("va_e2_pte", rdsys(d, TPTE), ptew(0x141, F_V|F_R));
    wrsys(d, TIDX, 0x042);
    check("va_e3_pte", rdsys(d, TPTE), ptew(0x142, F_R|F_G));
    wrsys(d, TIDX, SETS + 0x043);
    check("va_e4_pte", rdsys(d, TPTE), ptew(0x343, F_R));          // the live way hit
    wrsys(d, TIDX, 0x043);
    check("va_e4_dead", rdsys(d, TPTE), ptew(0x343, F_R));
    wrsys(d, CR, (3 << 8) | 0x1);                                  // ASID 3
    query_d(d, 0x00140000, ACC_READ, 0);
    check("va_e0_miss", d->o_d_miss_fault, 1);
    query_d(d, 0x00240000, ACC_READ, 0);
    check("va_e1_hit",  d->o_d_hit, 1);
    wrsys(d, CR, 0x1);

    // ── TLB_INVAL_ASID: valid, non-global, that ASID ─────────────
    //   f0  ASID 3          -- goes
    //   f1  ASID 4          -- stays
    //   f2  ASID 3, global  -- stays
    install(d, 0x050,        vpnw(0x150, 3), ptew(0x150, F_V|F_R));
    install(d, SETS + 0x050, vpnw(0x250, 4), ptew(0x250, F_V|F_R));
    install(d, 0x0F1,        vpnw(0x1F1, 3), ptew(0x1F1, F_V|F_R|F_G));
    check("asid_hold", inval(d, INVAL_ASID, 3), 2 * SETS);
    wrsys(d, TIDX, 0x050);
    check("asid_f0_pte", rdsys(d, TPTE), ptew(0x150, F_R));
    wrsys(d, TIDX, SETS + 0x050);
    check("asid_f1_pte", rdsys(d, TPTE), ptew(0x250, F_V|F_R));
    wrsys(d, TIDX, 0x0F1);
    check("asid_f2_pte", rdsys(d, TPTE), ptew(0x1F1, F_V|F_R|F_G));
    wrsys(d, TIDX, SETS + 0x040);
    check("asid_e1_pte", rdsys(d, TPTE), ptew(0x240, F_R));        // e1 was ASID 3 too

    // ── TLB_INVAL_ALL: every main entry; the pinned TLB stays ────
    wrsys(d, TIDX, 0x55);
    wrsys(d, TVPN, 0x01234505);
    check("all_hold", inval(d, INVAL_ALL, 0), 2 * SETS);
    check("all_idx_kept", rdsys(d, TIDX), 0x55);
    wrsys(d, TPTE, ptew(0x55, F_V|F_R));   // commits the staged word
    check("all_vpn_kept", rdsys(d, TVPN), 0x01234505);
    wrsys(d, TIDX, SETS + 0x050);
    check("all_f1_pte", rdsys(d, TPTE), ptew(0x250, F_R));
    wrsys(d, TIDX, SETS + 0x23);
    check("all_way1_pte", rdsys(d, TPTE), ptew(0x44, F_C|F_R));
    query_d(d, 0x00010ABC, ACC_READ, 0);
    check("all_main_miss", d->o_d_miss_fault, 1);
    query_d(d, 0x00030100, ACC_READ, 0);
    check("all_pinned_hit", d->o_d_hit, 1);

    // The invalidation registers read zero.
    check("inval_va_reads_0",   rdsys(d, INVAL_VA), 0);
    check("inval_asid_reads_0", rdsys(d, INVAL_ASID), 0);
    check("inval_all_reads_0",  rdsys(d, INVAL_ALL), 0);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
