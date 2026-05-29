// Verilator testbench for the Penumbra TLB Unit
//
// Tests the composition of main TLB + pinned TLB behind a single
// sysreg interface (per tlb_unit.sv header):
//
//   Sysreg routing:
//     SYSREG_MMU_TLB_VPN (3) — shared VPN staging (drives both banks)
//     SYSREG_MMU_TLB_PTE (4) — write commits to bank selected by IDX[6]
//     SYSREG_MMU_TLB_IDX (5) — IDX[6]=1 → pinned, IDX[6]=0 → main
//       (main: {way=IDX[5], set=IDX[4:0]}; pinned: slot=IDX[clog2(N)-1:0])
//
//   Lookup composition:
//     - Both banks run in parallel.
//     - Pinned hit takes priority over main TLB hit.
//     - Faults come from whichever bank hit.
//
// The two underlying banks have their own dedicated unit tests
// (tb_tlb, tb_tlb_pinned).  This testbench focuses on the *composition*
// behaviors: sysreg routing, shared staging, priority-on-lookup.

#include <cstdio>
#include <cstdint>
#include "Vtlb_unit.h"

// Sysreg numbers (from penumbra_pkg.sv)
enum SysReg {
    SYSREG_MMU_TLB_VPN = 3,
    SYSREG_MMU_TLB_PTE = 4,
    SYSREG_MMU_TLB_IDX = 5,
};

// Pinned-bank slot count — must match tlb_unit.sv PINNED_SLOTS.
constexpr int PINNED_SLOTS = 8;

enum TlbFlags {
    TLB_V = 1 << 0,
    TLB_C = 1 << 2,
    TLB_R = 1 << 3,
    TLB_W = 1 << 4,
    TLB_X = 1 << 5,
    TLB_U = 1 << 6,
    TLB_G = 1 << 7,
};

enum AccType {
    ACC_READ  = 0b001,
    ACC_WRITE = 0b010,
    ACC_EXEC  = 0b100,
};

static int errors = 0, tests = 0;

static void tick(Vtlb_unit* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vtlb_unit* d) {
    d->i_rst = 1;
    d->i_lookup_en = 0;
    d->i_vaddr = 0;
    d->i_access_type = ACC_READ;
    d->i_user_mode = 0;
    d->i_asid = 0;
    d->i_sys_reg = 0;
    d->i_sys_wdata = 0;
    d->i_sys_we = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
}

static uint32_t make_vpn_word(uint32_t vpn, uint8_t asid) {
    return ((vpn & 0xFFFFFu) << 8) | asid;
}

static uint32_t make_pte_word(uint32_t ppn, uint8_t sw, uint8_t flags) {
    return ((ppn & 0xFFFFFu) << 12) | ((sw & 0xFu) << 8) | (flags & 0xFFu);
}

// Drive a single-cycle sysreg write.  Must hold combinational state
// during the tick so the write commits to the bank selected by IDX[6].
static void sys_write(Vtlb_unit* d, uint32_t reg, uint32_t data) {
    d->i_sys_reg = reg;
    d->i_sys_wdata = data;
    d->i_sys_we = 1;
    tick(d);
    d->i_sys_we = 0;
}

// Combinational sysreg read
static uint32_t sys_read(Vtlb_unit* d, uint32_t reg) {
    d->i_sys_reg = reg;
    d->eval();
    return d->o_sys_rdata;
}

// Write a TLB entry through the unified sysreg interface.
// idx_full is the full IDX value (bit 6 selects pinned vs main).
static void write_entry_via_sysreg(Vtlb_unit* d, uint32_t idx_full,
                                   uint32_t vpn, uint32_t ppn, uint8_t asid,
                                   uint8_t sw, uint8_t flags) {
    // Stage VPN and IDX (any order — they're independent staging regs).
    sys_write(d, SYSREG_MMU_TLB_IDX, idx_full);
    sys_write(d, SYSREG_MMU_TLB_VPN, make_vpn_word(vpn, asid));
    // PTE write commits the entry to the bank selected by IDX[6].
    sys_write(d, SYSREG_MMU_TLB_PTE, make_pte_word(ppn, sw, flags));
}

