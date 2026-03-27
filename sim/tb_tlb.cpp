// Verilator testbench for the Penumbra TLB
//
// Tests the 64-entry 2-way set-associative TLB:
//   - Indexed write and readback
//   - Lookup hit/miss
//   - VPN + ASID matching, G (global) bypass
//   - Permission checks (R/W/X, user/supervisor)
//   - Way selection (way0 priority)
//   - Set indexing from virtual address
//   - Invalidation (V=0)
//   - Protection fault generation with correct FAULT_STATUS

#include <cstdio>
#include <cstdint>
#include "Vtlb.h"

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

// Fault types
enum FaultType {
    FAULT_TLB_MISS = 0x0001,
    FAULT_PROT     = 0x0002,
};

static int errors = 0, tests = 0;

static void tick(Vtlb* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vtlb* d) {
    d->i_rst = 1;
    d->i_lookup_en = 0;
    d->i_write_en = 0;
    d->i_vaddr = 0;
    d->i_access_type = ACC_READ;
    d->i_user_mode = 0;
    d->i_asid = 0;
    d->i_idx_set = 0;
    d->i_idx_way = 0;
    d->i_write_vpn = 0;
    d->i_write_pte = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
}

// Build TLB_VPN word: {4'b0, VPN[19:0], ASID[7:0]}
static uint32_t make_vpn_word(uint32_t vpn, uint8_t asid) {
    return ((vpn & 0xFFFFF) << 8) | asid;
}

// Build TLB_PTE word: {PPN[19:0], SW[3:0], flags[7:0]}
// PPN in bits 31:12, SW in bits 11:8, flags in bits 7:0
static uint32_t make_pte_word(uint32_t ppn, uint8_t sw, uint8_t flags) {
    return ((ppn & 0xFFFFF) << 12) | ((sw & 0xF) << 8) | (flags & 0xFF);
}

// Write a TLB entry via indexed write
static void write_entry(Vtlb* d, int set, int way,
                        uint32_t vpn, uint32_t ppn, uint8_t asid,
                        uint8_t sw, uint8_t flags) {
    d->i_idx_set = set;
    d->i_idx_way = way;
    d->i_write_vpn = make_vpn_word(vpn, asid);
    d->i_write_pte = make_pte_word(ppn, sw, flags);
    d->i_write_en = 1;
    tick(d);
    d->i_write_en = 0;
}

// Read a TLB entry via indexed read
static void read_entry(Vtlb* d, int set, int way,
                       uint32_t* vpn_word, uint32_t* pte_word) {
    d->i_idx_set = set;
    d->i_idx_way = way;
    d->eval();  // Combinational read
    *vpn_word = d->o_read_vpn;
    *pte_word = d->o_read_pte;
}

// Perform a lookup and return results
struct LookupResult {
    uint32_t paddr;
    bool     hit;
    bool     fault;
    uint32_t fault_status;
    bool     cacheable;
};

