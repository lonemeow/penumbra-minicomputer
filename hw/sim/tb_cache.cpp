// Verilator testbench for Penumbra Cache
//
// Tests the parameterized PIPT cache module via cache_test wrapper
// (cache + simple_mem with READ_LATENCY=4, WRITE_LATENCY=2).
//
// Test wrapper parameters: NUM_SETS=16, LINE_WORDS=4, MEM_WORDS=1024
// Address breakdown (16 sets × 4-word lines):
//   [31:8]  tag       (24 bits)
//   [ 7:4]  set index (4 bits)
//   [ 3:2]  word      (2 bits)
//   [ 1:0]  byte      (ignored)

#include <cstdio>
#include <cstdint>
#include "Vcache_test.h"

static int errors = 0, tests = 0;

static void tick(Vcache_test* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vcache_test* d) {
    d->i_rst = 1;
    d->i_paddr = 0;
    d->i_wdata = 0;
    d->i_byte_en = 0xF;
    d->i_we = 0;
    d->i_re = 0;
    d->i_cacheable = 0;
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

// Write a word directly to backing memory (debug port).
// Must hold write enable long enough for simple_mem's write latency,
// then drain any stale access state before returning.
static void mem_write(Vcache_test* d, uint32_t addr, uint32_t data) {
    d->i_dbg_mem_addr = addr;
    d->i_dbg_mem_wdata = data;
    d->i_dbg_mem_we = 1;
    for (int i = 0; i < 10; i++)
        tick(d);
    d->i_dbg_mem_we = 0;
    // Drain stale access in simple_mem so the next cache operation
    // starts cleanly (in_access=0, busy_count=0)
    for (int i = 0; i < 10; i++)
        tick(d);
}

// Write a sysreg
static void sys_write(Vcache_test* d, uint32_t reg, uint32_t data) {
    d->i_sys_reg = reg;
    d->i_sys_wdata = data;
    d->i_sys_we = 1;
    tick(d);
    d->i_sys_we = 0;
}

// Read a sysreg (combinational)
static uint32_t sys_read(Vcache_test* d, uint32_t reg) {
    d->i_sys_reg = reg;
    d->eval();
    return d->o_sys_rdata;
}

// Enable the cache
static void cache_enable(Vcache_test* d) {
    sys_write(d, 1, 1);  // SYSREG_CACHE_CTRL = 1, enable bit = 1
}

// Invalidate all — needs extra tick for the inval_req pulse to
// propagate through the always_ff (set on cycle N, clears valid on N+1)
static void cache_inval(Vcache_test* d) {
    sys_write(d, 2, 0);  // SYSREG_CACHE_INVAL = 2, value doesn't matter yet
    tick(d);              // Let valid bits actually clear
}

// Perform a read through the cache, wait for completion.
// Returns the read data and the number of cycles spent busy.
struct ReadResult {
    uint32_t data;
    int busy_cycles;
};

static ReadResult cache_read(Vcache_test* d, uint32_t addr, bool cacheable) {
    ReadResult r;
    r.busy_cycles = 0;

    d->i_paddr = addr;
    d->i_re = 1;
    d->i_we = 0;
    d->i_cacheable = cacheable ? 1 : 0;
    d->i_byte_en = 0xF;
    d->eval();

    // Count busy cycles
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
    tick(d);  // Let state settle
    return r;
}

// Perform a write through the cache, wait for completion.
// Returns number of busy cycles.
static int cache_write(Vcache_test* d, uint32_t addr, uint32_t data,
                       uint8_t byte_en, bool cacheable) {
    int busy_cycles = 0;

    d->i_paddr = addr;
    d->i_wdata = data;
    d->i_we = 1;
    d->i_re = 0;
    d->i_cacheable = cacheable ? 1 : 0;
    d->i_byte_en = byte_en;
    d->eval();

    if (!d->o_busy)
        printf("    [wr@%X] NOT busy on first eval!\n", addr);

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
    tick(d);
    return busy_cycles;
}

// Read a word directly from backing memory (via debug port).
// Sets debug write with byte_en=0 to route the address through
// the mux without actually writing, then reads the output.
static uint32_t mem_read(Vcache_test* d, uint32_t addr) {
    // Use the uncacheable cache_read to get a verified read from memory
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

static void check_range(const char* name, int got, int min, int max) {
    tests++;
    if (got < min || got > max) {
        printf("  FAIL [%s]: got %d, expected %d–%d\n", name, got, min, max);
        errors++;
    }
}

// ══════════════════════════════════════════════════════════════
// Tests
// ══════════════════════════════════════════════════════════════

static void test_sysreg_info(Vcache_test* d) {
    // INFO register (reg 0) should report cache geometry.
    // Format (matches cache.sv INFO_VALUE):
    //   [3:0]   line_words
    //   [13:4]  num_sets
    //   [17:14] num_ways
    //   [19:18] addressing  (0=PIPT, 1=VIPT, 2=VIVT)
    //   [20]    write_back  (0=WT, 1=WB)
    //   [21]    write_alloc (0=WnA, 1=WA)
    uint32_t info = sys_read(d, 0);  // SYSREG_CACHE_INFO = 0

    uint32_t line_words = info & 0xF;
    uint32_t num_sets   = (info >> 4) & 0x3FF;
    uint32_t num_ways   = (info >> 14) & 0xF;
    uint32_t addressing = (info >> 18) & 0x3;
    uint32_t write_back = (info >> 20) & 0x1;
    uint32_t write_alloc = (info >> 21) & 0x1;

    check("info.line_words",  line_words, 4);
    check("info.num_sets",    num_sets,   16);
    check("info.num_ways",    num_ways,   1);
    check("info.addressing",  addressing, 0);  // PIPT
    check("info.write_back",  write_back, 0);  // WT
    check("info.write_alloc", write_alloc, 0); // WnA
}

static void test_disabled_passthru(Vcache_test* d) {
    // Cache is disabled at reset. Reads/writes should pass through.
    reset(d);

    // Pre-load memory
    mem_write(d, 0x100, 0xDEAD'BEEF);

    // Read (uncacheable doesn't matter when disabled)
    ReadResult r = cache_read(d, 0x100, true);
    check("disabled_read.data", r.data, 0xDEAD'BEEF);
    check_bool("disabled_read.busy", r.busy_cycles > 0, true);

    // Write through
    int wc = cache_write(d, 0x200, 0xCAFE'BABE, 0xF, true);
    check_bool("disabled_write.busy", wc > 0, true);

    // Verify memory was written
    uint32_t mval = mem_read(d, 0x200);
    check("disabled_write.mem", mval, 0xCAFE'BABE);
}

static void test_uncacheable_passthru(Vcache_test* d) {
    // Even with cache enabled, C=0 should bypass
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0x1234'5678);

    // Read with cacheable=0
    ReadResult r = cache_read(d, 0x100, false);
    check("uncacheable_read.data", r.data, 0x1234'5678);
    check_bool("uncacheable_read.busy", r.busy_cycles > 0, true);
}

static void test_read_miss_then_hit(Vcache_test* d) {
    // Read miss should fill the line, subsequent read should hit
    reset(d);
    cache_enable(d);

    // Pre-load a cache line's worth of data at address 0x100
    // (set = (0x100 >> 4) & 0xF = 0, words at 0x100, 0x104, 0x108, 0x10C)
    mem_write(d, 0x100, 0xAAAA'0000);
    mem_write(d, 0x104, 0xAAAA'0001);
    mem_write(d, 0x108, 0xAAAA'0002);
    mem_write(d, 0x10C, 0xAAAA'0003);

    // First read: miss, should trigger fill
    ReadResult r1 = cache_read(d, 0x100, true);
    check("miss.data", r1.data, 0xAAAA'0000);
    check_bool("miss.was_slow", r1.busy_cycles > 0, true);
    int miss_cycles = r1.busy_cycles;

    // Second read same address: should hit (0 busy cycles)
    ReadResult r2 = cache_read(d, 0x100, true);
    check("hit.data", r2.data, 0xAAAA'0000);
    check("hit.busy_cycles", r2.busy_cycles, 0);

    // Read other words in same line: should also hit
    ReadResult r3 = cache_read(d, 0x104, true);
    check("hit_w1.data", r3.data, 0xAAAA'0001);
    check("hit_w1.busy_cycles", r3.busy_cycles, 0);

    ReadResult r4 = cache_read(d, 0x108, true);
    check("hit_w2.data", r4.data, 0xAAAA'0002);
    check("hit_w2.busy_cycles", r4.busy_cycles, 0);

    ReadResult r5 = cache_read(d, 0x10C, true);
    check("hit_w3.data", r5.data, 0xAAAA'0003);
    check("hit_w3.busy_cycles", r5.busy_cycles, 0);

    // Verify hit is faster than miss
    printf("    (miss=%d cycles, hit=0 cycles)\n", miss_cycles);
}

static void test_write_hit(Vcache_test* d) {
    // Write to a cached line should update both cache and memory
    reset(d);
    cache_enable(d);

    // Fill a line first
    mem_write(d, 0x100, 0x1111'1111);
    mem_write(d, 0x104, 0x2222'2222);
    mem_write(d, 0x108, 0x3333'3333);
    mem_write(d, 0x10C, 0x4444'4444);
    cache_read(d, 0x100, true);  // Fill line

    // Write to word 1 of the cached line
    cache_write(d, 0x104, 0xFEED'9999, 0xF, true);

    // Read back from cache: should see new value
    ReadResult r = cache_read(d, 0x104, true);
    check("write_hit.cache", r.data, 0xFEED'9999);
    check("write_hit.cache_cycles", r.busy_cycles, 0);

    // Verify memory was also updated (write-through)
    uint32_t mval = mem_read(d, 0x104);
    check("write_hit.mem", mval, 0xFEED'9999);

    // Other words in line should be unchanged
    ReadResult r2 = cache_read(d, 0x100, true);
    check("write_hit.other_word", r2.data, 0x1111'1111);
}

static void test_write_miss(Vcache_test* d) {
    // Write miss: write-no-allocate — write goes to memory, no fill
    reset(d);
    cache_enable(d);

    // Write to address not in cache
    cache_write(d, 0x300, 0xBEEF'CAFE, 0xF, true);

    // Verify memory was written
    uint32_t mval = mem_read(d, 0x300);
    check("write_miss.mem", mval, 0xBEEF'CAFE);

    // Subsequent read should MISS (write-no-allocate didn't fill)
    ReadResult r = cache_read(d, 0x300, true);
    check("write_miss.read_data", r.data, 0xBEEF'CAFE);
    check_bool("write_miss.read_was_slow", r.busy_cycles > 0, true);
}

static void test_invalidate(Vcache_test* d) {
    // Fill a line, invalidate, verify it misses again
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0xAAAA'BBBB);
    mem_write(d, 0x104, 0xCCCC'DDDD);
    mem_write(d, 0x108, 0xEEEE'FFFF);
    mem_write(d, 0x10C, 0x0000'1111);
    cache_read(d, 0x100, true);  // Fill

    // Confirm it's a hit
    ReadResult r1 = cache_read(d, 0x100, true);
    check("inval.before", r1.busy_cycles, 0);

    // Invalidate
    cache_inval(d);

    // Should miss now
    ReadResult r2 = cache_read(d, 0x100, true);
    check("inval.after_data", r2.data, 0xAAAA'BBBB);
    check_bool("inval.after_slow", r2.busy_cycles > 0, true);
}

static void test_tag_conflict(Vcache_test* d) {
    // Two addresses that map to the same set but different tags.
    // Second fill should evict the first.
    // Set index = (addr >> 4) & 0xF. So 0x100 and 0x200 both map to set 0
    // if (0x100 >> 4) & 0xF == 0 and (0x200 >> 4) & 0xF == 0.
    // Wait: 0x100 >> 4 = 0x10, & 0xF = 0. 0x200 >> 4 = 0x20, & 0xF = 0.
    // Both map to set 0. Tags differ: 0x100 >> 8 vs 0x200 >> 8.
    reset(d);
    cache_enable(d);

    // Fill with address A
    mem_write(d, 0x100, 0xAAAA'AAAA);
    mem_write(d, 0x104, 0xAAAA'AAAB);
    mem_write(d, 0x108, 0xAAAA'AAAC);
    mem_write(d, 0x10C, 0xAAAA'AAAD);
    cache_read(d, 0x100, true);
    ReadResult rA = cache_read(d, 0x100, true);
    check("conflict.A_hit", rA.busy_cycles, 0);

    // Fill with address B (same set, different tag)
    mem_write(d, 0x200, 0xBBBB'BBBB);
    mem_write(d, 0x204, 0xBBBB'BBBC);
    mem_write(d, 0x208, 0xBBBB'BBBD);
    mem_write(d, 0x20C, 0xBBBB'BBBE);
    cache_read(d, 0x200, true);
    ReadResult rB = cache_read(d, 0x200, true);
    check("conflict.B_hit", rB.busy_cycles, 0);
    check("conflict.B_data", rB.data, 0xBBBB'BBBB);

    // A should now miss (evicted by B)
    ReadResult rA2 = cache_read(d, 0x100, true);
    check("conflict.A_evicted_data", rA2.data, 0xAAAA'AAAA);
    check_bool("conflict.A_evicted_slow", rA2.busy_cycles > 0, true);
}

static void test_multi_set(Vcache_test* d) {
    // Two addresses in different sets should coexist
    // 0x100 → set 0, 0x110 → set 1
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

    // Fill both
    cache_read(d, 0x100, true);
    cache_read(d, 0x110, true);

    // Both should hit
    ReadResult r1 = cache_read(d, 0x100, true);
    check("multi_set.s0_data", r1.data, 0x1111'1111);
    check("multi_set.s0_hit", r1.busy_cycles, 0);

    ReadResult r2 = cache_read(d, 0x110, true);
    check("multi_set.s1_data", r2.data, 0x2222'2222);
    check("multi_set.s1_hit", r2.busy_cycles, 0);
}

static void test_byte_write(Vcache_test* d) {
    // Byte-granular write should update only selected bytes
    reset(d);
    cache_enable(d);

    mem_write(d, 0x100, 0xAABBCCDD);
    mem_write(d, 0x104, 0);
    mem_write(d, 0x108, 0);
    mem_write(d, 0x10C, 0);
    cache_read(d, 0x100, true);  // Fill

    // Write only byte 1 (bits 15:8)
    cache_write(d, 0x100, 0x0000FF00, 0b0010, true);

    ReadResult r = cache_read(d, 0x100, true);
    check("byte_write.cache", r.data, 0xAABBFFDD);
    check("byte_write.hit", r.busy_cycles, 0);

    // Verify memory too
    uint32_t mval = mem_read(d, 0x100);
    check("byte_write.mem", mval, 0xAABBFFDD);
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vcache_test* d = new Vcache_test;

    printf("── Cache Unit Tests ──\n\n");

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

    printf("\ncache: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return errors ? 1 : 0;
}
