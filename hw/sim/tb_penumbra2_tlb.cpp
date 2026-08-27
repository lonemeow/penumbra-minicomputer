// Verilator testbench for the gen2 BRAM-backed main TLB (penumbra2_tlb)
//
// Unlike the combinational gen1 TLB, penumbra2_tlb is a registered (BRAM) lookup:
// drive the query at cycle T, the verdict is valid at T+1. Every lookup /
// readback helper here therefore drives inputs and then ticks one edge before
// sampling outputs.
//
// Coverage:
//   - registered translation on port A and port B
//   - both ports translating concurrently from one storage copy
//   - permission faults (R/W/X and user/supervisor)
//   - global (G) ASID bypass, miss (no entry), invalidation
//   - registered indexed readback with V from the flop vector

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_tlb.h"

enum TlbFlags {
    TLB_V = 1 << 0, TLB_C = 1 << 2, TLB_R = 1 << 3, TLB_W = 1 << 4,
    TLB_X = 1 << 5, TLB_U = 1 << 6, TLB_G = 1 << 7,
};
enum AccType { ACC_READ = 0b001, ACC_WRITE = 0b010, ACC_EXEC = 0b100 };

static int errors = 0, tests = 0;

static void tick(Vpenumbra2_tlb* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void idle(Vpenumbra2_tlb* d) {
    d->i_a_lookup_en = 0;
    d->i_b_lookup_en = 0;
    d->i_write_en = 0;
    d->i_read_en = 0;
}

static void reset(Vpenumbra2_tlb* d) {
    d->i_rst = 1;
    d->i_asid = 0;
    d->i_a_vaddr = 0; d->i_a_access_type = ACC_READ; d->i_a_user_mode = 0;
    d->i_b_vaddr = 0; d->i_b_access_type = ACC_READ; d->i_b_user_mode = 0;
    d->i_idx_set = 0; d->i_idx_way = 0; d->i_write_vpn = 0; d->i_write_pte = 0;
    idle(d);
    tick(d); tick(d);
    d->i_rst = 0;
}

static uint32_t make_vpn_word(uint32_t vpn, uint8_t asid) {
    return ((vpn & 0xFFFFF) << 8) | asid;
}
static uint32_t make_pte_word(uint32_t ppn, uint8_t sw, uint8_t flags) {
    return ((ppn & 0xFFFFF) << 12) | ((sw & 0xF) << 8) | (flags & 0xFF);
}

// Indexed write — port B only, so no concurrent B-read/readback.
static void write_entry(Vpenumbra2_tlb* d, int set, int way, uint32_t vpn,
                        uint32_t ppn, uint8_t asid, uint8_t flags) {
    idle(d);
    d->i_idx_set = set; d->i_idx_way = way;
    d->i_write_vpn = make_vpn_word(vpn, asid);
    d->i_write_pte = make_pte_word(ppn, 0, flags);
    d->i_write_en = 1;
    tick(d);
    d->i_write_en = 0;
}

struct Verdict { uint32_t paddr; bool hit, fault; bool cacheable; };

// Port A registered translate.
static Verdict lookup_a(Vpenumbra2_tlb* d, uint32_t vaddr, uint8_t acc,
                        bool user, uint8_t asid) {
    idle(d);
    d->i_asid = asid;
    d->i_a_vaddr = vaddr; d->i_a_access_type = acc; d->i_a_user_mode = user;
    d->i_a_lookup_en = 1;
    tick(d);
    d->i_a_lookup_en = 0;
    return { d->o_a_paddr, (bool)d->o_a_hit, (bool)d->o_a_fault,
             (bool)d->o_a_cacheable };
}

// Port B registered translate.
static Verdict lookup_b(Vpenumbra2_tlb* d, uint32_t vaddr, uint8_t acc,
                        bool user, uint8_t asid) {
    idle(d);
    d->i_asid = asid;
    d->i_b_vaddr = vaddr; d->i_b_access_type = acc; d->i_b_user_mode = user;
    d->i_b_lookup_en = 1;
    tick(d);
    d->i_b_lookup_en = 0;
    return { d->o_b_paddr, (bool)d->o_b_hit, (bool)d->o_b_fault,
             (bool)d->o_b_cacheable };
}

// Both ports translate in the same cycle (the dual-port point).
static void lookup_ab(Vpenumbra2_tlb* d, uint32_t va_a, uint32_t va_b, uint8_t asid,
                      Verdict* ra, Verdict* rb) {
    idle(d);
    d->i_asid = asid;
    d->i_a_vaddr = va_a; d->i_a_access_type = ACC_READ; d->i_a_user_mode = 0;
    d->i_b_vaddr = va_b; d->i_b_access_type = ACC_READ; d->i_b_user_mode = 0;
    d->i_a_lookup_en = 1; d->i_b_lookup_en = 1;
    tick(d);
    d->i_a_lookup_en = 0; d->i_b_lookup_en = 0;
    *ra = { d->o_a_paddr, (bool)d->o_a_hit, (bool)d->o_a_fault, (bool)d->o_a_cacheable };
    *rb = { d->o_b_paddr, (bool)d->o_b_hit, (bool)d->o_b_fault, (bool)d->o_b_cacheable };
}

// Registered indexed readback — port B only.
static void readback(Vpenumbra2_tlb* d, int set, int way, uint32_t* vpn, uint32_t* pte) {
    idle(d);
    d->i_idx_set = set; d->i_idx_way = way;
    d->i_read_en = 1;
    tick(d);
    d->i_read_en = 0;
    *vpn = d->o_read_vpn;
    *pte = d->o_read_pte;
}

static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp); errors++; }
}
static void check_bool(const char* n, bool got, bool exp) {
    tests++;
    if (got != exp) { printf("  FAIL [%s]: got %s, expected %s\n", n, got?"true":"false", exp?"true":"false"); errors++; }
}

