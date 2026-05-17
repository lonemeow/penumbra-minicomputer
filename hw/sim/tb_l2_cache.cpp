// Verilator testbench for l2_cache (phase 1).
//
// Drives the L2's CPU-side ports and mocks the memory-side bus;
// covers the documented contract from `hw/rtl/soc/l2_cache.sv`
// and `doc/internals/l2-cache.md`:
//
//   - Sysreg INFO returns non-zero with correct geometry fields
//   - CTRL.enable=0 default → all accesses pass-through with no
//     2-cycle latency
//   - CTRL.enable=1 → cacheable reads hit/miss correctly; misses
//     allocate, install, and become hits on the next access
//   - Cached writes pass through to memory AND invalidate any
//     cached copy of the line
//   - Uncacheable accesses skip the cache pipeline regardless of
//     CTRL.enable
//   - INVAL_ALL drops all valid bits and asserts STATUS.busy
//     while the walker runs
//
// The "memory" mock is the same shape as `tb_cpu_bus_arbiter.cpp`
// uses: latency-counted busy + last-rdata-presented.

#include <cstdio>
#include <cstdint>
#include "Vl2_cache.h"

static int errors = 0, tests = 0;

static void tick(Vl2_cache* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vl2_cache* d) {
    d->i_rst = 1;
    d->i_addr = 0; d->i_wdata = 0; d->i_byte_en = 0;
    d->i_re = 0; d->i_we = 0; d->i_cacheable = 0;
    d->i_mem_rdata = 0; d->i_mem_busy = 0;
    d->i_sys_reg = 0; d->i_sys_wdata = 0; d->i_sys_we = 0;
    tick(d); tick(d);
    d->i_rst = 0;
}

static void check(const char* name, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n",
               name, got, exp);
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

// Memory mock: serves one outstanding read at a time with a
// configurable latency, returning a function of the address.
struct MemMock {
    int      busy_remaining = 0;
    uint32_t pending_rdata  = 0;
    bool     have_request   = false;
    uint32_t served_count   = 0;

    void clock_edge(Vl2_cache* d, int latency,
                    uint32_t (*rdata_fn)(uint32_t)) {
        if (!have_request && (d->o_mem_re || d->o_mem_we)) {
            have_request   = true;
            busy_remaining = latency;
            pending_rdata  = rdata_fn(d->o_mem_addr);
        }
        if (have_request) {
            if (busy_remaining > 0) {
                d->i_mem_busy  = 1;
                d->i_mem_rdata = 0xDEAD'BEEFu;
                busy_remaining--;
            }
            if (busy_remaining == 0) {
                d->i_mem_busy  = 0;
                d->i_mem_rdata = pending_rdata;
                have_request   = false;
                served_count++;
            }
        } else {
            d->i_mem_busy  = 0;
            d->i_mem_rdata = 0;
        }
    }
};

static void tick_with_mem(Vl2_cache* d, MemMock& mem, int lat,
                          uint32_t (*rdata_fn)(uint32_t)) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
    mem.clock_edge(d, lat, rdata_fn);
    d->eval();
}

// Default rdata function: data = ~addr (cheap, distinct per word).
static uint32_t rdata_addr_complement(uint32_t a) { return ~a; }

static void wrsys(Vl2_cache* d, uint8_t reg, uint32_t val) {
    d->i_sys_reg   = reg;
    d->i_sys_wdata = val;
    d->i_sys_we    = 1;
    d->eval();
    tick(d);
    d->i_sys_we    = 0;
    d->eval();
}

static uint32_t rdsys(Vl2_cache* d, uint8_t reg) {
    d->i_sys_reg = reg;
    d->eval();
    return d->o_sys_rdata;
}

// ══════════════════════════════════════════════════════════════
// Tests
// ══════════════════════════════════════════════════════════════

static void test_info_register(Vl2_cache* d) {
    printf("── INFO returns geometry, non-zero ──\n");
    reset(d);
    uint32_t info = rdsys(d, 0);
    // Expected layout: {type[7:0], ways[3:0], sets[15:0], line_words[3:0]}
    uint8_t  type        = (info >> 24) & 0xFF;
    uint8_t  ways        = (info >> 20) & 0x0F;
    uint16_t sets        = (info >> 4)  & 0xFFFF;
    uint8_t  line_words  =  info        & 0x0F;
    check("info.type_l2_unified",  type,       1u);
    check("info.ways_4",           ways,       4u);
    check("info.sets_1024",        sets,       1024u);
    check("info.line_words_4",     line_words, 4u);
}

