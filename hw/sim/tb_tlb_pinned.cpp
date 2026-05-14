// Verilator testbench for the Penumbra Pinned TLB
//
// Tests the 4-entry fully-associative pinned TLB:
//   - Indexed write and readback via i_idx / i_write_*
//   - Reset clears all entries (V=0 everywhere)
//   - Lookup hit/miss, no-match returns o_hit=0
//   - VPN + ASID matching with G-bit bypass
//   - Priority encoder: lowest-numbered matching entry wins
//   - Permission checks (R/W/X × user/supervisor) match main TLB
//   - Fault status fields on permission violation
//
// Per the tlb_pinned.sv header, the wrapper-side sysreg interface
// (shared TLB_VPN/TLB_PTE/TLB_INDEX with INDEX[6]=1) is tested in
// tb_tlb_unit; this testbench drives the indexed read/write port
// directly.

#include <cstdio>
#include <cstdint>
#include "Vtlb_pinned.h"

// TLB flag bits (must match penumbra_pkg.sv TLB_* constants)
enum TlbFlags {
    TLB_V = 1 << 0,
    TLB_C = 1 << 2,
    TLB_R = 1 << 3,
    TLB_W = 1 << 4,
    TLB_X = 1 << 5,
    TLB_U = 1 << 6,
    TLB_G = 1 << 7,
};

// Access types (one-hot, must match penumbra_pkg.sv)
enum AccType {
    ACC_READ  = 0b001,
    ACC_WRITE = 0b010,
    ACC_EXEC  = 0b100,
};

enum FaultType {
    FAULT_TLB_MISS = 0x0001,
    FAULT_PROT     = 0x0002,
};

static int errors = 0, tests = 0;

static void tick(Vtlb_pinned* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vtlb_pinned* d) {
    d->i_rst = 1;
    d->i_lookup_en = 0;
    d->i_write_en = 0;
    d->i_vaddr = 0;
    d->i_access_type = ACC_READ;
    d->i_user_mode = 0;
    d->i_asid = 0;
    d->i_idx = 0;
    d->i_write_vpn = 0;
    d->i_write_pte = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
}

// Entry format (identical to main TLB):
//   VPN word: {4'b0, VPN[19:0], ASID[7:0]}
//   PTE word: {PPN[19:0], SW[3:0], flags[7:0]}
static uint32_t make_vpn_word(uint32_t vpn, uint8_t asid) {
    return ((vpn & 0xFFFFF) << 8) | asid;
}

static uint32_t make_pte_word(uint32_t ppn, uint8_t sw, uint8_t flags) {
    return ((ppn & 0xFFFFF) << 12) | ((sw & 0xF) << 8) | (flags & 0xFF);
}

static void write_entry(Vtlb_pinned* d, int slot,
                        uint32_t vpn, uint32_t ppn, uint8_t asid,
                        uint8_t sw, uint8_t flags) {
    d->i_idx = slot;
    d->i_write_vpn = make_vpn_word(vpn, asid);
    d->i_write_pte = make_pte_word(ppn, sw, flags);
    d->i_write_en = 1;
    tick(d);
    d->i_write_en = 0;
}

static void read_entry(Vtlb_pinned* d, int slot,
                       uint32_t* vpn_word, uint32_t* pte_word) {
    d->i_idx = slot;
    d->eval();
    *vpn_word = d->o_read_vpn;
    *pte_word = d->o_read_pte;
}

struct LookupResult {
    uint32_t paddr;
    bool     hit;
    bool     fault;
    uint32_t fault_status;
    bool     cacheable;
};

static LookupResult lookup(Vtlb_pinned* d, uint32_t vaddr, uint8_t access_type,
                           bool user_mode, uint8_t asid) {
    d->i_vaddr = vaddr;
    d->i_access_type = access_type;
    d->i_user_mode = user_mode ? 1 : 0;
    d->i_asid = asid;
    d->i_lookup_en = 1;
    d->eval();

    LookupResult r;
    r.paddr = d->o_paddr;
    r.hit = d->o_hit;
    r.fault = d->o_fault;
    r.fault_status = d->o_fault_status;
    r.cacheable = d->o_cacheable;

    d->i_lookup_en = 0;
    return r;
}

static void check(const char* name, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", name, got, exp);
        errors++;
    }
}

static void check_bool(const char* name, bool got, bool exp) {
    tests++;
    if (got != exp) {
        printf("  FAIL [%s]: got %s, expected %s\n", name,
               got ? "true" : "false", exp ? "true" : "false");
        errors++;
    }
}

// ══════════════════════════════════════════════════════════════
// Test cases
// ══════════════════════════════════════════════════════════════