static LookupResult lookup(Vtlb* d, uint32_t vaddr, uint8_t access_type,
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

static void test_reset_all_invalid(Vtlb* d) {
    printf("── Reset: all entries invalid ──\n");
    reset(d);

    // Check a few entries are invalid (V=0 in PTE word bit 0)
    uint32_t vpn, pte;
    for (int i = 0; i < 64; i++) {
        read_entry(d, i & 0x1F, (i >> 5) & 1, &vpn, &pte);
        check("reset V=0", pte & TLB_V, 0u);
    }
}

static void test_write_readback(Vtlb* d) {
    printf("── Write and readback ──\n");
    reset(d);

    // Write entry to set 5, way 0
    uint32_t vpn = 0xABCDE;  // VPN
    uint32_t ppn = 0x12345;  // PPN
    uint8_t asid = 0x42;
    uint8_t sw = 0xF0;
    uint8_t flags = TLB_V | TLB_R | TLB_W | TLB_X | TLB_U | TLB_C;

    write_entry(d, 5, 0, vpn, ppn, asid, sw, flags);

    uint32_t got_vpn, got_pte;
    read_entry(d, 5, 0, &got_vpn, &got_pte);

    check("readback VPN", got_vpn, make_vpn_word(vpn, asid));
    check("readback PTE", got_pte, make_pte_word(ppn, sw, flags));
}

static void test_basic_hit(Vtlb* d) {
    printf("── Basic lookup hit ──\n");
    reset(d);

    // VPN 0x00005 → set = VPN[4:0] = 5
    uint32_t vpn = 0x00005;
    uint32_t ppn = 0xAAAAA;
    uint8_t flags = TLB_V | TLB_R | TLB_W | TLB_X | TLB_G;

    write_entry(d, 5, 0, vpn, ppn, 0, 0, flags);

    // Lookup: vaddr = VPN << 12 | offset
    uint32_t vaddr = (vpn << 12) | 0x100;
    auto r = lookup(d, vaddr, ACC_READ, false, 0);

    check_bool("hit", r.hit, true);
    check_bool("no fault", r.fault, false);
    check("paddr", r.paddr, (ppn << 12) | 0x100);
}

static void test_miss(Vtlb* d) {
    printf("── Lookup miss ──\n");
    reset(d);

    // No entries loaded — everything should miss
    uint32_t vaddr = 0x12345000;
    auto r = lookup(d, vaddr, ACC_READ, false, 0);

    check_bool("miss", r.hit, false);
}

static void test_way1_hit(Vtlb* d) {
    printf("── Way 1 hit ──\n");
    reset(d);

    uint32_t vpn = 0x00003;  // set = 3
    uint32_t ppn = 0xBBBBB;
    uint8_t flags = TLB_V | TLB_R | TLB_G;

    // Write to way 1, not way 0
    write_entry(d, 3, 1, vpn, ppn, 0, 0, flags);

    uint32_t vaddr = (vpn << 12) | 0x04;
    auto r = lookup(d, vaddr, ACC_READ, false, 0);

    check_bool("hit way1", r.hit, true);
    check("paddr way1", r.paddr, (ppn << 12) | 0x04);
}

static void test_asid_match(Vtlb* d) {
    printf("── ASID matching ──\n");
    reset(d);

    uint32_t vpn = 0x0000A;  // set = 10
    uint32_t ppn = 0x11111;
    uint8_t flags = TLB_V | TLB_R;

    // Entry with ASID=5, NOT global
    write_entry(d, 10, 0, vpn, ppn, 5, 0, flags);

    // Lookup with matching ASID → hit
    auto r1 = lookup(d, vpn << 12, ACC_READ, false, 5);
    check_bool("ASID match hit", r1.hit, true);

    // Lookup with wrong ASID → miss
    auto r2 = lookup(d, vpn << 12, ACC_READ, false, 7);
    check_bool("ASID mismatch miss", r2.hit, false);
}

static void test_global_bypass(Vtlb* d) {
    printf("── Global bit bypasses ASID ──\n");
    reset(d);

    uint32_t vpn = 0x0000B;  // set = 11
    uint32_t ppn = 0x22222;
    uint8_t flags = TLB_V | TLB_R | TLB_G;  // G=1

    // Entry with ASID=5 but G=1
    write_entry(d, 11, 0, vpn, ppn, 5, 0, flags);

    // Lookup with different ASID → should still hit (G=1)
    auto r = lookup(d, vpn << 12, ACC_READ, false, 99);
    check_bool("global hit", r.hit, true);
    check("global paddr", r.paddr, ppn << 12);
}

static void test_permission_read(Vtlb* d) {
    printf("── Permission: read ──\n");
    reset(d);

    uint32_t vpn = 0x00010;  // set = 16
    uint32_t ppn = 0x33333;

    // Entry with R=0 (no read)
    write_entry(d, 16, 0, vpn, ppn, 0, 0, TLB_V | TLB_W | TLB_X | TLB_G);

    auto r = lookup(d, vpn << 12, ACC_READ, false, 0);
    check_bool("read denied hit", r.hit, true);
    check_bool("read denied fault", r.fault, true);
    check("fault type", r.fault_status & 0xF, (uint32_t)FAULT_PROT);
    check("fault R bit", (r.fault_status >> 8) & 0x7, (uint32_t)ACC_READ);
}

static void test_permission_write(Vtlb* d) {
    printf("── Permission: write ──\n");
    reset(d);

    uint32_t vpn = 0x00011;  // set = 17
    uint32_t ppn = 0x44444;

    // Entry with W=0 (dirty tracking pattern)
    write_entry(d, 17, 0, vpn, ppn, 0, 0, TLB_V | TLB_R | TLB_X | TLB_G);

    auto r = lookup(d, vpn << 12, ACC_WRITE, false, 0);
    check_bool("write denied fault", r.fault, true);
    check("fault type", r.fault_status & 0xF, (uint32_t)FAULT_PROT);
    check("fault W bit", (r.fault_status >> 8) & 0x7, (uint32_t)ACC_WRITE);
}

static void test_permission_exec(Vtlb* d) {
    printf("── Permission: execute ──\n");
    reset(d);

    uint32_t vpn = 0x00012;  // set = 18
    uint32_t ppn = 0x55555;

    // Entry with X=0 (no-execute)
    write_entry(d, 18, 0, vpn, ppn, 0, 0, TLB_V | TLB_R | TLB_W | TLB_G);

    auto r = lookup(d, vpn << 12, ACC_EXEC, false, 0);
    check_bool("exec denied fault", r.fault, true);
    check("fault X bit", (r.fault_status >> 8) & 0x7, (uint32_t)ACC_EXEC);
}

static void test_permission_user(Vtlb* d) {
    printf("── Permission: user mode ──\n");
    reset(d);

    uint32_t vpn = 0x00013;  // set = 19
    uint32_t ppn = 0x66666;

    // Entry with U=0 (supervisor only), R=1
    write_entry(d, 19, 0, vpn, ppn, 0, 0, TLB_V | TLB_R | TLB_G);

    // Supervisor read → should pass
    auto r1 = lookup(d, vpn << 12, ACC_READ, false, 0);
    check_bool("supervisor pass", r1.fault, false);

    // User read → should fault (U=0)
    auto r2 = lookup(d, vpn << 12, ACC_READ, true, 0);
    check_bool("user denied fault", r2.fault, true);
    check("user fault USR bit", (r2.fault_status >> 11) & 1, 1u);
}

static void test_cacheable(Vtlb* d) {
    printf("── Cacheable flag ──\n");
    reset(d);

    uint32_t vpn = 0x00014;  // set = 20

    // Cached entry
    write_entry(d, 20, 0, vpn, 0xCC000, 0, 0, TLB_V | TLB_R | TLB_C | TLB_G);
    auto r1 = lookup(d, vpn << 12, ACC_READ, false, 0);
    check_bool("cached", r1.cacheable, true);

    // Uncached entry (different VPN, same set via way 1)
    uint32_t vpn2 = vpn + 32;  // same set (low 5 bits match)
    write_entry(d, 20, 1, vpn2, 0xDD000, 0, 0, TLB_V | TLB_R | TLB_G);
    auto r2 = lookup(d, vpn2 << 12, ACC_READ, false, 0);
    check_bool("uncached", r2.cacheable, false);
}

static void test_invalidate(Vtlb* d) {
    printf("── Invalidate entry ──\n");
    reset(d);

    uint32_t vpn = 0x00015;  // set = 21
    write_entry(d, 21, 0, vpn, 0xEEEEE, 0, 0, TLB_V | TLB_R | TLB_G);

    // Verify hit
    auto r1 = lookup(d, vpn << 12, ACC_READ, false, 0);
    check_bool("before invalidate hit", r1.hit, true);

    // Invalidate by writing V=0
    write_entry(d, 21, 0, 0, 0, 0, 0, 0);  // all zeros including V=0

    // Verify miss
    auto r2 = lookup(d, vpn << 12, ACC_READ, false, 0);
    check_bool("after invalidate miss", r2.hit, false);
}

static void test_page_offset_passthrough(Vtlb* d) {
    printf("── Page offset preserved ──\n");
    reset(d);

    uint32_t vpn = 0x00016;  // set = 22
    uint32_t ppn = 0xFFF00;
    write_entry(d, 22, 0, vpn, ppn, 0, 0, TLB_V | TLB_R | TLB_G);

    // Various offsets within the page
    for (uint32_t off = 0; off < 0x1000; off += 0x100) {
        uint32_t vaddr = (vpn << 12) | off;
        auto r = lookup(d, vaddr, ACC_READ, false, 0);
        char name[64];
        snprintf(name, sizeof(name), "offset 0x%03X", off);
        check(name, r.paddr, (ppn << 12) | off);
    }
}

static void test_sw_bits_preserved(Vtlb* d) {
    printf("── SW bits stored and returned ──\n");
    reset(d);

    uint8_t sw = 0x0A;  // Distinctive 4-bit pattern (1010)
    write_entry(d, 0, 0, 0x00001, 0x99999, 0, sw, TLB_V | TLB_R | TLB_G);

    uint32_t vpn, pte;
    read_entry(d, 0, 0, &vpn, &pte);

    // SW bits are PTE[11:8] (4 bits)
    uint8_t got_sw = (pte >> 8) & 0xF;
    check("SW bits", got_sw, (uint32_t)(sw & 0xF));
}

static void test_lookup_disabled(Vtlb* d) {
    printf("── Lookup disabled defaults ──\n");
    reset(d);

    write_entry(d, 0, 0, 0x00001, 0x11111, 0, 0, TLB_V | TLB_R | TLB_G);

    // Lookup with enable=0 → should not hit
    d->i_vaddr = 0x00001000;
    d->i_access_type = ACC_READ;
    d->i_user_mode = 0;
    d->i_asid = 0;
    d->i_lookup_en = 0;
    d->eval();

    check_bool("disabled no hit", (bool)d->o_hit, false);
    check_bool("disabled no fault", (bool)d->o_fault, false);
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vtlb* d = new Vtlb;

    printf("── TLB Testbench ──\n\n");

    test_reset_all_invalid(d);
    test_write_readback(d);
    test_basic_hit(d);
    test_miss(d);
    test_way1_hit(d);
    test_asid_match(d);
    test_global_bypass(d);
    test_permission_read(d);
    test_permission_write(d);
    test_permission_exec(d);
    test_permission_user(d);
    test_cacheable(d);
    test_invalidate(d);
    test_page_offset_passthrough(d);
    test_sw_bits_preserved(d);
    test_lookup_disabled(d);

    printf("\ntlb: %d/%d tests passed\n", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