static void test_passthrough_when_disabled(Vl2_cache* d) {
    printf("── CTRL.enable=0: every access is uncached pass-through ──\n");
    reset(d);
    MemMock mem;
    d->i_addr      = 0x1000;
    d->i_cacheable = 1;        // but L2 is disabled, so pass-through
    d->i_re        = 1;
    d->eval();
    check_bool("disabled.o_mem_re_immediate", d->o_mem_re, true);
    // Drive forward until the mock has served at least one transaction.
    // (Mock-side i_mem_busy is registered, so the L2's o_busy lags the
    // arrival by 1 tick on the pass-through path; we drive transactions
    // to completion rather than chasing the busy edge directly.)
    uint32_t before = mem.served_count;
    int safety = 0;
    while (mem.served_count == before && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check("disabled.rdata", d->o_rdata, rdata_addr_complement(0x1000));
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
}

static void test_cached_read_miss_then_hit(Vl2_cache* d) {
    printf("── Cached read: first miss installs, second hit serves from L2 ──\n");
    reset(d);
    wrsys(d, 1, 1);   // CTRL.enable = 1
    MemMock mem;

    // ── First access: miss → fill → eventually serve ──
    d->i_addr      = 0x4000;     // line base
    d->i_cacheable = 1;
    d->i_re        = 1;
    d->eval();
    int safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check("miss.rdata_word0", d->o_rdata,
          rdata_addr_complement(0x4000));
    uint32_t miss_mem_accesses = mem.served_count;
    check("miss.served_4_words", miss_mem_accesses, 4u);

    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);

    // ── Second access to same line: should hit, no memory traffic ──
    d->i_addr = 0x4004;          // word 1 of same line
    d->i_re   = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check("hit.rdata_word1", d->o_rdata,
          rdata_addr_complement(0x4004));
    check("hit.no_extra_memory_traffic",
          mem.served_count, miss_mem_accesses);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
}

static uint32_t rdata_zero(uint32_t a) { (void)a; return 0; }

static void test_write_invalidate(Vl2_cache* d) {
    printf("── Cached write hit invalidates L2 line ──\n");
    reset(d);
    wrsys(d, 1, 1);
    MemMock mem;

    // Prime the cache with a read to install line 0x5000.
    d->i_addr      = 0x5000;
    d->i_cacheable = 1;
    d->i_re        = 1;
    d->eval();
    int safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    uint32_t mem_after_install = mem.served_count;

    // Now write to the same line.  Should pass through to memory
    // AND invalidate L2's copy.
    d->i_addr      = 0x5004;
    d->i_wdata     = 0xCAFEBABEu;
    d->i_byte_en   = 0xF;
    d->i_cacheable = 1;
    d->i_we        = 1;
    d->eval();
    check_bool("write.o_mem_we_immediate", d->o_mem_we, true);
    uint32_t served_before_write = mem.served_count;
    safety = 0;
    while (mem.served_count == served_before_write && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_zero);
    d->i_we = 0; tick_with_mem(d, mem, 0, rdata_zero);
    uint32_t mem_after_write = mem.served_count;
    check("write.one_memory_access", mem_after_write,
          mem_after_install + 1);

    // Now read the same line — should miss (invalidate worked) and
    // produce a fresh 4-word fill from memory.
    d->i_addr = 0x5000;
    d->i_re   = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check("invalidated.refilled", mem.served_count,
          mem_after_write + 4);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
}

static void test_inval_all(Vl2_cache* d) {
    printf("── INVAL_ALL drops every line ──\n");
    reset(d);
    wrsys(d, 1, 1);
    MemMock mem;

    // Install line 0x6000.
    d->i_addr      = 0x6000;
    d->i_cacheable = 1;
    d->i_re        = 1;
    d->eval();
    int safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    uint32_t mem_after_install = mem.served_count;

    // Trigger INVAL_ALL.  wrsys leaves inval_all_req=1 registered;
    // one extra tick is needed for the FSM to sample it and
    // transition to S_INVAL_ALL.
    wrsys(d, 2, 0);
    tick_with_mem(d, mem, 2, rdata_addr_complement);

    // Walker runs.  Poll STATUS.busy via the sysreg interface.
    // Bound it generously — 1024 sets + transition cycles.
    int walk_cycles = 0;
    while ((rdsys(d, 6) & 1) && walk_cycles++ < 2000)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check_bool("inval_all.walker_completed",
               (rdsys(d, 6) & 1) == 0, true);

    // Re-read line 0x6000 — must miss (refill from memory).
    d->i_addr = 0x6000;
    d->i_re   = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check("inval_all.refill_happened", mem.served_count,
          mem_after_install + 4);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
}

static void test_uncached_skips_cache(Vl2_cache* d) {
    printf("── Uncacheable access skips the cache pipeline ──\n");
    reset(d);
    wrsys(d, 1, 1);
    MemMock mem;

    // Uncacheable read.  Drives o_mem_re immediately, no 2-cycle
    // pipeline penalty.
    d->i_addr      = 0xFF000000u;
    d->i_cacheable = 0;
    d->i_re        = 1;
    d->eval();
    check_bool("uncached.o_mem_re_immediate", d->o_mem_re, true);
    uint32_t before = mem.served_count;
    int safety = 0;
    while (mem.served_count == before && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check("uncached.rdata", d->o_rdata,
          rdata_addr_complement(0xFF000000u));

    // A second uncached access to the same addr must still hit
    // memory (no cache install for uncached accesses).
    uint32_t served_before = mem.served_count;
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    d->i_re = 1; d->eval();
    safety = 0;
    while (mem.served_count == served_before && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check("uncached.no_caching", mem.served_count, served_before + 1);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vl2_cache* d = new Vl2_cache;

    printf("── L2 Cache Unit Tests ──\n\n");

    test_info_register(d);
    test_passthrough_when_disabled(d);
    test_cached_read_miss_then_hit(d);
    test_write_invalidate(d);
    test_inval_all(d);
    test_uncached_skips_cache(d);

    printf("\nl2_cache: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return errors ? 1 : 0;
}