static void test_reset_all_invalid(Vtlb_pinned* d) {
    printf("── Reset: all 4 slots invalid ──\n");
    reset(d);

    uint32_t vpn, pte;
    for (int i = 0; i < 4; i++) {
        read_entry(d, i, &vpn, &pte);
        check("reset V=0", pte & TLB_V, 0u);
    }

    // Lookup on un-populated TLB should always miss
    LookupResult r = lookup(d, 0xABCD1000, ACC_READ, false, 0);
    check_bool("reset.no_hit", r.hit, false);
    check_bool("reset.no_fault", r.fault, false);
}

static void test_write_readback(Vtlb_pinned* d) {
    printf("── Write and readback ──\n");
    reset(d);

    uint32_t vpn = 0xABCDE;
    uint32_t ppn = 0x12345;
    uint8_t asid = 0x42;
    uint8_t sw = 0xF;
    uint8_t flags = TLB_V | TLB_R | TLB_W | TLB_X | TLB_U | TLB_C;

    write_entry(d, 2, vpn, ppn, asid, sw, flags);

    uint32_t vpn_word, pte_word;
    read_entry(d, 2, &vpn_word, &pte_word);
    check("readback.vpn", vpn_word, make_vpn_word(vpn, asid));
    check("readback.pte", pte_word, make_pte_word(ppn, sw, flags));

    // Other slots untouched
    for (int s = 0; s < 4; s++) {
        if (s == 2) continue;
        read_entry(d, s, &vpn_word, &pte_word);
        check("other_slot.V=0", pte_word & TLB_V, 0u);
    }
}