struct LookupResult {
    uint32_t paddr;
    bool     hit;
    bool     fault;
    uint32_t fault_status;
    bool     cacheable;
};

static LookupResult lookup(Vtlb_unit* d, uint32_t vaddr, uint8_t access_type,
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
// Tests
// ══════════════════════════════════════════════════════════════

static void test_idx_routing_main(Vtlb_unit* d) {
    printf("── IDX[6]=0 → write routes to main TLB ──\n");
    reset(d);

    // Stage IDX with bit 6 = 0 (main), way 0, set 5
    uint32_t idx_main = (0u << 6) | (0u << 5) | 5u;  // 0x05
    write_entry_via_sysreg(d, idx_main, 0x12345, 0xABCDE, 0x01, 0,
                           TLB_V | TLB_R | TLB_W | TLB_X | TLB_C);

    // Readback via same IDX should match
    sys_write(d, SYSREG_MMU_TLB_IDX, idx_main);
    uint32_t vpn = sys_read(d, SYSREG_MMU_TLB_VPN);
    uint32_t pte = sys_read(d, SYSREG_MMU_TLB_PTE);
    check("main.vpn_readback", vpn, make_vpn_word(0x12345, 0x01));
    check("main.pte_readback", pte, make_pte_word(0xABCDE, 0,
            TLB_V | TLB_R | TLB_W | TLB_X | TLB_C));

    // Readback with IDX[6]=1 (pinned slot 0) should return the
    // pinned-bank value at slot 0, which is still V=0 (cleared by reset).
    sys_write(d, SYSREG_MMU_TLB_IDX, 0x40);  // bit6=1, slot=0
    uint32_t pin_pte = sys_read(d, SYSREG_MMU_TLB_PTE);
    check("main.pinned_separate", pin_pte & TLB_V, 0u);
}

static void test_idx_routing_pinned(Vtlb_unit* d) {
    printf("── IDX[6]=1 → write routes to pinned TLB ──\n");
    reset(d);

    // Pinned slot 2: IDX = 0x40 | 2 = 0x42
    uint32_t idx_pin = (1u << 6) | 2u;
    write_entry_via_sysreg(d, idx_pin, 0xDEADB, 0xBEEF0, 0x03, 0,
                           TLB_V | TLB_R | TLB_X);

    // Readback via same IDX
    sys_write(d, SYSREG_MMU_TLB_IDX, idx_pin);
    uint32_t vpn = sys_read(d, SYSREG_MMU_TLB_VPN);
    uint32_t pte = sys_read(d, SYSREG_MMU_TLB_PTE);
    check("pin.vpn_readback", vpn, make_vpn_word(0xDEADB, 0x03));
    check("pin.pte_readback", pte, make_pte_word(0xBEEF0, 0,
            TLB_V | TLB_R | TLB_X));

    // Main TLB at "set 2, way 0" (IDX=0x02) should still be empty —
    // the write should have gone exclusively to pinned slot 2.
    sys_write(d, SYSREG_MMU_TLB_IDX, 2);  // bit6=0, way=0, set=2
    uint32_t main_pte = sys_read(d, SYSREG_MMU_TLB_PTE);
    check("pin.main_unaffected", main_pte & TLB_V, 0u);
}

static void test_shared_vpn_staging(Vtlb_unit* d) {
    printf("── TLB_VPN is shared staging across both banks ──\n");
    reset(d);

    // Stage a VPN once, then commit it to main TLB.
    sys_write(d, SYSREG_MMU_TLB_VPN, make_vpn_word(0x77777, 0x10));

    // Commit to main slot
    sys_write(d, SYSREG_MMU_TLB_IDX, 1);  // main, set 1, way 0
    sys_write(d, SYSREG_MMU_TLB_PTE, make_pte_word(0x11111, 0,
                                                    TLB_V | TLB_R | TLB_X));

    // Commit the same staged VPN to a pinned slot via just an IDX
    // change + PTE write (no second VPN write needed).
    sys_write(d, SYSREG_MMU_TLB_IDX, 0x40 | 1);  // pinned slot 1
    sys_write(d, SYSREG_MMU_TLB_PTE, make_pte_word(0x22222, 0,
                                                    TLB_V | TLB_R | TLB_X));

    // Read main entry — VPN should be 0x77777, PPN 0x11111
    sys_write(d, SYSREG_MMU_TLB_IDX, 1);
    check("shared_vpn.main_vpn", sys_read(d, SYSREG_MMU_TLB_VPN),
          make_vpn_word(0x77777, 0x10));
    check("shared_vpn.main_ppn",
          (sys_read(d, SYSREG_MMU_TLB_PTE) >> 12) & 0xFFFFF,
          0x11111u);

    // Read pinned entry — VPN should also be 0x77777, PPN 0x22222
    sys_write(d, SYSREG_MMU_TLB_IDX, 0x40 | 1);
    check("shared_vpn.pin_vpn", sys_read(d, SYSREG_MMU_TLB_VPN),
          make_vpn_word(0x77777, 0x10));
    check("shared_vpn.pin_ppn",
          (sys_read(d, SYSREG_MMU_TLB_PTE) >> 12) & 0xFFFFF,
          0x22222u);
}

static void test_lookup_main_only(Vtlb_unit* d) {
    printf("── Lookup: main TLB hit when pinned misses ──\n");
    reset(d);

    // Main TLB is set-associative; lookup_set = vaddr[16:12] = low
    // 5 bits of VPN.  The write IDX's set bits must match, or the
    // entry lands in a set the lookup never reaches.
    uint32_t vpn = 0x12345;
    uint32_t main_idx = vpn & 0x1Fu;  // bit6=0, way=0, set=vpn[4:0]
    write_entry_via_sysreg(d, main_idx, vpn, 0xABCDE, 0, 0,
                           TLB_V | TLB_R | TLB_X | TLB_C);

    LookupResult r = lookup(d, (vpn << 12) | 0x200, ACC_READ, false, 0);
    check_bool("main_only.hit", r.hit, true);
    check("main_only.paddr", r.paddr, 0xABCDE200u);
    check_bool("main_only.cacheable", r.cacheable, true);
    check_bool("main_only.no_fault", r.fault, false);
}

static void test_lookup_pinned_only(Vtlb_unit* d) {
    printf("── Lookup: pinned TLB hit when main misses ──\n");
    reset(d);

    // Populate one pinned slot only
    write_entry_via_sysreg(d, 0x40 | 0, 0xCAFEC, 0x55555, 0, 0,
                           TLB_V | TLB_R | TLB_X | TLB_C);

    LookupResult r = lookup(d, 0xCAFEC'400, ACC_READ, false, 0);
    check_bool("pin_only.hit", r.hit, true);
    check("pin_only.paddr", r.paddr, 0x55555400u);
    check_bool("pin_only.cacheable", r.cacheable, true);
}

static void test_lookup_pinned_priority(Vtlb_unit* d) {
    printf("── Lookup: pinned hit wins over main hit ──\n");
    reset(d);

    // SAME VPN in both banks, different PPN.  For main to *actually*
    // hit (so we're testing priority and not just main-miss-vs-pin-hit),
    // the main IDX must match the VPN's set bits.
    uint32_t vpn = 0x33333;
    uint32_t main_ppn = 0xAAAAA;
    uint32_t pin_ppn  = 0xBBBBB;
    uint32_t main_idx = vpn & 0x1Fu;

    write_entry_via_sysreg(d, main_idx, vpn, main_ppn, 0, 0,
                           TLB_V | TLB_R | TLB_X);
    write_entry_via_sysreg(d, 0x40 | 1, vpn, pin_ppn,  0, 0,
                           TLB_V | TLB_R | TLB_X);

    LookupResult r = lookup(d, (vpn << 12) | 0x800, ACC_READ, false, 0);
    check_bool("priority.hit", r.hit, true);
    // Pinned wins → PPN should be pin_ppn, not main_ppn.
    check("priority.paddr_from_pinned",
          r.paddr, (pin_ppn << 12) | 0x800);
}

static void test_lookup_miss(Vtlb_unit* d) {
    printf("── Lookup: miss in both banks ──\n");
    reset(d);

    // No entries written.
    LookupResult r = lookup(d, 0xDEADB'EEF, ACC_READ, false, 0);
    check_bool("miss.no_hit", r.hit, false);
    check_bool("miss.no_fault", r.fault, false);
}

// Bit-slicing regression: writing the highest pinned slot must reach
// exactly that slot — not bleed into IDX[5] (main TLB way) or IDX[6]
// (the pinned/main selector itself).  A wrong index slice would
// either silently land in the wrong pinned slot, or worse, corrupt
// the main TLB.
static void test_pinned_high_slot_no_alias(Vtlb_unit* d) {
    printf("── Pinned high slot (%d) doesn't alias main TLB ──\n",
           PINNED_SLOTS - 1);
    reset(d);

    const uint32_t high = PINNED_SLOTS - 1;
    const uint32_t idx_high = 0x40u | high;

    write_entry_via_sysreg(d, idx_high, 0x88888, 0x99999, 0x11, 0,
                           TLB_V | TLB_R | TLB_X | TLB_C);

    // Readback at the same high IDX must reproduce the write.
    sys_write(d, SYSREG_MMU_TLB_IDX, idx_high);
    check("high_pin.vpn", sys_read(d, SYSREG_MMU_TLB_VPN),
          make_vpn_word(0x88888, 0x11));
    check("high_pin.pte", sys_read(d, SYSREG_MMU_TLB_PTE),
          make_pte_word(0x99999, 0, TLB_V | TLB_R | TLB_X | TLB_C));

    // Lookup must hit, and via the pinned bank (no main entry exists).
    LookupResult r = lookup(d, 0x88888'400, ACC_READ, false, 0x11);
    check_bool("high_pin.lookup_hit", r.hit, true);
    check("high_pin.lookup_paddr", r.paddr, 0x99999400u);

    // Any main TLB slot whose IDX matches the low bits of `high`
    // must remain empty.  Probe set = (high & 0x1F), both ways.
    for (uint32_t way = 0; way < 2; way++) {
        uint32_t main_idx = (way << 5) | (high & 0x1Fu);
        sys_write(d, SYSREG_MMU_TLB_IDX, main_idx);
        uint32_t main_pte = sys_read(d, SYSREG_MMU_TLB_PTE);
        char name[64];
        snprintf(name, sizeof(name), "high_pin.main_set%u_way%u_empty",
                 high & 0x1Fu, way);
        check(name, main_pte & TLB_V, 0u);
    }
}

static void test_fault_from_hit_bank(Vtlb_unit* d) {
    printf("── Lookup: fault status comes from the bank that hit ──\n");
    reset(d);

    // Main TLB entry: user-readable (won't fault).
    // Pinned entry at same VPN: supervisor-only (will fault for user).
    // Pinned wins on lookup → fault should fire even though the main
    // bank would have served cleanly.
    uint32_t vpn = 0x44444;
    uint32_t main_idx = vpn & 0x1Fu;
    write_entry_via_sysreg(d, main_idx, vpn, 0xCCCCC, 0, 0,
                           TLB_V | TLB_R | TLB_X | TLB_U);
    write_entry_via_sysreg(d, 0x40 | 0, vpn, 0xDDDDD, 0, 0,
                           TLB_V | TLB_R | TLB_X);  // U=0, supervisor-only

    LookupResult r = lookup(d, (vpn << 12) | 0x000, ACC_READ,
                            /*user=*/true, 0);
    check_bool("fault.hit_set", r.hit, true);
    // Pinned wins → user access fails permission check.
    check_bool("fault.user_faulted", r.fault, true);
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vtlb_unit* d = new Vtlb_unit;

    printf("── TLB Unit Composition Tests ──\n\n");

    test_idx_routing_main(d);
    test_idx_routing_pinned(d);
    test_shared_vpn_staging(d);
    test_lookup_main_only(d);
    test_lookup_pinned_only(d);
    test_lookup_pinned_priority(d);
    test_lookup_miss(d);
    test_pinned_high_slot_no_alias(d);
    test_fault_from_hit_bank(d);

    printf("\ntlb_unit: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return errors ? 1 : 0;
}
