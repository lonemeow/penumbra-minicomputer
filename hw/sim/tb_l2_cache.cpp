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
//   - After WRSYS CACHE_INVAL_ALL the next memory access (cached
//     or uncached MMIO) stalls automatically until the walker
//     finishes — software does not need a STATUS.busy poll loop
//   - Post-reset auto-INVAL walks all sets to initialise valid
//     BRAMs (undefined at power-on on real HW); the cache stays
//     in pass-through mode until `ready` asserts
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
    // Post-reset auto-INVAL walks all NUM_SETS=1024 sets clearing
    // valid bits in BRAM (which power up undefined on real HW).
    // Cached operations are gated on STATUS.busy=0 / `ready` flag;
    // tick past the walk so subsequent tests start with a ready
    // cache.  Matches what real software does after boot: poll
    // STATUS.busy before enabling/using L2.
    for (int i = 0; i < 1100; i++) tick(d);
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
    bool     req_was_write  = false;
    uint32_t served_count   = 0;
    int      violations     = 0;

    void clock_edge(Vl2_cache* d, int latency,
                    uint32_t (*rdata_fn)(uint32_t)) {
        // Contract enforcement: once we've latched a request, the
        // master must keep re/we asserted until busy drops.  See
        // doc/hardware/bus-protocol.md "Sync-Bus Mapping".
        if (have_request && busy_remaining > 0) {
            bool live = req_was_write ? (bool)d->o_mem_we
                                      : (bool)d->o_mem_re;
            if (!live) {
                fprintf(stderr,
                        "MemMock: contract violation — master dropped %s "
                        "mid-access (busy_remaining=%d)\n",
                        req_was_write ? "o_mem_we" : "o_mem_re",
                        busy_remaining);
                violations++;
                errors++;
            }
        }
        if (!have_request && (d->o_mem_re || d->o_mem_we)) {
            have_request   = true;
            req_was_write  = d->o_mem_we;
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
    // Unified cache INFO layout (see penumbra_pkg.sv):
    //   [5:0]   line_words
    //   [20:6]  num_sets
    //   [25:21] num_ways
    //   [27:26] addressing  (PIPT for L2)
    //   [28]    write_back  (0 — WT/WnA via write-invalidate-on-hit)
    //   [29]    write_alloc
    uint8_t  line_words  =  info        & 0x3F;
    uint16_t sets        = (info >>  6) & 0x7FFF;
    uint8_t  ways        = (info >> 21) & 0x1F;
    uint8_t  addressing  = (info >> 26) & 0x03;
    uint8_t  write_back  = (info >> 28) & 0x01;
    uint8_t  write_alloc = (info >> 29) & 0x01;
    check("info.line_words_4",  line_words,  4u);
    check("info.sets_1024",     sets,        1024u);
    check("info.ways_4",        ways,        4u);
    check("info.addressing_pipt", addressing, 0u);
    check("info.write_back_off",  write_back, 0u);
    check("info.write_alloc_off", write_alloc, 0u);
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

static void test_write_updates_line(Vl2_cache* d) {
    printf("── Cached write hit updates L2 line (WT-WnA) ──\n");
    reset(d);
    wrsys(d, 1, 1);
    MemMock mem;

    // Prime the cache with a read to install line 0x5000.  After
    // this, L2 holds 0x5000-0x500F with data from
    // rdata_addr_complement().
    d->i_addr      = 0x5000;
    d->i_cacheable = 1;
    d->i_re        = 1;
    d->eval();
    int safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    uint32_t mem_after_install = mem.served_count;

    // Write to a word inside the cached line.  Under WT-WnA the
    // store passes through to memory (one downstream access) AND
    // the cached copy gets updated via byte-en — no invalidation.
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
    check("write.one_memory_access", mem.served_count,
          mem_after_install + 1);

    // Read the just-written word.  Should hit (line still cached)
    // and return the value we wrote, NOT the original fill data.
    d->i_addr = 0x5004;
    d->i_re   = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check("write.line_still_cached_no_refill", mem.served_count,
          mem_after_install + 1);
    check("write.read_returns_new_value", d->o_rdata, 0xCAFEBABEu);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);

    // Read an unwritten word in the same line.  Still a hit,
    // returns the original fill data.
    d->i_addr = 0x5008;
    d->i_re   = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check("write.other_word_unchanged",
          d->o_rdata, rdata_addr_complement(0x5008));
    check("write.still_no_refill", mem.served_count,
          mem_after_install + 1);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);

    // Partial-byte write: only one byte enable.  The other bytes
    // of the word must keep their old values.
    d->i_addr      = 0x5008;
    d->i_wdata     = 0xDEAD'BEEFu;
    d->i_byte_en   = 0x1;          // only byte 0
    d->i_cacheable = 1;
    d->i_we        = 1;
    d->eval();
    uint32_t served_before_partial = mem.served_count;
    safety = 0;
    while (mem.served_count == served_before_partial && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_zero);
    d->i_we = 0; tick_with_mem(d, mem, 0, rdata_zero);

    d->i_addr = 0x5008;
    d->i_re   = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    uint32_t orig    = rdata_addr_complement(0x5008);
    uint32_t expect  = (orig & 0xFFFFFF00u) | (0xDEAD'BEEFu & 0xFFu);
    check("write.byte_en_partial_update", d->o_rdata, expect);
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

static void test_inval_all_implicit_fence(Vl2_cache* d) {
    printf("── INVAL_ALL implicit fence: next access stalls until done ──\n");
    reset(d);
    wrsys(d, 1, 1);  // CTRL.enable = 1
    MemMock mem;

    // Trigger INVAL_ALL.
    wrsys(d, 2, 0);
    tick_with_mem(d, mem, 2, rdata_addr_complement);

    // Immediately issue a cached read — without polling STATUS.busy.
    // The L2 must hold o_busy high until the walker finishes; the
    // CPU's stall mechanism then naturally waits.  This is the
    // "implicit fence" guarantee: software does not need a poll
    // loop after WRSYS CACHE_INVAL_ALL.
    d->i_addr      = 0x8000;
    d->i_cacheable = 1;
    d->i_re        = 1;
    d->eval();
    check_bool("fence.cached_stalls_during_walk", d->o_busy, true);

    // Also verify uncached MMIO would stall (implicit fence covers it).
    d->i_re = 0;
    d->i_we = 1;
    d->i_cacheable = 0;
    d->i_addr = 0xFF000000;
    d->eval();
    check_bool("fence.uncached_stalls_during_walk", d->o_busy, true);

    // Resume the cached read and let the walk finish.
    d->i_we = 0;
    d->i_re = 1;
    d->i_cacheable = 1;
    d->i_addr = 0x8000;
    d->eval();
    int safety = 0;
    while (d->o_busy && safety++ < 2000)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check_bool("fence.access_completed_after_walk",
               (rdsys(d, 6) & 1) == 0, true);
    check("fence.access_returned_correct_data", d->o_rdata,
          rdata_addr_complement(0x8000));
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

static void test_perfctrs(Vl2_cache* d) {
    printf("── Performance counters (regs 10-13) ──\n");
    reset(d);
    wrsys(d, 1, 1);                // CTRL.enable = 1
    MemMock mem;

    // Counters start at 0 after reset.
    check("perf.initial_read_hits",    rdsys(d, 10), 0u);
    check("perf.initial_read_misses",  rdsys(d, 11), 0u);
    check("perf.initial_write_hits",   rdsys(d, 12), 0u);
    check("perf.initial_write_misses", rdsys(d, 13), 0u);

    // First access to line 0x4000: miss → fill → re-serve.  Counts
    // as exactly one read_miss; the post-fill re-serve must NOT
    // also fire read_hit (that's the fill_reserve_pending suppression).
    d->i_addr      = 0x4000;
    d->i_cacheable = 1;
    d->i_re        = 1;
    d->eval();
    int safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    check("perf.miss_counted_once",      rdsys(d, 11), 1u);
    check("perf.no_phantom_hit_on_reserve", rdsys(d, 10), 0u);

    // Second access to same line, different word — must count as hit.
    // This is the case that broke under the content-match approach:
    // s1_addr matches the most recent fill_tag/fill_set, so naive
    // content matching would suppress this legitimate hit.
    d->i_addr = 0x4004;
    d->i_re   = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    check("perf.hit_after_fill_counted",   rdsys(d, 10), 1u);
    check("perf.miss_still_one",           rdsys(d, 11), 1u);

    // Third access to the same line — another hit.
    d->i_addr = 0x4008;
    d->i_re   = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    check("perf.second_hit_counted",       rdsys(d, 10), 2u);

    // Cached write that hits → write_hit counted; the L2 policy
    // drops the line, but for counter purposes "hit" is determined
    // by the tag check at the moment of access (the line WAS present).
    d->i_addr      = 0x400C;
    d->i_wdata     = 0x12345678u;
    d->i_byte_en   = 0xF;
    d->i_cacheable = 1;
    d->i_we        = 1;
    d->eval();
    uint32_t served_before = mem.served_count;
    safety = 0;
    while (mem.served_count == served_before && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_zero);
    d->i_we = 0; tick_with_mem(d, mem, 0, rdata_zero);
    check("perf.write_hit_counted",        rdsys(d, 12), 1u);
    check("perf.write_miss_still_zero",    rdsys(d, 13), 0u);

    // Cached write to a line that's not cached → write_miss counted.
    d->i_addr      = 0x9000;
    d->i_wdata     = 0xAABBCCDDu;
    d->i_byte_en   = 0xF;
    d->i_cacheable = 1;
    d->i_we        = 1;
    d->eval();
    served_before = mem.served_count;
    safety = 0;
    while (mem.served_count == served_before && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_zero);
    d->i_we = 0; tick_with_mem(d, mem, 0, rdata_zero);
    check("perf.write_miss_counted",       rdsys(d, 13), 1u);

    // Uncacheable read pass-through → no counter movement.
    uint32_t rh_before = rdsys(d, 10);
    uint32_t rm_before = rdsys(d, 11);
    d->i_addr      = 0xFF001000u;
    d->i_cacheable = 0;
    d->i_re        = 1;
    d->eval();
    served_before = mem.served_count;
    safety = 0;
    while (mem.served_count == served_before && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    check("perf.uncached_read_no_event_hits",   rdsys(d, 10), rh_before);
    check("perf.uncached_read_no_event_misses", rdsys(d, 11), rm_before);

    // Uncacheable write pass-through → no counter movement.
    uint32_t wh_before = rdsys(d, 12);
    uint32_t wm_before = rdsys(d, 13);
    d->i_addr      = 0xFF002000u;
    d->i_wdata     = 0xDEADBEEFu;
    d->i_we        = 1;
    d->eval();
    served_before = mem.served_count;
    safety = 0;
    while (mem.served_count == served_before && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_zero);
    d->i_we = 0; tick_with_mem(d, mem, 0, rdata_zero);
    check("perf.uncached_write_no_event_hits",   rdsys(d, 12), wh_before);
    check("perf.uncached_write_no_event_misses", rdsys(d, 13), wm_before);

    // Re-read line 0x4000 — under WT-WnA the earlier write to
    // 0x400C left the line resident in L2 (byte-en update, not
    // invalidation), so this is a HIT, not a refill miss.  This
    // is exactly the phase-1.5 win the upgrade was designed to
    // produce.
    uint32_t rm_before2 = rdsys(d, 11);
    uint32_t rh_before2 = rdsys(d, 10);
    d->i_addr      = 0x4000;
    d->i_cacheable = 1;     // previous uncached steps left this at 0
    d->i_re        = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    check("perf.write_did_not_evict_l2_line",
          rdsys(d, 11), rm_before2);
    check("perf.subsequent_read_hits_under_wtwna",
          rdsys(d, 10), rh_before2 + 1);
}

// ══════════════════════════════════════════════════════════════
// Correctness tests targeting the cache contract under WT-WnA.
// Focus is contract-observable behaviour — anything where a wrong
// answer or stale byte would propagate to the CPU.  Eviction
// policy / hit-rate tuning is deliberately not the target here.
// ══════════════════════════════════════════════════════════════

static void test_multi_set_independence(Vl2_cache* d) {
    printf("── Multi-set independence: distinct sets stay distinct ──\n");
    reset(d);
    wrsys(d, 1, 1);
    MemMock mem;

    // With LINE_BYTES=16 and NUM_SETS=1024, addr_set = addr[13:4].
    // Walk a spread of sets so that bugs in addr_set extraction or
    // data_idx packing surface as cross-contamination between sets.
    struct case_t { uint32_t addr; uint32_t value; };
    case_t cases[] = {
        {0x0040, 0xAABB'CCDDu},   // set 0x004
        {0x0050, 0x1122'3344u},   // set 0x005
        {0x0060, 0xDEAD'BEEFu},   // set 0x006
        {0x0070, 0xCAFE'BABEu},   // set 0x007
        {0x1000, 0x5A5A'5A5Au},   // set 0x100
        {0x4000, 0x5555'AAAAu},   // set 0x400
        {0x7FF0, 0x0123'4567u},   // set 0x7FF (last set)
    };
    int N = sizeof(cases) / sizeof(cases[0]);

    // Prime each line via a read so it lands in L2, then overwrite
    // with the test value via WT-WnA byte-en update.
    for (int i = 0; i < N; i++) {
        d->i_addr = cases[i].addr;
        d->i_cacheable = 1;
        d->i_re = 1;
        d->eval();
        int safety = 0;
        while (d->o_busy && safety++ < 100)
            tick_with_mem(d, mem, 2, rdata_addr_complement);
        d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);

        d->i_addr      = cases[i].addr;
        d->i_wdata     = cases[i].value;
        d->i_byte_en   = 0xF;
        d->i_we        = 1;
        d->eval();
        uint32_t before = mem.served_count;
        safety = 0;
        while (mem.served_count == before && safety++ < 20)
            tick_with_mem(d, mem, 2, rdata_zero);
        d->i_we = 0; tick_with_mem(d, mem, 0, rdata_zero);
    }

    // Read every line back; each must return exactly the value we
    // wrote, with no influence from any other write.
    for (int i = 0; i < N; i++) {
        d->i_addr = cases[i].addr;
        d->i_cacheable = 1;
        d->i_re = 1;
        d->eval();
        int safety = 0;
        while (d->o_busy && safety++ < 100)
            tick_with_mem(d, mem, 2, rdata_addr_complement);
        char name[64];
        snprintf(name, sizeof(name),
                 "multi_set.value_at_0x%04X", cases[i].addr);
        check(name, d->o_rdata, cases[i].value);
        d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    }
}

static void test_byte_en_accumulation(Vl2_cache* d) {
    printf("── Byte-en writes accumulate within a cached line ──\n");
    reset(d);
    wrsys(d, 1, 1);
    MemMock mem;

    // Helper to drive one cacheable write and let it drain.
    auto do_write = [&](uint32_t addr, uint32_t wdata, uint8_t be) {
        d->i_addr      = addr;
        d->i_wdata     = wdata;
        d->i_byte_en   = be;
        d->i_cacheable = 1;
        d->i_we        = 1;
        d->eval();
        uint32_t before = mem.served_count;
        int safety = 0;
        while (mem.served_count == before && safety++ < 20)
            tick_with_mem(d, mem, 2, rdata_zero);
        d->i_we = 0; tick_with_mem(d, mem, 0, rdata_zero);
    };
    auto do_read = [&](uint32_t addr) -> uint32_t {
        d->i_addr = addr;
        d->i_cacheable = 1;
        d->i_re = 1;
        d->eval();
        int safety = 0;
        while (d->o_busy && safety++ < 100)
            tick_with_mem(d, mem, 2, rdata_addr_complement);
        uint32_t val = d->o_rdata;
        d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
        return val;
    };

    // Prime line at 0x800.
    do_read(0x800);
    uint32_t orig = rdata_addr_complement(0x800);

    // Write byte 0 (LSB).  Other 3 bytes must keep `orig` values.
    do_write(0x800, 0xDEAD'BEEFu, 0x1);
    uint32_t step1 = (orig & 0xFFFF'FF00u) | (0xDEAD'BEEFu & 0x0000'00FFu);
    check("byte_en.lsb_only", do_read(0x800), step1);

    // Write byte 1 next.  Previous byte 0 must persist, bytes 2-3
    // still untouched.
    do_write(0x800, 0xCAFE'CAFEu, 0x2);
    uint32_t step2 = (step1 & 0xFFFF'00FFu) | (0xCAFE'CAFEu & 0x0000'FF00u);
    check("byte_en.lsb_then_byte1", do_read(0x800), step2);

    // Now the high halfword (bytes 2 and 3 together).  Previous
    // low-half writes must persist.
    do_write(0x800, 0xFEED'FACEu, 0xC);
    uint32_t step3 = (step2 & 0x0000'FFFFu) | (0xFEED'FACEu & 0xFFFF'0000u);
    check("byte_en.high_halfword", do_read(0x800), step3);

    // Full-word write overrides everything.
    do_write(0x800, 0x1234'5678u, 0xF);
    check("byte_en.full_word_override", do_read(0x800), 0x1234'5678u);

    // Low halfword only.  Upper half preserved from the full-word.
    do_write(0x800, 0xABCD'9876u, 0x3);
    uint32_t step5 = (0x1234'5678u & 0xFFFF'0000u)
                   | (0xABCD'9876u & 0x0000'FFFFu);
    check("byte_en.low_halfword", do_read(0x800), step5);

    // byte_en=0 must be a no-op (no bytes enabled).  Value
    // unchanged.
    do_write(0x800, 0xFFFF'FFFFu, 0x0);
    check("byte_en.zero_mask_is_noop", do_read(0x800), step5);
}

static void test_perfctrs_inval_all(Vl2_cache* d) {
    printf("── Perfctrs survive INVAL_ALL (free-running, not reset) ──\n");
    reset(d);
    wrsys(d, 1, 1);
    MemMock mem;

    // Generate some traffic so the counters are non-zero, giving
    // INVAL_ALL something to potentially clobber if it had a bug.
    d->i_addr = 0x4000;
    d->i_cacheable = 1;
    d->i_re = 1;
    d->eval();
    int safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);

    // Add a hit on the same line so read_hits is also non-zero.
    d->i_addr = 0x4004; d->i_re = 1; d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 20)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);

    uint32_t rh_before = rdsys(d, 10);
    uint32_t rm_before = rdsys(d, 11);
    check_bool("inval_all_perf.read_hits_nonzero_pre",
               rh_before > 0u, true);
    check_bool("inval_all_perf.read_misses_nonzero_pre",
               rm_before > 0u, true);

    // Issue INVAL_ALL and wait for the walker to finish.  No
    // bus traffic to the memory mock is expected during the walk
    // — invalidation is internal.
    wrsys(d, 2, 0);
    tick_with_mem(d, mem, 2, rdata_addr_complement);
    int walk_cycles = 0;
    while ((rdsys(d, 6) & 1) && walk_cycles++ < 2000)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check_bool("inval_all_perf.walker_completed",
               (rdsys(d, 6) & 1) == 0u, true);

    // Counters must be unchanged — INVAL_ALL is a maintenance op,
    // not a cache access, and the walker writes valid bits without
    // going through the stage-1 classification that fires events.
    check("inval_all_perf.read_hits_preserved",
          rdsys(d, 10), rh_before);
    check("inval_all_perf.read_misses_preserved",
          rdsys(d, 11), rm_before);
}

static void test_perfctrs_during_post_reset_walk(Vl2_cache* d) {
    printf("── Perfctrs do not fire during post-reset auto-INVAL ──\n");

    // Manual reset that does *not* skip past the auto-INVAL walk
    // (the standard reset() helper ticks 1100 cycles, which already
    // takes us past the walker).  We want to observe behaviour
    // while the walker is still running.
    d->i_rst = 1;
    d->i_addr = 0; d->i_wdata = 0; d->i_byte_en = 0;
    d->i_re = 0; d->i_we = 0; d->i_cacheable = 0;
    d->i_mem_rdata = 0; d->i_mem_busy = 0;
    d->i_sys_reg = 0; d->i_sys_wdata = 0; d->i_sys_we = 0;
    tick(d); tick(d);
    d->i_rst = 0;
    tick(d);

    // Walker should be active, all counters should be zero.
    check_bool("post_reset.walker_busy",
               (rdsys(d, 6) & 1) == 1u, true);
    check("post_reset.read_hits_zero",    rdsys(d, 10), 0u);
    check("post_reset.read_misses_zero",  rdsys(d, 11), 0u);
    check("post_reset.write_hits_zero",   rdsys(d, 12), 0u);
    check("post_reset.write_misses_zero", rdsys(d, 13), 0u);

    // Enable the cache and issue a cacheable access while the
    // walker is still running.  During the walker `ready=0`, so
    // `l2_active` returns false regardless of `i_cacheable` — the
    // access is demoted to pass-through.  It must NOT count as a
    // cache event because the cache pipeline never sees it.
    wrsys(d, 1, 1);
    MemMock mem;
    d->i_addr = 0x1000;
    d->i_cacheable = 1;
    d->i_re = 1;
    d->eval();

    // Drive the pass-through access to completion while STATUS.busy
    // is still asserted (the walker is still running).
    int safety = 0;
    while (d->o_busy && safety++ < 30)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check_bool("post_reset.passthrough_satisfied_during_walk",
               d->o_busy == 0u, true);
    check_bool("post_reset.walker_still_busy_after_passthrough",
               (rdsys(d, 6) & 1) == 1u, true);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);

    // Counters must still be zero — the access went through
    // pass-through, not the cache pipeline.
    check("post_reset.read_hits_still_zero",   rdsys(d, 10), 0u);
    check("post_reset.read_misses_still_zero", rdsys(d, 11), 0u);

    // Let the walker complete.
    int walk_cycles = 0;
    while ((rdsys(d, 6) & 1) && walk_cycles++ < 2000)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    check_bool("post_reset.walker_completed",
               (rdsys(d, 6) & 1) == 0u, true);

    // After the walker is done, ready=1.  A fresh cacheable access
    // now goes through the cache pipeline and counts as a normal
    // cache miss.
    d->i_addr = 0x2000;
    d->i_cacheable = 1;
    d->i_re = 1;
    d->eval();
    safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);

    check("post_reset.first_post_walk_access_is_miss",
          rdsys(d, 11), 1u);
    check("post_reset.no_phantom_hit_from_walker_period",
          rdsys(d, 10), 0u);
}

// ══════════════════════════════════════════════════════════════
// Throughput characterization: back-to-back cached read hits.
//
// HIT_LATENCY=2 is a *latency* property (data lands 2 cycles after
// a request is accepted).  This test measures *throughput* — the
// initiation interval (II), i.e. the cycle gap between consecutive
// accepted read hits when a requester streams addresses instead of
// waiting for each word's busy to drop.  That streaming pattern is
// exactly the L2→L1 line-fill access (four consecutive words of one
// resident line).
//
// Every other test here uses the wait-for-busy single-outstanding
// pattern, which serialises by construction and cannot observe II.
// This driver advances i_addr on every cycle o_busy is low.
//
// The storage path (tag/data/valid BRAM reads off i_addr, launched
// every cycle — l2_cache.sv:195/240/294) is already II=1-capable;
// the stage-0 latch gate `!s1_valid` (l2_cache.sv:682) is what
// holds a new request out of stage 1 for an extra cycle, giving
// II=2.  This test locks the current II=2 baseline.  When the
// read-pipeline decouple lands, flip EXPECTED_II to 1 — the test
// then proves the decouple works and that data stays correct.
static void test_back_to_back_read_throughput(Vl2_cache* d) {
    printf("── Back-to-back read-hit throughput (initiation interval) ──\n");
    reset(d);
    wrsys(d, 1, 1);            // CTRL.enable = 1
    MemMock mem;

    const uint32_t base   = 0x4000;
    const int      NWORDS = 4;

    // Prime the line: first read misses and fills all 4 words.
    d->i_addr = base; d->i_cacheable = 1; d->i_re = 1; d->eval();
    int safety = 0;
    while (d->o_busy && safety++ < 100)
        tick_with_mem(d, mem, 2, rdata_addr_complement);
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);
    uint32_t served_after_prime = mem.served_count;

    // Stream the 4 words.  On every cycle o_busy is low, record the
    // tick index + data for the in-flight word and advance i_addr to
    // the next word.  o_rdata on a busy-low cycle is the stage-1
    // (s1) word, not the just-set i_addr — so capture before advancing.
    int      valid_tick[NWORDS] = {0};
    uint32_t got[NWORDS]        = {0};
    int      word = 0;
    d->i_addr = base; d->i_cacheable = 1; d->i_re = 1; d->eval();
    int t = 0;
    safety = 0;
    while (word < NWORDS && safety++ < 64) {
        if (!d->o_busy) {
            valid_tick[word] = t;
            got[word]        = d->o_rdata;
            word++;
            if (word < NWORDS) { d->i_addr = base + 4 * word; d->eval(); }
        }
        tick_with_mem(d, mem, 0, rdata_addr_complement);
        t++;
    }
    d->i_re = 0; tick_with_mem(d, mem, 0, rdata_addr_complement);

    check_bool("throughput.all_four_served", word == NWORDS, true);

    // All four hit — no downstream memory traffic during the stream.
    check("throughput.no_memory_traffic_on_hits",
          mem.served_count, served_after_prime);

    // Data correctness across the pipelined stream.
    for (int i = 0; i < NWORDS; i++) {
        char name[48];
        snprintf(name, sizeof(name), "throughput.data_word%d", i);
        check(name, got[i], rdata_addr_complement(base + 4 * i));
    }

    // Initiation interval = gap between consecutive data-valid cycles.
    const int EXPECTED_II = 2;   // II=2 today; → 1 after the decouple.
    for (int i = 1; i < NWORDS; i++) {
        int gap = valid_tick[i] - valid_tick[i - 1];
        char name[48];
        snprintf(name, sizeof(name), "throughput.ii_word%d_%d", i - 1, i);
        check(name, (uint32_t)gap, (uint32_t)EXPECTED_II);
    }
    printf("   measured II = %d cycle(s)/word (data-valid ticks %d,%d,%d,%d)\n",
           valid_tick[1] - valid_tick[0],
           valid_tick[0], valid_tick[1], valid_tick[2], valid_tick[3]);
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vl2_cache* d = new Vl2_cache;

    printf("── L2 Cache Unit Tests ──\n\n");

    test_info_register(d);
    test_passthrough_when_disabled(d);
    test_cached_read_miss_then_hit(d);
    test_write_updates_line(d);
    test_inval_all(d);
    test_inval_all_implicit_fence(d);
    test_uncached_skips_cache(d);
    test_perfctrs(d);
    test_multi_set_independence(d);
    test_byte_en_accumulation(d);
    test_perfctrs_inval_all(d);
    test_perfctrs_during_post_reset_walk(d);
    test_back_to_back_read_throughput(d);

    printf("\nl2_cache: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return errors ? 1 : 0;
}
