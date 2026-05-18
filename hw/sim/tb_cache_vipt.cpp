// Verilator testbench for the Penumbra VIPT cache
//
// Tests the VIPT cache_vipt module via cache_vipt_test wrapper
// (cache_vipt + simple_mem with READ_LATENCY=4, WRITE_LATENCY=2).
//
// Test wrapper parameters: NUM_SETS=16, LINE_WORDS=4, MEM_WORDS=1024
// Address breakdown (16 sets × 4-word lines):
//   [31:8]  tag       (24 bits, from i_paddr)
//   [ 7:4]  set index ( 4 bits, from i_vaddr)
//   [ 3:2]  word      ( 2 bits, from i_vaddr)
//   [ 1:0]  byte      (ignored)
//
// The wrapper drives i_vaddr = i_paddr from a single i_addr port,
// which is valid under the VIPT precondition (cache ≤ page size →
// low 12 bits agree).  Tests focus on the VIPT-vs-PIPT contract delta:
//
//   1. SYSREG_CACHE_INFO reports ADDRESSING=VIPT(1).
//   2. The combinational read-hit contract (cache_vipt SVA: hit |-> !o_busy).
//   3. Shadow-flop timing: write-hit data update lands one cycle later
//      than in the PIPT cache (per the header).
//   4. i_fault gating: faulting read-miss does NOT initiate a fill;
//      faulting write-hit does NOT update the cached data.
//
// Plus the baseline tests carried over from tb_cache: pass-through,
// miss→hit, write-through, invalidate, tag conflict, multi-set,
// byte-granular writes.

#include <cstdio>
#include <cstdint>
#include "Vcache_vipt_test.h"

static int errors = 0, tests = 0;

static void tick(Vcache_vipt_test* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vcache_vipt_test* d) {
    d->i_rst = 1;
    d->i_addr = 0;
    d->i_wdata = 0;
    d->i_byte_en = 0xF;
    d->i_we = 0;
    d->i_re = 0;
    d->i_cacheable = 0;
    d->i_fault = 0;
    d->i_sys_reg = 0;
    d->i_sys_wdata = 0;
    d->i_sys_we = 0;
    d->i_dbg_mem_addr = 0;
    d->i_dbg_mem_wdata = 0;
    d->i_dbg_mem_we = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
}

static void mem_write(Vcache_vipt_test* d, uint32_t addr, uint32_t data) {
    d->i_dbg_mem_addr = addr;
    d->i_dbg_mem_wdata = data;
    d->i_dbg_mem_we = 1;
    for (int i = 0; i < 10; i++)
        tick(d);
    d->i_dbg_mem_we = 0;
    for (int i = 0; i < 10; i++)
        tick(d);
}

static void sys_write(Vcache_vipt_test* d, uint32_t reg, uint32_t data) {
    d->i_sys_reg = reg;
    d->i_sys_wdata = data;
    d->i_sys_we = 1;
    tick(d);
    d->i_sys_we = 0;
}

static uint32_t sys_read(Vcache_vipt_test* d, uint32_t reg) {
    d->i_sys_reg = reg;
    d->eval();
    return d->o_sys_rdata;
}

static void cache_enable(Vcache_vipt_test* d) {
    sys_write(d, 1, 1);  // SYSREG_CACHE_CTRL = 1
}

static void cache_inval(Vcache_vipt_test* d) {
    sys_write(d, 2, 0);  // SYSREG_CACHE_INVAL_ALL = 2
    // Two ticks: inval_req latches on cycle N, clears valid[] on N+1.
    // A second tick lets the cleared state propagate into combinational
    // hit detection for the next access.
    tick(d);
    tick(d);
}

struct ReadResult {
    uint32_t data;
    int busy_cycles;
};

static ReadResult cache_read(Vcache_vipt_test* d, uint32_t addr,
                             bool cacheable, bool fault = false) {
    ReadResult r;
    r.busy_cycles = 0;

    d->i_addr = addr;
    d->i_re = 1;
    d->i_we = 0;
    d->i_cacheable = cacheable ? 1 : 0;
    d->i_fault = fault ? 1 : 0;
    d->i_byte_en = 0xF;
    d->eval();

    while (d->o_busy) {
        r.busy_cycles++;
        tick(d);
        d->eval();
        if (r.busy_cycles > 200) {
            printf("  STUCK: read at 0x%08X busy for >200 cycles\n", addr);
            break;
        }
    }

    r.data = d->o_rdata;
    d->i_re = 0;
    d->i_fault = 0;
    tick(d);
    return r;
}