int main() {
    Vpenumbra2_tlb* d = new Vpenumbra2_tlb;
    reset(d);

    // ── Reset leaves entries invalid (V from flop vec = 0) ──
    printf("-- reset: V=0 --\n");
    for (int s = 0; s < 4; s++) {
        uint32_t vpn, pte;
        readback(d, s, 0, &vpn, &pte); check("reset V0", pte & TLB_V, 0u);
        readback(d, s, 1, &vpn, &pte); check("reset V1", pte & TLB_V, 0u);
    }

    // ── Port-A translate of a written entry ──
    printf("-- port A translate --\n");
    write_entry(d, 3, 0, /*vpn*/0x00003, /*ppn*/0x00042, /*asid*/1, TLB_V | TLB_R | TLB_X);
    Verdict r = lookup_a(d, (0x00003u << 12) | 0x123, ACC_READ, false, 1);
    check_bool("A hit", r.hit, true);
    check_bool("A nofault", r.fault, false);
    check("A paddr", r.paddr, (0x00042u << 12) | 0x123);

    // ── Port-B translate of a different entry ──
    printf("-- port B translate --\n");
    write_entry(d, 5, 1, 0x00025, 0x00099, 1, TLB_V | TLB_R | TLB_W);
    r = lookup_b(d, (0x00025u << 12) | 0x004, ACC_WRITE, false, 1);
    check_bool("B hit", r.hit, true);
    check_bool("B nofault", r.fault, false);
    check("B paddr", r.paddr, (0x00099u << 12) | 0x004);

    // ── Both ports concurrently, one storage copy ──
    printf("-- concurrent A+B --\n");
    Verdict ra, rb;
    lookup_ab(d, (0x00003u << 12) | 0x010, (0x00025u << 12) | 0x020, 1, &ra, &rb);
    check_bool("AB A hit", ra.hit, true);
    check_bool("AB B hit", rb.hit, true);
    check("AB A paddr", ra.paddr, (0x00042u << 12) | 0x010);
    check("AB B paddr", rb.paddr, (0x00099u << 12) | 0x020);

    // ── Permission: write to a read-only page faults ──
    printf("-- permission R/W --\n");
    write_entry(d, 7, 0, 0x00007, 0x00007, 1, TLB_V | TLB_R);      // RO
    r = lookup_a(d, (0x00007u << 12), ACC_WRITE, false, 1);
    check_bool("RO write hit", r.hit, true);
    check_bool("RO write fault", r.fault, true);
    r = lookup_a(d, (0x00007u << 12), ACC_READ, false, 1);         // read OK
    check_bool("RO read nofault", r.fault, false);

    // ── Permission: user access needs U ──
    printf("-- permission user --\n");
    write_entry(d, 9, 0, 0x00009, 0x00009, 1, TLB_V | TLB_R);      // U=0
    r = lookup_a(d, (0x00009u << 12), ACC_READ, true, 1);          // user
    check_bool("user U=0 fault", r.fault, true);
    r = lookup_a(d, (0x00009u << 12), ACC_READ, false, 1);         // supervisor
    check_bool("supervisor U=0 nofault", r.fault, false);

    // ── Global entry ignores ASID ──
    printf("-- global ASID bypass --\n");
    write_entry(d, 11, 0, 0x0000B, 0x000AB, 4, TLB_V | TLB_R | TLB_G);
    r = lookup_a(d, (0x0000Bu << 12), ACC_READ, false, /*asid*/99);
    check_bool("global hit other ASID", r.hit, true);

    // ── Miss: unmapped VPN ──
    printf("-- miss --\n");
    r = lookup_a(d, (0x0001Fu << 12), ACC_READ, false, 1);
    check_bool("miss nohit", r.hit, false);
    check_bool("miss nofault", r.fault, false);

    // ── Invalidate (V=0) then look up ──
    printf("-- invalidate --\n");
    write_entry(d, 3, 0, 0x00003, 0x00042, 1, /*flags*/0);        // V=0
    r = lookup_a(d, (0x00003u << 12), ACC_READ, false, 1);
    check_bool("invalidated nohit", r.hit, false);

    // ── A disabled lookup presents no verdict ──
    // The enable qualifies the outputs rather than gating the match cone, so
    // an idle port must stay silent even while a matching entry sits under
    // the address it happens to be presenting. Every other check here samples
    // with the enable high, so this is the only one that pins that.
    printf("-- disabled lookup is silent --\n");
    write_entry(d, 15, 0, 0x0000F, 0x000EF, 1, TLB_V | TLB_R | TLB_C);
    idle(d);
    d->i_asid = 1;
    d->i_idx_set = 15;                 // port B's read address when idle
    d->i_a_vaddr = (0x0000Fu << 12); d->i_a_access_type = ACC_READ; d->i_a_user_mode = 0;
    d->i_b_vaddr = (0x0000Fu << 12); d->i_b_access_type = ACC_READ; d->i_b_user_mode = 0;
    d->eval();
    check_bool("idle A nohit",       d->o_a_hit,       false);
    check_bool("idle A nofault",     d->o_a_fault,     false);
    check_bool("idle A nocacheable", d->o_a_cacheable, false);
    check_bool("idle B nohit",       d->o_b_hit,       false);
    check_bool("idle B nofault",     d->o_b_fault,     false);
    check_bool("idle B nocacheable", d->o_b_cacheable, false);

    // Same, on the permission-fault leg: a write to that read-only entry
    // would fault if the verdict were live.
    d->i_a_access_type = ACC_WRITE;
    d->i_b_access_type = ACC_WRITE;
    d->eval();
    check_bool("idle A no perm fault", d->o_a_fault, false);
    check_bool("idle B no perm fault", d->o_b_fault, false);

    // ── Readback returns the written words ──
    printf("-- readback --\n");
    write_entry(d, 13, 1, 0x0000D, 0x000CD, 7, TLB_V | TLB_R | TLB_W);
    uint32_t vpn, pte;
    readback(d, 13, 1, &vpn, &pte);
    check("readback vpn", vpn, make_vpn_word(0x0000D, 7));
    check("readback pte", pte, make_pte_word(0x000CD, 0, TLB_V | TLB_R | TLB_W));

    printf("\n%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