static void test_lookup_hit(Vtlb_pinned* d) {
    printf("── Lookup: VPN+ASID match → hit ──\n");
    reset(d);

    write_entry(d, 0, 0x12345, 0xABCDE, 0x42, 0,
                TLB_V | TLB_R | TLB_W | TLB_X | TLB_U | TLB_C);

    // Matching VPN+ASID
    LookupResult r = lookup(d, 0x12345'080, ACC_READ, false, 0x42);
    check_bool("hit.o_hit", r.hit, true);
    check("hit.paddr", r.paddr, 0xABCDE080u);
    check_bool("hit.cacheable", r.cacheable, true);
    check_bool("hit.no_fault", r.fault, false);

    // Wrong ASID, G=0 → no hit
    LookupResult r_wrong_asid = lookup(d, 0x12345'080, ACC_READ, false, 0x43);
    check_bool("wrong_asid.no_hit", r_wrong_asid.hit, false);

    // Wrong VPN → no hit
    LookupResult r_wrong_vpn = lookup(d, 0x12346'080, ACC_READ, false, 0x42);
    check_bool("wrong_vpn.no_hit", r_wrong_vpn.hit, false);
}

static void test_global_bit_bypasses_asid(Vtlb_pinned* d) {
    printf("── G=1: ASID bypass ──\n");
    reset(d);

    // Write entry with G=1 and ASID=0x42
    write_entry(d, 1, 0xFEEDF, 0x10000, 0x42, 0,
                TLB_V | TLB_R | TLB_X | TLB_G);

    // Lookup with a different ASID should still hit because G=1
    LookupResult r = lookup(d, 0xFEEDF'000, ACC_READ, false, 0x99);
    check_bool("G.hit_with_wrong_asid", r.hit, true);
    check("G.paddr", r.paddr, 0x10000000u);
}

static void test_priority_lowest_index_wins(Vtlb_pinned* d) {
    printf("── Priority: lowest-numbered match wins ──\n");
    reset(d);

    // Two slots with the SAME VPN and ASID, but different PPN.
    // Per the header, the priority encoder returns the lowest-indexed
    // matching entry.  Slot 1 should take priority over slot 3.
    write_entry(d, 3, 0x55555, 0x33333, 0x10, 0,
                TLB_V | TLB_R | TLB_X);
    write_entry(d, 1, 0x55555, 0x11111, 0x10, 0,
                TLB_V | TLB_R | TLB_X);

    LookupResult r = lookup(d, 0x55555'000, ACC_READ, false, 0x10);
    check_bool("priority.hit", r.hit, true);
    // Slot 1 (PPN=0x11111) wins over slot 3 (PPN=0x33333)
    check("priority.paddr_from_slot1", r.paddr, 0x11111000u);
}

static void test_permission_supervisor_only(Vtlb_pinned* d) {
    printf("── Permission: supervisor-only (U=0) ──\n");
    reset(d);

    // Supervisor-only mapping (U=0)
    write_entry(d, 0, 0x10000, 0x20000, 0, 0,
                TLB_V | TLB_R | TLB_W | TLB_X);

    // Supervisor access: OK
    LookupResult r_sup = lookup(d, 0x10000'010, ACC_READ, false, 0);
    check_bool("sup_only.sup_hit", r_sup.hit, true);
    check_bool("sup_only.sup_no_fault", r_sup.fault, false);

    // User access: hit-but-fault
    LookupResult r_user = lookup(d, 0x10000'010, ACC_READ, true, 0);
    check_bool("sup_only.user_hit", r_user.hit, true);
    check_bool("sup_only.user_fault", r_user.fault, true);
    // Fault class lives in fault_status[3:0]; bits 11/10:8 are
    // user_mode/access_type stamped on by the RTL for the trap handler.
    check("sup_only.user_fault_class",
          r_user.fault_status & 0xF, (uint32_t)FAULT_PROT);
}

static void test_permission_rwx(Vtlb_pinned* d) {
    printf("── Permission: R/W/X bit checks ──\n");
    reset(d);

    // Read-only mapping (R=1, W=0, X=0)
    write_entry(d, 0, 0x20000, 0x30000, 0, 0,
                TLB_V | TLB_R | TLB_U);

    LookupResult r_read = lookup(d, 0x20000'000, ACC_READ, true, 0);
    check_bool("ro.read_ok", r_read.hit && !r_read.fault, true);

    LookupResult r_write = lookup(d, 0x20000'000, ACC_WRITE, true, 0);
    check_bool("ro.write_hit", r_write.hit, true);
    check_bool("ro.write_fault", r_write.fault, true);
    check("ro.write_fault_class",
          r_write.fault_status & 0xF, (uint32_t)FAULT_PROT);

    LookupResult r_exec = lookup(d, 0x20000'000, ACC_EXEC, true, 0);
    check_bool("ro.exec_hit", r_exec.hit, true);
    check_bool("ro.exec_fault", r_exec.fault, true);
}

static void test_invalidation(Vtlb_pinned* d) {
    printf("── Invalidation: V=0 clears slot ──\n");
    reset(d);

    write_entry(d, 2, 0xCAFEC, 0x55555, 0x05, 0,
                TLB_V | TLB_R | TLB_X);

    LookupResult r1 = lookup(d, 0xCAFEC'000, ACC_READ, false, 0x05);
    check_bool("inv.before", r1.hit, true);

    // Invalidate by writing V=0
    write_entry(d, 2, 0xCAFEC, 0x55555, 0x05, 0, 0);

    LookupResult r2 = lookup(d, 0xCAFEC'000, ACC_READ, false, 0x05);
    check_bool("inv.after", r2.hit, false);
}

static void test_lookup_en_gates_output(Vtlb_pinned* d) {
    printf("── i_lookup_en gates lookup outputs ──\n");
    reset(d);

    write_entry(d, 0, 0x33333, 0x44444, 0, 0,
                TLB_V | TLB_R | TLB_X);

    // With lookup_en=0, no outputs should assert
    d->i_vaddr = 0x33333'000;
    d->i_access_type = ACC_READ;
    d->i_user_mode = 0;
    d->i_asid = 0;
    d->i_lookup_en = 0;
    d->eval();

    check_bool("lookup_en=0.o_hit", d->o_hit, false);
    check_bool("lookup_en=0.o_fault", d->o_fault, false);
}

static void test_fill_all_four_slots(Vtlb_pinned* d) {
    printf("── Fill all 4 slots, each translates independently ──\n");
    reset(d);

    // Use uint32_t arithmetic throughout — (0xA0000 + i) * 4096 with
    // int operands overflows signed int (UB).
    for (int i = 0; i < 4; i++) {
        uint32_t vpn = 0xA0000u + i;
        uint32_t ppn = 0xB0000u + i;
        write_entry(d, i, vpn, ppn, 0, 0,
                    TLB_V | TLB_R | TLB_X);
    }

    for (int i = 0; i < 4; i++) {
        uint32_t vpn = 0xA0000u + i;
        uint32_t ppn = 0xB0000u + i;
        uint32_t vaddr = (vpn << 12) | 0x100;
        uint32_t expected_paddr = (ppn << 12) | 0x100;
        LookupResult r = lookup(d, vaddr, ACC_READ, false, 0);
        char name[64];
        snprintf(name, sizeof(name), "fill_all.slot%d.hit", i);
        check_bool(name, r.hit, true);
        snprintf(name, sizeof(name), "fill_all.slot%d.paddr", i);
        check(name, r.paddr, expected_paddr);
    }
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vtlb_pinned* d = new Vtlb_pinned;

    printf("── Pinned TLB Unit Tests ──\n\n");

    test_reset_all_invalid(d);
    test_write_readback(d);
    test_lookup_hit(d);
    test_global_bit_bypasses_asid(d);
    test_priority_lowest_index_wins(d);
    test_permission_supervisor_only(d);
    test_permission_rwx(d);
    test_invalidation(d);
    test_lookup_en_gates_output(d);
    test_fill_all_four_slots(d);

    printf("\ntlb_pinned: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return errors ? 1 : 0;
}
