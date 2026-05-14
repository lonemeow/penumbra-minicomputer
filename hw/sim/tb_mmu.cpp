// Verilator testbench for the Penumbra MMU
//
// Tests the MMU wrapper (mmu.sv) — bypass/translate mux, alignment
// check, fault latching, MMUCR, and sysreg routing.  The underlying
// TLB banks have their own unit tests (tb_tlb, tb_tlb_pinned,
// tb_tlb_unit); this testbench covers what only the MMU wrapper can
// break.
//
// Documented contract (from mmu.sv header):
//
//   Translation output priority: alignment > MMU-translated > bypass.
//     - When MMUCR.M=0 OR i_force_bypass=1: identity map, C=0,
//       hit=1, fault=0.
//     - When M=1 and !force_bypass: route through tlb_unit; miss
//       generates fault.
//     - Misaligned word/half-word access faults regardless of MMU
//       state.
//
//   Fault-info latching priority: alignment > TLB prot > TLB miss
//                                 > bus fault.
//     - The first three branches require i_req && !i_force_bypass;
//       the bus-fault branch only requires !i_force_bypass.
//     - Vector fetches set i_force_bypass=1 and must NOT overwrite
//       the original exception's fault info.
//
//   Sysreg routing: reg 0 = MMUCR, 1 = FADDR (ro), 2 = FSTAT (ro);
//   regs 3+ pass through to tlb_unit.

#include <cstdio>
#include <cstdint>
#include "Vmmu.h"

// Sysregs
enum SysReg {
    SYSREG_MMU_CR       = 0,
    SYSREG_MMU_FADDR    = 1,
    SYSREG_MMU_FSTAT    = 2,
    SYSREG_MMU_TLB_VPN  = 3,
    SYSREG_MMU_TLB_PTE  = 4,
    SYSREG_MMU_TLB_IDX  = 5,
};

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

enum MemSize {
    SZ_BYTE = 0,
    SZ_HALF = 1,
    SZ_WORD = 2,
};

// Fault class values (penumbra_pkg.sv FAULT_* constants)
enum FaultClass {
    FAULT_TLB_MISS = 0x1,
    FAULT_PROT     = 0x2,
    FAULT_ALIGN    = 0x3,
    FAULT_BUS      = 0x4,
};

static int errors = 0, tests = 0;