static int cache_write(Vcache_vipt_test* d, uint32_t addr, uint32_t data,
                       uint8_t byte_en, bool cacheable, bool fault = false) {
    int busy_cycles = 0;

    d->i_addr = addr;
    d->i_wdata = data;
    d->i_we = 1;
    d->i_re = 0;
    d->i_cacheable = cacheable ? 1 : 0;
    d->i_fault = fault ? 1 : 0;
    d->i_byte_en = byte_en;
    d->eval();

    while (d->o_busy) {
        busy_cycles++;
        tick(d);
        d->eval();
        if (busy_cycles > 200) {
            printf("  STUCK: write at 0x%08X busy for >200 cycles\n", addr);
            break;
        }
    }

    d->i_we = 0;
    d->i_fault = 0;
    // Write-hit data update lands one cycle later than in PIPT (shadow
    // flops); give it a tick to settle before the next access.
    tick(d);
    return busy_cycles;
}

static uint32_t mem_read(Vcache_vipt_test* d, uint32_t addr) {
    ReadResult r = cache_read(d, addr, false);
    return r.data;
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

static void test_sysreg_info(Vcache_vipt_test* d) {
    // Unified cache INFO layout (see penumbra_pkg.sv):
    //   [5:0]   line_words
    //   [20:6]  num_sets
    //   [25:21] num_ways
    //   [27:26] addressing  (1 = VIPT)
    //   [28]    write_back  (0 = WT)
    //   [29]    write_alloc (0 = WnA)
    uint32_t info = sys_read(d, 0);

    uint32_t line_words = info & 0x3F;
    uint32_t num_sets   = (info >> 6) & 0x7FFF;
    uint32_t num_ways   = (info >> 21) & 0x1F;
    uint32_t addressing = (info >> 26) & 0x3;
    uint32_t write_back = (info >> 28) & 0x1;
    uint32_t write_alloc = (info >> 29) & 0x1;

    check("info.line_words",  line_words, 4);
    check("info.num_sets",    num_sets,   16);
    check("info.num_ways",    num_ways,   1);
    check("info.addressing",  addressing, 1);  // VIPT
    check("info.write_back",  write_back, 0);
    check("info.write_alloc", write_alloc, 0);
}

static void test_disabled_passthru(Vcache_vipt_test* d) {
    reset(d);
    mem_write(d, 0x100, 0xDEAD'BEEF);

    ReadResult r = cache_read(d, 0x100, true);
    check("disabled_read.data", r.data, 0xDEAD'BEEF);
    check_bool("disabled_read.busy", r.busy_cycles > 0, true);

    int wc = cache_write(d, 0x200, 0xCAFE'BABE, 0xF, true);
    check_bool("disabled_write.busy", wc > 0, true);

    uint32_t mval = mem_read(d, 0x200);
    check("disabled_write.mem", mval, 0xCAFE'BABE);
}

static void test_uncacheable_passthru(Vcache_vipt_test* d) {
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0x1234'5678);
    ReadResult r = cache_read(d, 0x100, false);
    check("uncacheable_read.data", r.data, 0x1234'5678);
    check_bool("uncacheable_read.busy", r.busy_cycles > 0, true);
}

static void test_read_miss_then_hit(Vcache_vipt_test* d) {
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0xAAAA'0000);
    mem_write(d, 0x104, 0xAAAA'0001);
    mem_write(d, 0x108, 0xAAAA'0002);
    mem_write(d, 0x10C, 0xAAAA'0003);

    ReadResult r1 = cache_read(d, 0x100, true);
    check("miss.data", r1.data, 0xAAAA'0000);
    check_bool("miss.was_slow", r1.busy_cycles > 0, true);
    int miss_cycles = r1.busy_cycles;

    ReadResult r2 = cache_read(d, 0x100, true);
    check("hit.data", r2.data, 0xAAAA'0000);
    // VIPT contract: combinational hit completes in zero busy cycles.
    // The cache_vipt SVA `hit |-> !o_busy` covers this on every cycle;
    // this test is the explicit functional check.
    check("hit.busy_cycles", r2.busy_cycles, 0);

    ReadResult r3 = cache_read(d, 0x104, true);
    check("hit_w1.data", r3.data, 0xAAAA'0001);
    check("hit_w1.busy_cycles", r3.busy_cycles, 0);

    ReadResult r4 = cache_read(d, 0x108, true);
    check("hit_w2.data", r4.data, 0xAAAA'0002);
    check("hit_w2.busy_cycles", r4.busy_cycles, 0);

    ReadResult r5 = cache_read(d, 0x10C, true);
    check("hit_w3.data", r5.data, 0xAAAA'0003);
    check("hit_w3.busy_cycles", r5.busy_cycles, 0);

    printf("    (miss=%d cycles, hit=0 cycles)\n", miss_cycles);
}

static void test_write_hit(Vcache_vipt_test* d) {
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0x1111'1111);
    mem_write(d, 0x104, 0x2222'2222);
    mem_write(d, 0x108, 0x3333'3333);
    mem_write(d, 0x10C, 0x4444'4444);
    cache_read(d, 0x100, true);  // Fill

    cache_write(d, 0x104, 0xFEED'9999, 0xF, true);

    ReadResult r = cache_read(d, 0x104, true);
    check("write_hit.cache", r.data, 0xFEED'9999);
    check("write_hit.cache_cycles", r.busy_cycles, 0);

    uint32_t mval = mem_read(d, 0x104);
    check("write_hit.mem", mval, 0xFEED'9999);

    ReadResult r2 = cache_read(d, 0x100, true);
    check("write_hit.other_word", r2.data, 0x1111'1111);
}

static void test_write_miss(Vcache_vipt_test* d) {
    reset(d);
    cache_enable(d);

    cache_write(d, 0x300, 0xBEEF'CAFE, 0xF, true);
    uint32_t mval = mem_read(d, 0x300);
    check("write_miss.mem", mval, 0xBEEF'CAFE);

    ReadResult r = cache_read(d, 0x300, true);
    check("write_miss.read_data", r.data, 0xBEEF'CAFE);
    check_bool("write_miss.read_was_slow", r.busy_cycles > 0, true);
}

static void test_invalidate(Vcache_vipt_test* d) {
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0xAAAA'BBBB);
    mem_write(d, 0x104, 0xCCCC'DDDD);
    mem_write(d, 0x108, 0xEEEE'FFFF);
    mem_write(d, 0x10C, 0x0000'1111);
    cache_read(d, 0x100, true);  // Fill

    ReadResult r1 = cache_read(d, 0x100, true);
    check("inval.before", r1.busy_cycles, 0);

    cache_inval(d);

    ReadResult r2 = cache_read(d, 0x100, true);
    check("inval.after_data", r2.data, 0xAAAA'BBBB);
    check_bool("inval.after_slow", r2.busy_cycles > 0, true);
}

static void test_tag_conflict(Vcache_vipt_test* d) {
    // 0x100 and 0x200 both map to set 0 with different tags.
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0xAAAA'AAAA);
    mem_write(d, 0x104, 0xAAAA'AAAB);
    mem_write(d, 0x108, 0xAAAA'AAAC);
    mem_write(d, 0x10C, 0xAAAA'AAAD);
    cache_read(d, 0x100, true);
    ReadResult rA = cache_read(d, 0x100, true);
    check("conflict.A_hit", rA.busy_cycles, 0);

    mem_write(d, 0x200, 0xBBBB'BBBB);
    mem_write(d, 0x204, 0xBBBB'BBBC);
    mem_write(d, 0x208, 0xBBBB'BBBD);
    mem_write(d, 0x20C, 0xBBBB'BBBE);
    cache_read(d, 0x200, true);
    ReadResult rB = cache_read(d, 0x200, true);
    check("conflict.B_hit", rB.busy_cycles, 0);
    check("conflict.B_data", rB.data, 0xBBBB'BBBB);

    ReadResult rA2 = cache_read(d, 0x100, true);
    check("conflict.A_evicted_data", rA2.data, 0xAAAA'AAAA);
    check_bool("conflict.A_evicted_slow", rA2.busy_cycles > 0, true);
}

static void test_multi_set(Vcache_vipt_test* d) {
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0x1111'1111);
    mem_write(d, 0x104, 0x1111'1112);
    mem_write(d, 0x108, 0x1111'1113);
    mem_write(d, 0x10C, 0x1111'1114);
    mem_write(d, 0x110, 0x2222'2222);
    mem_write(d, 0x114, 0x2222'2223);
    mem_write(d, 0x118, 0x2222'2224);
    mem_write(d, 0x11C, 0x2222'2225);

    cache_read(d, 0x100, true);
    cache_read(d, 0x110, true);

    ReadResult r1 = cache_read(d, 0x100, true);
    check("multi_set.s0_data", r1.data, 0x1111'1111);
    check("multi_set.s0_hit", r1.busy_cycles, 0);

    ReadResult r2 = cache_read(d, 0x110, true);
    check("multi_set.s1_data", r2.data, 0x2222'2222);
    check("multi_set.s1_hit", r2.busy_cycles, 0);
}

static void test_byte_write(Vcache_vipt_test* d) {
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0xAABBCCDD);
    mem_write(d, 0x104, 0);
    mem_write(d, 0x108, 0);
    mem_write(d, 0x10C, 0);
    cache_read(d, 0x100, true);  // Fill

    cache_write(d, 0x100, 0x0000FF00, 0b0010, true);

    ReadResult r = cache_read(d, 0x100, true);
    check("byte_write.cache", r.data, 0xAABBFFDD);
    check("byte_write.hit", r.busy_cycles, 0);

    uint32_t mval = mem_read(d, 0x100);
    check("byte_write.mem", mval, 0xAABBFFDD);
}

// ── VIPT-specific contract: i_fault gating ─────────────────────────
//
// The header says: i_fault gates side-effecting state changes
// (fill entry on a read miss, cache data update on a write hit).
// The hit *read* path runs unconditionally (that's the whole point of
// VIPT — let cache RAM and TLB run in parallel; bus output suppression
// for the pass-through case is enforced by the CPU externally).
//
// On a faulting cached read miss, the cache holds o_busy=1 indefinitely
// (the FSM refuses to enter S_FILL while i_fault_q is set).  The CPU is
// expected to abandon the request via fault dispatch; the cache never
// observes a "completion" itself.  Tests below drive faulting accesses
// for a bounded number of cycles and verify the no-side-effect property
// directly, rather than looping on o_busy.

// Drive a request for `cycles` ticks with i_re/i_we/i_fault as given,
// then release. Returns immediately — does NOT wait for o_busy to drop.
static void drive_request(Vcache_vipt_test* d, uint32_t addr, uint32_t wdata,
                          uint8_t byte_en, bool cacheable, bool fault,
                          bool re, bool we, int cycles) {
    d->i_addr = addr;
    d->i_wdata = wdata;
    d->i_byte_en = byte_en;
    d->i_cacheable = cacheable ? 1 : 0;
    d->i_fault = fault ? 1 : 0;
    d->i_re = re ? 1 : 0;
    d->i_we = we ? 1 : 0;
    for (int i = 0; i < cycles; i++)
        tick(d);
    d->i_re = 0;
    d->i_we = 0;
    d->i_fault = 0;
    // Allow the cache shadow flops and arbiter FSM to drain.
    for (int i = 0; i < 8; i++)
        tick(d);
}

static void test_fault_blocks_fill_on_miss(Vcache_vipt_test* d) {
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0xCAFE'CAFE);
    mem_write(d, 0x104, 0xCAFE'CAFE);
    mem_write(d, 0x108, 0xCAFE'CAFE);
    mem_write(d, 0x10C, 0xCAFE'CAFE);

    // Faulting read miss: cache must NOT initiate a fill.  Drive
    // the faulting request for 20 cycles — enough that a non-faulting
    // miss would have completed the burst (4 fill words × ~4 cycles
    // each).  No fill should happen, no bus traffic should commit
    // the cache line.
    drive_request(d, 0x100, 0, 0xF, /*cacheable=*/true,
                  /*fault=*/true, /*re=*/true, /*we=*/false,
                  /*cycles=*/20);

    // Now do a clean non-faulting read at the same address.  If the
    // earlier faulting access had erroneously filled the line, this
    // would be a hit (busy_cycles = 0).  The contract says it must
    // be a miss.
    ReadResult r2 = cache_read(d, 0x100, true, /*fault=*/false);
    check("fault_miss.no_fill.data", r2.data, 0xCAFE'CAFE);
    check_bool("fault_miss.no_fill.was_slow", r2.busy_cycles > 0, true);
}

static void test_fault_blocks_write_hit_update(Vcache_vipt_test* d) {
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0x1010'1010);
    mem_write(d, 0x104, 0x2020'2020);
    mem_write(d, 0x108, 0x3030'3030);
    mem_write(d, 0x10C, 0x4040'4040);

    // Fill the line cleanly.
    cache_read(d, 0x100, true);

    // Faulting write hit: cache data array must NOT update.  This
    // test pins the *cache-side* contract; external bus suppression
    // on fault is the CPU's job and is exercised by integration
    // tests `test_tlb_cow.s` / `test_tlb_cow_inv.s`.
    //
    // Write-hit busy = i_mem_busy (pass-through), so a regular
    // cache_write completes normally; the gating is on the cache
    // data array update.
    cache_write(d, 0x104, 0xDEAD'BEEF, 0xF, true, /*fault=*/true);

    ReadResult r = cache_read(d, 0x104, true);
    // Cache hit (line still resident from the earlier fill); the
    // stored data must be the pre-fault value, not 0xDEAD'BEEF.
    check("fault_write.cache_unchanged", r.data, 0x2020'2020);
    check("fault_write.was_hit", r.busy_cycles, 0);
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vcache_vipt_test* d = new Vcache_vipt_test;

    printf("── VIPT Cache Unit Tests ──\n\n");

    reset(d);

    test_sysreg_info(d);
    test_disabled_passthru(d);
    test_uncacheable_passthru(d);
    test_read_miss_then_hit(d);
    test_write_hit(d);
    test_write_miss(d);
    test_invalidate(d);
    test_tag_conflict(d);
    test_multi_set(d);
    test_byte_write(d);
    test_fault_blocks_fill_on_miss(d);
    test_fault_blocks_write_hit_update(d);

    printf("\ncache_vipt: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return errors ? 1 : 0;
}
