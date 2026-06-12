// Verilator testbench for the gen2 MMU (mmu_bram)
//
// Verifies the MMU glue over tlb_unit_bram with its registered timing:
//   - bypass (identity map) when disabled and on force_bypass
//   - registered translation when enabled (drive at T, verdict at T+1)
//   - TLB miss vs protection fault
//   - concurrent A (translate) + B (bypass)
//   - commit-time FADDR/FSTAT latch (and that it ignores non-commit cycles)
//   - sysreg: MMUCR read/write, TLB readback passthrough

#include <cstdio>
#include <cstdint>
#include "Vmmu_bram.h"

enum TlbFlags { TLB_V = 1<<0, TLB_C = 1<<2, TLB_R = 1<<3, TLB_W = 1<<4, TLB_X = 1<<5, TLB_U = 1<<6, TLB_G = 1<<7 };
enum AccType { ACC_READ = 0b001, ACC_WRITE = 0b010, ACC_EXEC = 0b100 };
enum FaultType { FAULT_PROT = 0x0002 };
enum SysReg { MMU_CR = 0, MMU_FADDR = 1, MMU_FSTAT = 2, MMU_TLB_VPN = 3, MMU_TLB_PTE = 4, MMU_TLB_IDX = 5 };

static int errors = 0, tests = 0;

static void tick(Vmmu_bram* d) { d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval(); }
static void idle(Vmmu_bram* d) {
    d->i_a_req = 0; d->i_b_req = 0; d->i_a_force_bypass = 0; d->i_b_force_bypass = 0;
    d->i_sys_we = 0; d->i_sys_re = 0; d->i_fault_commit = 0;
}
static void reset(Vmmu_bram* d) {
    d->i_rst = 1;
    d->i_a_vaddr = 0; d->i_a_access_type = ACC_EXEC; d->i_a_user_mode = 0;
    d->i_b_vaddr = 0; d->i_b_access_type = ACC_READ; d->i_b_user_mode = 0;
    d->i_sys_reg = 0; d->i_sys_wdata = 0;
    d->i_fault_vaddr = 0; d->i_fault_status = 0;
    idle(d);
    tick(d); tick(d);
    d->i_rst = 0;
}

static uint32_t mk_vpn(uint32_t vpn, uint8_t asid) { return ((vpn & 0xFFFFF) << 8) | asid; }
static uint32_t mk_pte(uint32_t ppn, uint8_t flags) { return ((ppn & 0xFFFFF) << 12) | (flags & 0xFF); }

static void wrsys(Vmmu_bram* d, int reg, uint32_t data) {
    idle(d);
    d->i_sys_reg = reg; d->i_sys_wdata = data; d->i_sys_we = 1;
    tick(d);
    d->i_sys_we = 0;
}
static void set_mmucr(Vmmu_bram* d, uint8_t asid, bool en) {
    wrsys(d, MMU_CR, ((uint32_t)asid << 8) | (en ? 1 : 0));
}
static void write_main(Vmmu_bram* d, int set, int way, uint32_t vpn, uint32_t ppn, uint8_t asid, uint8_t flags) {
    wrsys(d, MMU_TLB_IDX, ((way & 1) << 5) | (set & 0x1F));
    wrsys(d, MMU_TLB_VPN, mk_vpn(vpn, asid));
    wrsys(d, MMU_TLB_PTE, mk_pte(ppn, flags));
}

struct Verdict { uint32_t paddr; bool hit, fault; uint32_t fstatus; bool cacheable; };