static void tick(Vmmu* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vmmu* d) {
    d->i_rst = 1;
    d->i_vaddr = 0;
    d->i_access_type = ACC_READ;
    d->i_user_mode = 0;
    d->i_req = 0;
    d->i_force_bypass = 0;
    d->i_mem_size = SZ_WORD;
    d->i_bus_fault = 0;
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

static void sys_write(Vmmu* d, uint32_t reg, uint32_t data) {
    d->i_sys_reg = reg;
    d->i_sys_wdata = data;
    d->i_sys_we = 1;
    tick(d);
    d->i_sys_we = 0;
}

static uint32_t sys_read(Vmmu* d, uint32_t reg) {
    d->i_sys_reg = reg;
    d->eval();
    return d->o_sys_rdata;
}

// Enable MMU with the given ASID.
static void mmu_enable(Vmmu* d, uint8_t asid) {
    uint32_t mmucr = (asid << 8) | 0x1u;
    sys_write(d, SYSREG_MMU_CR, mmucr);
}

// Populate a main TLB slot.  The lookup_set comes from vaddr[16:12],
// so the IDX must encode the matching set bits — encode it inline.
static void write_main_tlb(Vmmu* d, uint32_t vpn, uint32_t ppn,
                           uint8_t asid, uint8_t flags) {
    uint32_t idx = vpn & 0x1Fu;  // bit6=0 (main), way=0, set=vpn[4:0]
    sys_write(d, SYSREG_MMU_TLB_IDX, idx);
    sys_write(d, SYSREG_MMU_TLB_VPN, make_vpn_word(vpn, asid));
    sys_write(d, SYSREG_MMU_TLB_PTE, make_pte_word(ppn, 0, flags));
}

// Drive a single-cycle translation request and return all outputs.
struct XlateResult {
    uint32_t paddr;
    bool     fault;
    bool     hit;
    bool     align;
    bool     cacheable;
};

static XlateResult xlate(Vmmu* d, uint32_t vaddr, uint8_t access_type,
                         bool user_mode, uint8_t mem_size,
                         bool force_bypass, bool bus_fault) {
    d->i_vaddr = vaddr;
    d->i_access_type = access_type;
    d->i_user_mode = user_mode ? 1 : 0;
    d->i_req = 1;
    d->i_force_bypass = force_bypass ? 1 : 0;
    d->i_mem_size = mem_size;
    d->i_bus_fault = bus_fault ? 1 : 0;
    d->eval();

    XlateResult r;
    r.paddr     = d->o_paddr;
    r.fault     = d->o_fault;
    r.hit       = d->o_hit;
    r.align     = d->o_align;
    r.cacheable = d->o_cacheable;
    return r;
}

// Advance one clock so fault info latching takes effect, then
// drop i_req so subsequent latches don't pollute.
static void commit_xlate(Vmmu* d) {
    tick(d);
    d->i_req = 0;
    d->i_bus_fault = 0;
    d->i_force_bypass = 0;
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

static void test_reset_state(Vmmu* d) {
    printf("── Reset: MMUCR=0, faults clear, M=0 ──\n");
    reset(d);
    check("reset.mmucr", sys_read(d, SYSREG_MMU_CR), 0u);
    check("reset.faddr", sys_read(d, SYSREG_MMU_FADDR), 0u);
    check("reset.fstat", sys_read(d, SYSREG_MMU_FSTAT), 0u);
}

static void test_bypass_identity(Vmmu* d) {
    printf("── M=0 (bypass): identity map, C=0, hit=1, no fault ──\n");
    reset(d);

    XlateResult r = xlate(d, 0xCAFE'B000, ACC_READ, false, SZ_WORD,
                          /*force_bypass=*/false, /*bus_fault=*/false);
    check("bypass.paddr_identity", r.paddr, 0xCAFEB000u);
    check_bool("bypass.cacheable_off", r.cacheable, false);
    check_bool("bypass.hit_on", r.hit, true);
    check_bool("bypass.no_fault", r.fault, false);
    commit_xlate(d);
}

static void test_force_bypass_overrides_enable(Vmmu* d) {
    printf("── i_force_bypass=1 forces identity even with M=1 ──\n");
    reset(d);
    mmu_enable(d, 0x05);

    // Without force_bypass, an un-populated TLB would miss.  With
    // force_bypass, the request bypasses the TLB entirely.
    XlateResult r = xlate(d, 0x9999'A000, ACC_EXEC, false, SZ_WORD,
                          /*force_bypass=*/true, false);
    check("force_bypass.paddr_identity", r.paddr, 0x9999A000u);
    check_bool("force_bypass.no_fault", r.fault, false);
    check_bool("force_bypass.cacheable_off", r.cacheable, false);
    commit_xlate(d);
}

static void test_mmu_enable_with_hit(Vmmu* d) {
    printf("── M=1, populated TLB → hit, paddr from TLB ──\n");
    reset(d);
    mmu_enable(d, 0x01);

    write_main_tlb(d, 0x12345, 0xABCDE, 0x01,
                   TLB_V | TLB_R | TLB_W | TLB_X | TLB_U | TLB_C);

    XlateResult r = xlate(d, 0x12345'080, ACC_READ, false, SZ_WORD,
                          false, false);
    check_bool("enable_hit.hit", r.hit, true);
    check("enable_hit.paddr", r.paddr, 0xABCDE080u);
    check_bool("enable_hit.cacheable", r.cacheable, true);
    check_bool("enable_hit.no_fault", r.fault, false);
    commit_xlate(d);
}

static void test_tlb_miss_latches_fault(Vmmu* d) {
    printf("── M=1, miss: fault, FAULT_TLB_MISS latched to FSTAT ──\n");
    reset(d);
    mmu_enable(d, 0x01);

    // No TLB entries: every access misses.
    XlateResult r = xlate(d, 0xDEADC'000, ACC_WRITE, true, SZ_WORD,
                          false, false);
    check_bool("miss.fault", r.fault, true);
    check_bool("miss.no_hit", r.hit, false);
    commit_xlate(d);

    // FADDR latched
    check("miss.faddr", sys_read(d, SYSREG_MMU_FADDR), 0xDEADC000u);
    // FSTAT.class = FAULT_TLB_MISS (low 4 bits)
    check("miss.fstat_class",
          sys_read(d, SYSREG_MMU_FSTAT) & 0xF, (uint32_t)FAULT_TLB_MISS);
}

static void test_alignment_fault_regardless_of_mmu(Vmmu* d) {
    printf("── Alignment check fires in both bypass and translate modes ──\n");

    // Bypass mode (M=0): misaligned word should still fault.
    reset(d);
    XlateResult r_bypass = xlate(d, 0x1000'0002, ACC_READ, false, SZ_WORD,
                                 false, false);
    check_bool("align.bypass_fault", r_bypass.fault, true);
    check_bool("align.bypass_o_align", r_bypass.align, true);
    commit_xlate(d);

    // FSTAT.class = FAULT_ALIGN
    check("align.bypass_fstat",
          sys_read(d, SYSREG_MMU_FSTAT) & 0xF, (uint32_t)FAULT_ALIGN);

    // Translate mode (M=1): same — alignment > TLB.
    reset(d);
    mmu_enable(d, 0);
    write_main_tlb(d, 0x10000, 0x20000, 0,
                   TLB_V | TLB_R | TLB_W | TLB_X | TLB_U | TLB_C);

    XlateResult r_xlate = xlate(d, 0x1000'0001, ACC_READ, false, SZ_HALF,
                                false, false);
    check_bool("align.xlate_fault", r_xlate.fault, true);
    check_bool("align.xlate_o_align", r_xlate.align, true);
    commit_xlate(d);
    check("align.xlate_fstat",
          sys_read(d, SYSREG_MMU_FSTAT) & 0xF, (uint32_t)FAULT_ALIGN);

    // Byte access is always aligned.
    XlateResult r_byte = xlate(d, 0x1000'0003, ACC_READ, false, SZ_BYTE,
                               false, false);
    check_bool("align.byte_never_faults", r_byte.fault, false);
    check_bool("align.byte_o_align_off", r_byte.align, false);
    commit_xlate(d);
}

static void test_force_bypass_blocks_fault_latching(Vmmu* d) {
    printf("── i_force_bypass=1 must NOT overwrite previously-latched fault info ──\n");
    reset(d);
    mmu_enable(d, 0);

    // Trigger a TLB miss → fault info latched.
    xlate(d, 0x1234'5000, ACC_READ, true, SZ_WORD, false, false);
    commit_xlate(d);
    uint32_t faddr_before = sys_read(d, SYSREG_MMU_FADDR);
    uint32_t fstat_before = sys_read(d, SYSREG_MMU_FSTAT);
    check("ovw.faddr_latched", faddr_before, 0x12345000u);

    // Now simulate a vector fetch: force_bypass=1, different vaddr.
    // Fault info must NOT change even if conditions would otherwise
    // overwrite (e.g. a misaligned vector address — not legal in
    // practice, but tests the gating).
    xlate(d, 0x0000'0001, ACC_READ, false, SZ_HALF,
          /*force_bypass=*/true, false);
    commit_xlate(d);

    check("ovw.faddr_unchanged",
          sys_read(d, SYSREG_MMU_FADDR), faddr_before);
    check("ovw.fstat_unchanged",
          sys_read(d, SYSREG_MMU_FSTAT), fstat_before);
}

static void test_bus_fault_latched(Vmmu* d) {
    printf("── i_bus_fault latched into FSTAT ──\n");
    reset(d);
    mmu_enable(d, 0);

    // Populate so the TLB itself doesn't fault.
    write_main_tlb(d, 0x77777, 0x88888, 0,
                   TLB_V | TLB_R | TLB_W | TLB_X | TLB_U | TLB_C);

    // Now signal bus fault on a translated access.
    xlate(d, 0x77777'100, ACC_READ, false, SZ_WORD,
          /*force_bypass=*/false, /*bus_fault=*/true);
    commit_xlate(d);

    check("bus.faddr", sys_read(d, SYSREG_MMU_FADDR), 0x77777100u);
    check("bus.fstat_class",
          sys_read(d, SYSREG_MMU_FSTAT) & 0xF, (uint32_t)FAULT_BUS);
}

static void test_fault_priority_align_over_miss(Vmmu* d) {
    printf("── Fault priority: alignment beats TLB miss ──\n");
    reset(d);
    mmu_enable(d, 0);

    // M=1, no entries → would be TLB miss; but vaddr is misaligned,
    // so alignment-fault should win.
    xlate(d, 0xDEAD'B002, ACC_READ, false, SZ_WORD, false, false);
    commit_xlate(d);

    check("prio.align_wins_class",
          sys_read(d, SYSREG_MMU_FSTAT) & 0xF, (uint32_t)FAULT_ALIGN);
}

static void test_sysreg_routing_through_to_tlb(Vmmu* d) {
    printf("── Sysreg regs 3+ pass through to tlb_unit ──\n");
    reset(d);

    // TLB_IDX is the only TLB-side register that reads back the
    // staged value directly (tlb_vpn_reg is staging-only — reads of
    // TLB_VPN return the *bank entry* at the current IDX, which is
    // uninitialized DPRAM at reset).  Use TLB_IDX as the round-trip.
    uint32_t idx_val = 0x47;  // arbitrary, distinctly non-zero
    sys_write(d, SYSREG_MMU_TLB_IDX, idx_val);
    check("route.tlb_idx_readback",
          sys_read(d, SYSREG_MMU_TLB_IDX), idx_val);

    // MMU's own regs unaffected.
    check("route.mmucr_separate", sys_read(d, SYSREG_MMU_CR), 0u);
}

static void test_no_req_no_latch(Vmmu* d) {
    printf("── i_req=0: alignment-fault / TLB-miss paths don't latch ──\n");
    reset(d);
    mmu_enable(d, 0);

    // Drive a misaligned address but with i_req=0.  Fault must NOT
    // latch.  We can't use xlate() (which always sets i_req=1), so
    // drive manually.
    d->i_vaddr = 0xBAD0'C0DE;
    d->i_access_type = ACC_READ;
    d->i_user_mode = 0;
    d->i_req = 0;
    d->i_force_bypass = 0;
    d->i_mem_size = SZ_WORD;
    d->i_bus_fault = 0;
    tick(d);

    check("no_req.faddr_stays_zero", sys_read(d, SYSREG_MMU_FADDR), 0u);
    check("no_req.fstat_stays_zero", sys_read(d, SYSREG_MMU_FSTAT), 0u);
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vmmu* d = new Vmmu;

    printf("── MMU Unit Tests ──\n\n");

    test_reset_state(d);
    test_bypass_identity(d);
    test_force_bypass_overrides_enable(d);
    test_mmu_enable_with_hit(d);
    test_tlb_miss_latches_fault(d);
    test_alignment_fault_regardless_of_mmu(d);
    test_force_bypass_blocks_fault_latching(d);
    test_bus_fault_latched(d);
    test_fault_priority_align_over_miss(d);
    test_sysreg_routing_through_to_tlb(d);
    test_no_req_no_latch(d);

    printf("\nmmu: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return errors ? 1 : 0;
}
