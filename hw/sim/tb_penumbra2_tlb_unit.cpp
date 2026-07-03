// Verilator testbench for the gen2 TLB unit (penumbra2_tlb_unit)
//
// Verifies the async-LUTRAM-main + flop-pinned combine and its combinational
// timing:
//   - translation on both ports answers in the launch cycle (the lookup
//     helpers sample after the edge with the query held — same verdict)
//   - pinned-hit-wins over the main TLB
//   - concurrent A+B translation
//   - permission faults via main and via pinned
//   - sysreg readback (main = registered, pinned/index = held-combinational)
//   - the verdict is combinational off the live query: it moves when the
//     query moves. The registered hold (capture on the strobe, stable for a
//     stalled consumer) lives downstream in penumbra2_mmu, not here.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra2_tlb_unit.h"

enum TlbFlags {
    TLB_V = 1 << 0, TLB_C = 1 << 2, TLB_R = 1 << 3, TLB_W = 1 << 4,
    TLB_X = 1 << 5, TLB_U = 1 << 6, TLB_G = 1 << 7,
};
enum AccType { ACC_READ = 0b001, ACC_WRITE = 0b010, ACC_EXEC = 0b100 };
enum SysReg { MMU_TLB_VPN = 3, MMU_TLB_PTE = 4, MMU_TLB_IDX = 5 };

static int errors = 0, tests = 0;

static void tick(Vpenumbra2_tlb_unit* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}
static void idle(Vpenumbra2_tlb_unit* d) {
    d->i_a_lookup_en = 0; d->i_b_lookup_en = 0;
    d->i_sys_we = 0; d->i_sys_re = 0;
}
static void reset(Vpenumbra2_tlb_unit* d) {
    d->i_rst = 1; d->i_asid = 0;
    d->i_a_vaddr = 0; d->i_a_access_type = ACC_READ; d->i_a_user_mode = 0;
    d->i_b_vaddr = 0; d->i_b_access_type = ACC_READ; d->i_b_user_mode = 0;
    d->i_sys_reg = 0; d->i_sys_wdata = 0;
    idle(d);
    tick(d); tick(d);
    d->i_rst = 0;
}

static uint32_t mk_vpn(uint32_t vpn, uint8_t asid) { return ((vpn & 0xFFFFF) << 8) | asid; }
static uint32_t mk_pte(uint32_t ppn, uint8_t flags) { return ((ppn & 0xFFFFF) << 12) | (flags & 0xFF); }

static void wrsys(Vpenumbra2_tlb_unit* d, int reg, uint32_t data) {
    idle(d);
    d->i_sys_reg = reg; d->i_sys_wdata = data; d->i_sys_we = 1;
    tick(d);
    d->i_sys_we = 0;
}
static void write_main(Vpenumbra2_tlb_unit* d, int set, int way, uint32_t vpn,
                       uint32_t ppn, uint8_t asid, uint8_t flags) {
    wrsys(d, MMU_TLB_IDX, ((way & 1) << 5) | (set & 0x1F));   // bit6=0 → main
    wrsys(d, MMU_TLB_VPN, mk_vpn(vpn, asid));
    wrsys(d, MMU_TLB_PTE, mk_pte(ppn, flags));               // PTE write commits
}
static void write_pinned(Vpenumbra2_tlb_unit* d, int slot, uint32_t vpn,
                         uint32_t ppn, uint8_t asid, uint8_t flags) {
    wrsys(d, MMU_TLB_IDX, 0x40 | (slot & 0x7));               // bit6=1 → pinned
    wrsys(d, MMU_TLB_VPN, mk_vpn(vpn, asid));
    wrsys(d, MMU_TLB_PTE, mk_pte(ppn, flags));
}

struct Verdict { uint32_t paddr; bool hit, fault; bool cacheable; };