static Verdict translate_a(Vmmu_bram* d, uint32_t vaddr, uint8_t acc, bool user, bool req, bool fb) {
    idle(d);
    d->i_a_vaddr = vaddr; d->i_a_access_type = acc; d->i_a_user_mode = user;
    d->i_a_req = req; d->i_a_force_bypass = fb;
    tick(d);
    d->i_a_req = 0; d->i_a_force_bypass = 0;
    return { d->o_a_paddr, (bool)d->o_a_hit, (bool)d->o_a_fault, d->o_a_fault_status, (bool)d->o_a_cacheable };
}
static Verdict translate_b(Vmmu_bram* d, uint32_t vaddr, uint8_t acc, bool user, bool req, bool fb) {
    idle(d);
    d->i_b_vaddr = vaddr; d->i_b_access_type = acc; d->i_b_user_mode = user;
    d->i_b_req = req; d->i_b_force_bypass = fb;
    tick(d);
    d->i_b_req = 0; d->i_b_force_bypass = 0;
    return { d->o_b_paddr, (bool)d->o_b_hit, (bool)d->o_b_fault, d->o_b_fault_status, (bool)d->o_b_cacheable };
}
static void commit_fault(Vmmu_bram* d, uint32_t vaddr, uint32_t status) {
    idle(d);
    d->i_fault_commit = 1; d->i_fault_vaddr = vaddr; d->i_fault_status = status;
    tick(d);
    d->i_fault_commit = 0;
}
static uint32_t readback(Vmmu_bram* d, int reg) {
    idle(d);
    d->i_sys_reg = reg; d->i_sys_re = 1;
    tick(d);
    uint32_t r = d->o_sys_rdata;
    d->i_sys_re = 0;
    return r;
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
    Vmmu_bram* d = new Vmmu_bram;
    reset(d);

    // ── Disabled (M=0): identity-map bypass ──
    printf("-- bypass when disabled --\n");
    Verdict r = translate_a(d, 0x12345004, ACC_EXEC, false, true, false);
    check_bool("bypass hit", r.hit, true);
    check_bool("bypass nofault", r.fault, false);
    check("bypass paddr=vaddr", r.paddr, 0x12345004);
    check_bool("bypass uncacheable", r.cacheable, false);

    // ── Enable + main TLB translate (port A) ──
    printf("-- enabled translate --\n");
    set_mmucr(d, 1, true);
    write_main(d, 16, 0, 0x00010, 0x000A0, 1, TLB_V | TLB_R | TLB_X | TLB_C);
    r = translate_a(d, (0x00010u << 12) | 0x111, ACC_EXEC, false, true, false);
    check_bool("xlate hit", r.hit, true);
    check_bool("xlate nofault", r.fault, false);
    check("xlate paddr", r.paddr, (0x000A0u << 12) | 0x111);
    check_bool("xlate cacheable", r.cacheable, true);

    // ── force_bypass overrides translation even when enabled ──
    printf("-- force_bypass --\n");
    r = translate_a(d, (0x00010u << 12) | 0x111, ACC_EXEC, false, true, true);
    check("force_bypass paddr=vaddr", r.paddr, (0x00010u << 12) | 0x111);
    check_bool("force_bypass uncacheable", r.cacheable, false);

    // ── Port B translate ──
    printf("-- port B translate --\n");
    r = translate_b(d, (0x00010u << 12) | 0x008, ACC_READ, false, true, false);
    check_bool("B hit", r.hit, true);
    check("B paddr", r.paddr, (0x000A0u << 12) | 0x008);

    // ── Concurrent: A translate, B bypass (force) ──
    printf("-- concurrent A xlate + B bypass --\n");
    idle(d);
    d->i_a_vaddr = (0x00010u << 12) | 0x020; d->i_a_access_type = ACC_EXEC; d->i_a_req = 1; d->i_a_force_bypass = 0;
    d->i_b_vaddr = 0x70000040; d->i_b_access_type = ACC_READ; d->i_b_req = 1; d->i_b_force_bypass = 1;
    tick(d);
    check("conc A paddr (xlate)", d->o_a_paddr, (0x000A0u << 12) | 0x020);
    check("conc B paddr (bypass)", d->o_b_paddr, 0x70000040u);
    d->i_a_req = 0; d->i_b_req = 0; d->i_b_force_bypass = 0;

    // ── TLB miss (enabled, unmapped) ──
    printf("-- miss --\n");
    r = translate_a(d, (0x0001Fu << 12), ACC_EXEC, false, true, false);
    check_bool("miss nohit", r.hit, false);
    check_bool("miss nofault", r.fault, false);

    // ── Protection fault (write to read-only) ──
    printf("-- protection fault --\n");
    write_main(d, 7, 0, 0x00007, 0x0E0, 1, TLB_V | TLB_R);
    r = translate_b(d, (0x00007u << 12), ACC_WRITE, false, true, false);
    check_bool("prot fault", r.fault, true);
    check("prot fstatus", r.fstatus & 0xF, FAULT_PROT);

    // ── Verdict hold: the T+1 verdict persists across idle cycles ──
    // A stalled consumer (MEM held at its data-ready cycle by WB
    // back-pressure) reads the verdict later than T+1; the port must hold it
    // until the next query, insensitive to input wiggle while idle.
    printf("-- verdict hold --\n");
    r = translate_b(d, (0x00010u << 12) | 0x00C, ACC_READ, false, true, false);
    check("hold T+1 paddr", r.paddr, (0x000A0u << 12) | 0x00C);
    for (int i = 0; i < 3; i++) {
        idle(d);
        d->i_b_vaddr = 0x5A5A5000;             // wiggle: must not disturb the held verdict
        tick(d);
        check("hold paddr", d->o_b_paddr, (0x000A0u << 12) | 0x00C);
        check_bool("hold hit", d->o_b_hit, true);
        check_bool("hold nofault", d->o_b_fault, false);
    }
    // A protection-fault verdict holds the same way (the store-commit gate
    // samples o_b_fault on the consumer's advance cycle, not at T+1).
    r = translate_b(d, (0x00007u << 12), ACC_WRITE, false, true, false);
    check_bool("hold prot T+1 fault", r.fault, true);
    for (int i = 0; i < 3; i++) {
        idle(d);
        d->i_b_vaddr = 0x5A5A5000;
        tick(d);
        check_bool("hold prot fault", d->o_b_fault, true);
        check("hold prot fstatus", d->o_b_fault_status & 0xF, FAULT_PROT);
    }
    // A bypass verdict holds too (the bypass capture gates on the strobe).
    r = translate_b(d, 0x70000040, ACC_READ, false, true, true);
    check("hold bypass T+1 paddr", r.paddr, 0x70000040u);
    for (int i = 0; i < 3; i++) {
        idle(d);
        d->i_b_vaddr = 0x5A5A5000;
        tick(d);
        check("hold bypass paddr", d->o_b_paddr, 0x70000040u);
        check_bool("hold bypass hit", d->o_b_hit, true);
    }

    // ── Commit-time fault latch ──
    printf("-- commit fault latch --\n");
    commit_fault(d, 0x0BAD1000, 0x00000123);
    check("FADDR latched", readback(d, MMU_FADDR), 0x0BAD1000);
    check("FSTAT latched", readback(d, MMU_FSTAT), 0x00000123);
    // A non-commit cycle must not disturb FADDR/FSTAT.
    translate_a(d, (0x00010u << 12), ACC_EXEC, false, true, false);
    check("FADDR unchanged w/o commit", readback(d, MMU_FADDR), 0x0BAD1000);

    // ── Sysreg: MMUCR readback + TLB passthrough ──
    printf("-- sysreg readback --\n");
    check("MMUCR readback", readback(d, MMU_CR), (1u << 8) | 1u);
    wrsys(d, MMU_TLB_IDX, (0 << 5) | 16);
    check("TLB VPN passthrough", readback(d, MMU_TLB_VPN), mk_vpn(0x00010, 1));

    printf("\n%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