static Verdict lookup_a(Vpenumbra2_tlb_unit* d, uint32_t vaddr, uint8_t acc, bool user, uint8_t asid) {
    idle(d); d->i_asid = asid;
    d->i_a_vaddr = vaddr; d->i_a_access_type = acc; d->i_a_user_mode = user; d->i_a_lookup_en = 1;
    tick(d);
    d->i_a_lookup_en = 0;
    return { d->o_a_paddr, (bool)d->o_a_hit, (bool)d->o_a_fault, (bool)d->o_a_cacheable };
}
static Verdict lookup_b(Vpenumbra2_tlb_unit* d, uint32_t vaddr, uint8_t acc, bool user, uint8_t asid) {
    idle(d); d->i_asid = asid;
    d->i_b_vaddr = vaddr; d->i_b_access_type = acc; d->i_b_user_mode = user; d->i_b_lookup_en = 1;
    tick(d);
    d->i_b_lookup_en = 0;
    return { d->o_b_paddr, (bool)d->o_b_hit, (bool)d->o_b_fault, (bool)d->o_b_cacheable };
}
// Readback: set TLB_INDEX, then launch the read; main readback lands next cycle.
static uint32_t readback(Vpenumbra2_tlb_unit* d, int index, int reg) {
    wrsys(d, MMU_TLB_IDX, index);
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
    Vpenumbra2_tlb_unit* d = new Vpenumbra2_tlb_unit;
    reset(d);

    // ── Main TLB translate (port A) ──
    printf("-- main translate --\n");
    write_main(d, 16, 0, 0x00010, 0x000A0, 1, TLB_V | TLB_R | TLB_X);
    Verdict r = lookup_a(d, (0x00010u << 12) | 0x111, ACC_EXEC, false, 1);
    check_bool("main A hit", r.hit, true);
    check_bool("main A nofault", r.fault, false);
    check("main A paddr", r.paddr, (0x000A0u << 12) | 0x111);

    // ── Pinned translate (port A) ──
    printf("-- pinned translate --\n");
    write_pinned(d, 3, 0x00021, 0x000B0, 1, TLB_V | TLB_R | TLB_X);
    r = lookup_a(d, (0x00021u << 12) | 0x004, ACC_READ, false, 1);
    check_bool("pin A hit", r.hit, true);
    check("pin A paddr", r.paddr, (0x000B0u << 12) | 0x004);

    // ── Pinned-hit-wins over the main TLB for the same VPN ──
    printf("-- pinned wins --\n");
    write_main(d, 5, 0, 0x00005, 0x000C0, 1, TLB_V | TLB_R);   // main: ppn C0
    write_pinned(d, 4, 0x00005, 0x000D0, 1, TLB_V | TLB_R);    // pinned: ppn D0
    r = lookup_a(d, (0x00005u << 12), ACC_READ, false, 1);
    check_bool("both hit", r.hit, true);
    check("pinned wins paddr", r.paddr, (0x000D0u << 12));

    // ── Port B translate ──
    printf("-- port B translate --\n");
    r = lookup_b(d, (0x00010u << 12) | 0x008, ACC_READ, false, 1);   // main entry from above
    check_bool("B hit (main)", r.hit, true);
    check("B paddr", r.paddr, (0x000A0u << 12) | 0x008);

    // ── Concurrent A (pinned) + B (main) ──
    printf("-- concurrent A+B --\n");
    idle(d); d->i_asid = 1;
    d->i_a_vaddr = (0x00021u << 12) | 0x010; d->i_a_access_type = ACC_READ; d->i_a_user_mode = 0; d->i_a_lookup_en = 1;
    d->i_b_vaddr = (0x00010u << 12) | 0x020; d->i_b_access_type = ACC_READ; d->i_b_user_mode = 0; d->i_b_lookup_en = 1;
    tick(d);
    check_bool("AB A hit", (bool)d->o_a_hit, true);
    check("AB A paddr (pinned)", d->o_a_paddr, (0x000B0u << 12) | 0x010);
    check_bool("AB B hit", (bool)d->o_b_hit, true);
    check("AB B paddr (main)", d->o_b_paddr, (0x000A0u << 12) | 0x020);
    d->i_a_lookup_en = 0; d->i_b_lookup_en = 0;

    // ── Permission fault: write to a read-only page (main and pinned) ──
    printf("-- permission faults --\n");
    r = lookup_a(d, (0x00005u << 12), ACC_WRITE, false, 1);    // pinned RO entry, write
    check_bool("pinned RO write fault", r.fault, true);
    write_main(d, 7, 0, 0x00007, 0x0E0, 1, TLB_V | TLB_R);     // main RO
    r = lookup_a(d, (0x00007u << 12), ACC_WRITE, false, 1);
    check_bool("main RO write fault", r.fault, true);

    // ── Miss ──
    printf("-- miss --\n");
    r = lookup_a(d, (0x0001Fu << 12), ACC_READ, false, 1);
    check_bool("miss nohit", r.hit, false);
    check_bool("miss nofault", r.fault, false);

    // ── Sysreg readback (main registered, pinned combinational) ──
    printf("-- readback --\n");
    check("main readback vpn", readback(d, (0 << 5) | 16, MMU_TLB_VPN), mk_vpn(0x00010, 1));
    check("main readback pte", readback(d, (0 << 5) | 16, MMU_TLB_PTE), mk_pte(0x000A0, TLB_V | TLB_R | TLB_X));
    check("pinned readback vpn", readback(d, 0x40 | 3, MMU_TLB_VPN), mk_vpn(0x00021, 1));
    check("index readback", readback(d, 0x40 | 3, MMU_TLB_IDX), (uint32_t)(0x40 | 3));

    // ── Combinational verdict: the port answers the live query ──
    // Both TLBs read combinationally (async LUTRAM main, flop pinned), so
    // the verdict must track the query presented this cycle. A stale
    // captured verdict here would break penumbra2_mmu's capture-on-strobe
    // contract — the MMU owns the registered hold, this port must not.
    printf("-- combinational verdict (live query) --\n");
    idle(d); d->i_asid = 1;
    d->i_a_vaddr = (0x00021u << 12); d->i_a_access_type = ACC_READ; d->i_a_user_mode = 0; d->i_a_lookup_en = 1;
    d->eval();   // launch cycle: verdict for 0x21 (pinned hit), no edge needed
    check_bool("live query hit", (bool)d->o_a_hit, true);
    check("live query paddr", d->o_a_paddr, (0x000B0u << 12));
    // Move the live query to a miss without a clock edge: the verdict follows.
    d->i_a_vaddr = (0x0001Eu << 12); d->eval();
    check_bool("live change follows: miss", (bool)d->o_a_hit, false);
    // And back: the pinned hit reappears, still without an edge.
    d->i_a_vaddr = (0x00021u << 12); d->eval();
    check_bool("live change follows: hit again", (bool)d->o_a_hit, true);
    check("live change follows: paddr", d->o_a_paddr, (0x000B0u << 12));
    d->i_a_lookup_en = 0;

    printf("\n%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
