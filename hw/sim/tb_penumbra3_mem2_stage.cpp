// Verilator testbench for penumbra3_mem2_stage as the data side composes it
// (penumbra3_mem2_stage_test): MEM1, the TLB's D-copy, a real L1 and MEM2
// with its completion unit. The testbench is the uncore: it answers the
// requests the completion unit raises, from a memory whose content is a
// function of the address.
//
// Timing: an access driven in cycle A is in MEM1; one tick later it is in
// MEM2 with the translation and the L1 verdict resolved; it leaves MEM2 at
// the next tick, into WB or into the completion unit.
//
// Covers: a load with the L1 disabled; a miss, its request, the line
// install and the completion, cycle by cycle; hits on the installed line
// (+ sub-word); an uncached load as a word read; a faulted line read;
// stores -- a hit and a miss written through, a sub-word store's lane, an
// uncached store retiring with its completion, a faulted one, the launch
// side held until the write completes; the L1's counters; a TLB miss
// retiring with its fault; the MEM2->EX forward; a slot held in MEM2 taking
// effect only as it leaves (behind a parked access, under WB
// back-pressure, squashed by a fault commit); and the slot behind a parked
// access resolving on the L1 as it is after the install or the write.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include "Vpenumbra3_mem2_stage_test.h"
#include "verilated.h"

typedef Vpenumbra3_mem2_stage_test Dut;

enum { OPC_LOAD = 1, OPC_STORE = 2 };
enum { MEM_SZ_BYTE = 0, MEM_SZ_HALF = 1, MEM_SZ_WORD = 2 };
enum { VEC_TLB_MISS = 2 };
enum { BCAUSE_LOAD = 3, BCAUSE_STORE = 4 };
// TLB PTE flag bits
enum { F_V = 0x01, F_C = 0x04, F_R = 0x08, F_W = 0x10 };
// Cache device registers
enum { CACHE_CTRL = 1, CACHE_READ_HITS = 10, CACHE_READ_MISSES = 11,
       CACHE_WRITE_HITS = 12, CACHE_WRITE_MISSES = 13 };

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

// The memory behind the uncore: every word a function of its address
// until a write changes it.
static std::map<uint32_t, uint32_t> written;
static uint32_t mem_word(uint32_t pa) {
    auto it = written.find(pa & ~3u);
    return it != written.end() ? it->second : ((pa >> 2) * 2654435761u) ^ 0x5A5AA5A5u;
}

// ── The uncore, played each cycle ────────────────────────────────
// With automatic set it answers a request once the request has been up
// for `wait` whole cycles, so never in the cycle valid rises. A line read
// returns its line; a word read returns its word in word 0, beside words
// no load may take. A write lands in the memory with its done; a
// cacheable write completes without a fault whatever the bus said.
static struct {
    bool automatic = true;
    int  wait = 2;
    bool fault = false;      // fault the next response
    int  age = 0;
} uc;

static void uncore_idle(Dut* d);
static void uncore_answer(Dut* d, bool fault) {
    if (!d->o_dreq_valid) { uncore_idle(d); return; }    // nothing to answer
    uint32_t a = d->o_dreq_addr;
    if (d->o_dreq_we && d->o_dreq_cacheable) fault = false;
    if (d->o_dreq_we && !fault) {
        uint32_t lanes = 0;
        for (int b = 0; b < 4; b++)
            if (d->o_dreq_byte_en >> b & 1) lanes |= 0xFFu << 8 * b;
        written[a & ~3u] = (mem_word(a) & ~lanes) | (d->o_dreq_wdata & lanes);
    }
    d->i_drsp_done = 1;
    d->i_rsp_fault = fault;
    for (int k = 0; k < 4; k++)
        d->i_rsp_line[k] = fault               ? 0xFA017000u + k
                         : d->o_dreq_cacheable ? mem_word((a & ~15u) + 4 * k)
                         : k == 0              ? mem_word(a & ~3u)
                         :                       0xDEAD0000u + k;
    d->eval();
}
static void uncore_idle(Dut* d) {
    d->i_drsp_done = 0; d->i_rsp_fault = 0;
    d->eval();
}

// ── What the testbench watches every cycle ───────────────────────
static struct {
    int      cycle;
    int      requests, installs, completions, store_strobes, req_changed;
    uint32_t req_addr;  int req_cacheable, req_byte_en, req_we;
    uint32_t req_wdata;
    int      fill_way;  uint32_t fill_paddr;
    uint32_t cpl_value; int cpl_dest, cpl_fault, cpl_we;
    int      wb_n[32];  uint32_t wb_value[32];
    // Cycles from a done to the first cycle o_mem_pending is low after it.
    int      done_at, release_lag;
    bool     lag_open;
    bool     prev_valid, prev_done, prev_pending;
} m;

static void watch_reset() {
    int cycle = m.cycle;
    bool pv = m.prev_valid, pd = m.prev_done, pp = m.prev_pending;
    memset(&m, 0, sizeof m);
    m.cycle = cycle; m.prev_valid = pv; m.prev_done = pd; m.prev_pending = pp;
}

static void tick(Dut* d) {
    d->eval();
    if (uc.automatic) {
        uc.age = d->o_dreq_valid ? uc.age + 1 : 0;
        if (uc.age > uc.wait) { uncore_answer(d, uc.fault); uc.fault = false; }
        else                  uncore_idle(d);
    }

    // A request is new when valid rises, or stays up the cycle after a done.
    if (d->o_dreq_valid && (!m.prev_valid || m.prev_done)) {
        m.requests++;
        m.req_addr = d->o_dreq_addr; m.req_cacheable = d->o_dreq_cacheable;
        m.req_byte_en = d->o_dreq_byte_en; m.req_we = d->o_dreq_we;
        m.req_wdata = d->o_dreq_wdata;
    } else if (d->o_dreq_valid) {
        m.req_changed += d->o_dreq_addr != m.req_addr
                      || d->o_dreq_cacheable != m.req_cacheable
                      || d->o_dreq_byte_en != m.req_byte_en
                      || d->o_dreq_we != m.req_we
                      || d->o_dreq_wdata != m.req_wdata;
    }
    if (d->o_dcache_fill_we) {
        m.installs++;
        m.fill_way = d->o_dcache_fill_way; m.fill_paddr = d->o_dcache_fill_paddr;
    }
    if (d->o_complete) {
        m.completions++;
        m.cpl_value = d->o_complete_value; m.cpl_dest = d->o_complete_dest;
        m.cpl_fault = d->o_complete_fault; m.cpl_we = d->o_complete_we;
    }
    m.store_strobes += d->o_dcache_we;
    if (d->o_wb_valid) {
        m.wb_n[d->o_wb_phys_dst]++;
        m.wb_value[d->o_wb_phys_dst] = d->o_wb_value;
    }
    if (m.lag_open && m.prev_pending && !d->o_mem_pending) {
        m.release_lag = m.cycle - m.done_at; m.lag_open = false;
    }
    if (d->i_drsp_done) { m.done_at = m.cycle; m.lag_open = true; }
    m.prev_valid = d->o_dreq_valid; m.prev_done = d->i_drsp_done;
    m.prev_pending = d->o_mem_pending;
    m.cycle++;

    d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval();
}

// ── Driving the pipe ─────────────────────────────────────────────
static uint32_t enc_m(int L, int sz, int se, int rd) {
    return (2u << 30) | ((L & 1) << 29) | ((sz & 3) << 27) | ((se & 1) << 26)
         | ((rd & 0xF) << 22);
}
static uint32_t enc_r(int op, int rd) {
    return ((op & 0x1F) << 25) | ((rd & 0xF) << 21);
}
static uint32_t ld(int sz, int se, int rd) { return enc_m(1, sz, se, rd); }
static uint32_t st(int sz)                 { return enc_m(0, sz, 0, 7); }

static void drive_none(Dut* d) {
    d->i_ir = 0; d->i_ea = 0; d->i_phys_dst = 0; d->i_phys_dst_we = 0;
    d->i_fault_pending = 0; d->i_store_data = 0; d->i_valid = 0;
    d->eval();
}
// Put one instruction in MEM1 for this cycle.
static void drive(Dut* d, uint32_t ir, uint32_t ea, int dst, int dst_we,
                  uint32_t store = 0) {
    d->i_ir = ir; d->i_ea = ea; d->i_phys_dst = dst; d->i_phys_dst_we = dst_we;
    d->i_fault_pending = 0; d->i_store_data = store; d->i_valid = 1;
    d->eval();
}
// Tick until nothing is parked, then let WB and a completion drain.
static void drain(Dut* d) {
    for (int guard = 0; d->o_mem_pending && guard < 64; guard++) tick(d);
    for (int i = 0; i < 4; i++) tick(d);
}
// One instruction through the pipe, alone.
static void run(Dut* d, uint32_t ir, uint32_t ea, int dst, int dst_we,
                uint32_t store = 0) {
    drive(d, ir, ea, dst, dst_we, store);
    tick(d);                    // into MEM2
    drive_none(d);
    tick(d);                    // out of MEM2: to WB, or parked
    drain(d);
}
// Two instructions through the pipe, the second directly behind the first.
// A first instruction with destination 0 writes no register (a store).
static void run2(Dut* d, uint32_t ir1, uint32_t ea1, int dst1,
                         uint32_t ir2, uint32_t ea2, int dst2, int dst2_we,
                         uint32_t store2 = 0, uint32_t store1 = 0) {
    drive(d, ir1, ea1, dst1, dst1 != 0, store1);
    tick(d);                    // the first is in MEM2
    drive(d, ir2, ea2, dst2, dst2_we, store2);
    tick(d);                    // the first leaves MEM2; the second enters it
    drive_none(d);
    tick(d);
    drain(d);
}

static void tlb_install(Dut* d, uint32_t vaddr, uint32_t ppn, uint32_t flags) {
    d->i_tlb_wr_en = 1;
    d->i_tlb_wr_set = (vaddr >> 12) & 0x1F;
    d->i_tlb_wr_way = 0;
    d->i_tlb_wr_valid = 1;
    d->i_tlb_wr_vpn_word = (vaddr >> 12) << 8;          // VPN<<8 | ASID(0)
    d->i_tlb_wr_pte_word = (ppn << 12) | flags;
    tick(d);
    d->i_tlb_wr_en = 0; d->eval();
}
static void l1_write(Dut* d, int reg, uint32_t value) {
    d->i_l1_sys_reg = reg; d->i_l1_sys_wdata = value; d->i_l1_sys_we = 1;
    tick(d);
    d->i_l1_sys_we = 0; d->eval();
}
static uint32_t l1_read(Dut* d, int reg) {
    for (int i = 0; i < 3; i++) tick(d);    // the counters trail their events
    d->i_l1_sys_reg = reg; d->i_l1_sys_re = 1;
    tick(d);
    d->i_l1_sys_re = 0; d->eval();
    return d->o_l1_sys_rdata;
}

static int32_t sext8(uint32_t v) { return (int32_t)(int8_t)(v & 0xFF); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* d = new Dut;

    drive_none(d);
    d->i_user_mode = 0; d->i_asid = 0;
    d->i_tlb_wr_en = 0; d->i_ext_stall = 0; d->i_bubble = 0;
    d->i_l1_sys_reg = 0; d->i_l1_sys_wdata = 0; d->i_l1_sys_we = 0; d->i_l1_sys_re = 0;
    uncore_idle(d);
    d->i_rst = 1; tick(d); tick(d); d->i_rst = 0; d->eval();
    check("rst_valid", d->o_wb_valid, 0);

    // Three cacheable pages, so three lines can share an L1 set, and one
    // uncacheable page.
    tlb_install(d, 0x1000, 0x2, F_V | F_C | F_R | F_W);   // -> 0x2000
    tlb_install(d, 0x3000, 0x4, F_V | F_R | F_W);         // -> 0x4000, uncacheable
    tlb_install(d, 0x5000, 0x6, F_V | F_C | F_R | F_W);   // -> 0x6000
    tlb_install(d, 0x7000, 0x8, F_V | F_C | F_R | F_W);   // -> 0x8000

    // ── L1 disabled: a cacheable load reads a line and installs nothing ──
    watch_reset();
    run(d, ld(MEM_SZ_WORD, 0, 5), 0x1008, 5, 1);
    check("off_request",       m.requests, 1);
    check("off_cacheable",     m.req_cacheable, 1);
    check("off_addr",          m.req_addr, 0x2008);
    check("off_no_install",    m.installs, 0);
    check("off_value",         m.cpl_value, mem_word(0x2008));
    run(d, ld(MEM_SZ_WORD, 0, 5), 0x1008, 5, 1);
    check("off_asks_again",    m.requests, 2);

    l1_write(d, CACHE_CTRL, 1);

    // ── A miss, cycle by cycle: park, request, install, completion ──
    uc.automatic = false;
    watch_reset();
    drive(d, ld(MEM_SZ_WORD, 0, 5), 0x1008, 5, 1);
    tick(d);                                   // the load is in MEM2
    drive_none(d);
    check("miss_park_no_pending",  d->o_mem_pending, 0);   // both register at the park
    check("miss_park_no_request",  d->o_dreq_valid, 0);
    tick(d);                                   // p+1
    check("miss_pending",          d->o_mem_pending, 1);
    check("miss_request",          d->o_dreq_valid, 1);
    check("miss_req_cacheable",    d->o_dreq_cacheable, 1);
    check("miss_req_addr",         d->o_dreq_addr, 0x2008);
    check("miss_req_byte_en",      d->o_dreq_byte_en, 0xF);
    check("miss_not_retired",      d->o_wb_valid, 0);
    tick(d); tick(d);
    check("miss_request_held",     d->o_dreq_valid, 1);
    uncore_answer(d, false);                   // r
    check("miss_install",          d->o_dcache_fill_we, 1);
    check("miss_install_way",      d->o_dcache_fill_way, 0);   // the lowest invalid way
    check("miss_install_paddr",    d->o_dcache_fill_paddr >> 4, 0x2008 >> 4);
    check("miss_no_complete_yet",  d->o_complete, 0);
    tick(d);                                   // r+1
    uncore_idle(d);
    check("miss_complete",         d->o_complete, 1);
    check("miss_complete_value",   d->o_complete_value, mem_word(0x2008));
    check("miss_complete_dest",    d->o_complete_dest, 5);
    check("miss_complete_nofault", d->o_complete_fault, 0);
    check("miss_request_dropped",  d->o_dreq_valid, 0);
    drain(d);
    check("miss_one_request",      m.requests, 1);
    check("miss_request_stable",   m.req_changed, 0);
    check("miss_one_install",      m.installs, 1);
    check("miss_one_completion",   m.completions, 1);
    uc.automatic = true;

    // ── Hits on the installed line ──
    watch_reset();
    run(d, ld(MEM_SZ_WORD, 0, 6), 0x1008, 6, 1);
    check("hit_retired",    m.wb_n[6], 1);
    check("hit_value",      m.wb_value[6], mem_word(0x2008));
    check("hit_no_request", m.requests, 0);
    run(d, ld(MEM_SZ_WORD, 0, 6), 0x100C, 6, 1);           // another word of the line
    check("hit_other_word", m.wb_value[6], mem_word(0x200C));
    run(d, ld(MEM_SZ_BYTE, 0, 6), 0x1002, 6, 1);           // byte 2, zero-extended
    check("hit_byte",       m.wb_value[6], (mem_word(0x2000) >> 16) & 0xFF);
    check("hit_no_request_2", m.requests, 0);

    // ── A sub-word miss extracts from the line's word ──
    watch_reset();
    run(d, ld(MEM_SZ_BYTE, 1, 5), 0x1021, 5, 1);           // byte 1, sign-extended
    check("subword_miss_request", m.requests, 1);
    check("subword_miss_value",   m.cpl_value, (uint32_t)sext8(mem_word(0x2020) >> 8));
    run(d, ld(MEM_SZ_HALF, 0, 6), 0x1022, 6, 1);           // the upper half, now a hit
    check("subword_hit_value",    m.wb_value[6], mem_word(0x2020) >> 16);

    // ── An uncacheable load is a word read of the access itself ──
    watch_reset();
    run(d, ld(MEM_SZ_BYTE, 0, 5), 0x3006, 5, 1);
    check("uncached_request",    m.requests, 1);
    check("uncached_cacheable",  m.req_cacheable, 0);
    check("uncached_addr",       m.req_addr, 0x4006);      // the byte address, unchanged
    check("uncached_byte_en",    m.req_byte_en, 0x4);
    check("uncached_value",      m.cpl_value, (mem_word(0x4004) >> 16) & 0xFF);
    check("uncached_no_install", m.installs, 0);
    check("uncached_release_lag", m.release_lag, 1);       // nothing was installed

    // ── A faulted line read installs nothing and completes with the fault ──
    watch_reset();
    uc.fault = true;
    run(d, ld(MEM_SZ_WORD, 0, 5), 0x1040, 5, 1);
    check("fault_completes",  m.completions, 1);
    check("fault_reported",   m.cpl_fault, 1);
    check("fault_no_install", m.installs, 0);
    run(d, ld(MEM_SZ_WORD, 0, 5), 0x1040, 5, 1);           // so it is asked for again
    check("fault_asks_again", m.requests, 2);
    check("fault_then_value", m.cpl_value, mem_word(0x2040));

    // ── A cacheable store that hits writes the L1 and writes through ──
    watch_reset();
    run(d, st(MEM_SZ_WORD), 0x1008, 0, 0, 0xAABBCCDD);
    check("store_strobe_once",    m.store_strobes, 1);
    check("store_request",        m.requests, 1);
    check("store_req_we",         m.req_we, 1);
    check("store_req_cacheable",  m.req_cacheable, 1);
    check("store_req_addr",       m.req_addr, 0x2008);
    check("store_req_byte_en",    m.req_byte_en, 0xF);
    check("store_req_wdata",      m.req_wdata, 0xAABBCCDD);
    check("store_req_stable",     m.req_changed, 0);
    check("store_retired",        m.wb_n[0], 1);         // its fault is asynchronous
    check("store_completes",      m.completions, 1);
    check("store_writes_no_reg",  m.cpl_we, 0);
    check("store_no_install",     m.installs, 0);
    check("store_release_lag",    m.release_lag, 1);
    check("store_in_memory",      mem_word(0x2008), 0xAABBCCDD);
    run(d, ld(MEM_SZ_WORD, 0, 6), 0x1008, 6, 1);
    check("store_read_back",      m.wb_value[6], 0xAABBCCDD);
    check("store_read_back_hits", m.requests, 1);        // the store's write and no other

    // ── A store holds the launch side until its write completes ──
    uc.automatic = false;
    watch_reset();
    drive(d, st(MEM_SZ_WORD), 0x1010, 0, 0, 0x11223344);
    tick(d);                                   // the store is in MEM2
    drive(d, enc_r(0 /* ADD */, 4), 0x44440000, 4, 1);
    tick(d);                                   // p+1: parked; the ADD waits in MEM2
    drive_none(d);
    check("store_hold_pending",   d->o_mem_pending, 1);
    check("store_hold_request",   d->o_dreq_valid, 1);
    check("store_hold_we",        d->o_dreq_we, 1);
    check("store_hold_retired",   d->o_wb_valid, 1);     // the store, as it left MEM2
    tick(d);                                   // p+2
    check("store_hold_bubble",    d->o_wb_valid, 0);
    check("store_hold_bcause",    d->o_wb_bcause, BCAUSE_STORE);
    tick(d);
    uncore_answer(d, false);                   // r
    tick(d);                                   // r+1
    uncore_idle(d);
    check("store_hold_complete",  d->o_complete, 1);
    check("store_hold_no_reg",    d->o_complete_we, 0);
    check("store_hold_released",  d->o_mem_pending, 0);
    drain(d);
    check("store_hold_add_once",  m.wb_n[4], 1);
    uc.automatic = true;

    // ── A byte store writes its one lane ──
    watch_reset();
    run(d, st(MEM_SZ_BYTE), 0x100A, 0, 0, 0xEE);         // byte 2 of the word above
    check("store_byte_byte_en",   m.req_byte_en, 0x4);
    check("store_byte_lane",      (m.req_wdata >> 16) & 0xFF, 0xEE);
    check("store_byte_memory",    mem_word(0x2008), 0xAAEECCDD);
    run(d, ld(MEM_SZ_WORD, 0, 6), 0x1008, 6, 1);
    check("store_byte_read_back", m.wb_value[6], 0xAAEECCDD);

    // ── A cacheable store miss writes through and allocates nothing ──
    watch_reset();
    run(d, st(MEM_SZ_WORD), 0x10A4, 0, 0, 0x0BADF00D);   // a line the L1 does not hold
    check("store_miss_request",    m.requests, 1);
    check("store_miss_no_install", m.installs, 0);
    run(d, ld(MEM_SZ_WORD, 0, 6), 0x10A4, 6, 1);
    check("store_miss_load_misses", m.requests, 2);      // nothing was allocated
    check("store_miss_read_back",  m.cpl_value, 0x0BADF00D);

    // ── An uncached store: a word write, retired by its completion ──
    watch_reset();
    run(d, st(MEM_SZ_HALF), 0x3006, 0, 0, 0xBEEF);
    check("uc_store_request",       m.requests, 1);
    check("uc_store_cacheable",     m.req_cacheable, 0);
    check("uc_store_addr",          m.req_addr, 0x4006);  // the byte address, unchanged
    check("uc_store_byte_en",       m.req_byte_en, 0xC);
    check("uc_store_not_retired",   m.wb_n[0], 0);        // a fault on it is precise
    check("uc_store_completes",     m.completions, 1);
    check("uc_store_writes_no_reg", m.cpl_we, 0);
    check("uc_store_memory",        mem_word(0x4004) >> 16, 0xBEEF);
    run(d, ld(MEM_SZ_HALF, 0, 6), 0x3006, 6, 1);
    check("uc_store_read_back",     m.cpl_value, 0xBEEF);

    // ── A faulted uncached store completes with it, writing nothing ──
    watch_reset();
    uc.fault = true;
    run(d, st(MEM_SZ_WORD), 0x3010, 0, 0, 0x12345678);
    uc.fault = false;                          // whether or not a request took it
    check("uc_store_fault_completes", m.completions, 1);
    check("uc_store_fault_reported",  m.cpl_fault, 1);
    check("uc_store_fault_no_write",  (uint32_t)written.count(0x4010), 0);

    // ── A store with a TLB miss retires with the fault and asks for nothing ──
    watch_reset();
    drive(d, st(MEM_SZ_WORD), 0x9000, 0, 0, 0x1);       // no entry
    tick(d);
    drive_none(d);
    tick(d);
    check("st_tlbmiss_wb_valid",   d->o_wb_valid, 1);
    check("st_tlbmiss_fault",      d->o_wb_fault_pending, 1);
    drain(d);
    check("st_tlbmiss_no_request", m.requests, 0);

    // ── The L1 counts each cacheable access once ──
    uint32_t rh = l1_read(d, CACHE_READ_HITS),  rm = l1_read(d, CACHE_READ_MISSES);
    uint32_t wh = l1_read(d, CACHE_WRITE_HITS), wm = l1_read(d, CACHE_WRITE_MISSES);
    run(d, ld(MEM_SZ_WORD, 0, 5), 0x1140, 5, 1);           // a miss
    run(d, ld(MEM_SZ_WORD, 0, 5), 0x1140, 5, 1);           // a hit
    run(d, st(MEM_SZ_WORD), 0x1144, 0, 0, 0x01020304);     // a store hit
    run(d, st(MEM_SZ_WORD), 0x1244, 0, 0, 0x05060708);     // a store miss
    run(d, ld(MEM_SZ_WORD, 0, 5), 0x3000, 5, 1);           // uncacheable: not counted
    check("count_read_hits",    l1_read(d, CACHE_READ_HITS)    - rh, 1);
    check("count_read_misses",  l1_read(d, CACHE_READ_MISSES)  - rm, 1);
    check("count_write_hits",   l1_read(d, CACHE_WRITE_HITS)   - wh, 1);
    check("count_write_misses", l1_read(d, CACHE_WRITE_MISSES) - wm, 1);

    // ── An ALU result is forwarded from MEM2 as it entered ──
    drive(d, enc_r(0 /* ADD */, 4), 0xCAFE0010, 4, 1);     // the carried result
    tick(d);
    drive_none(d);
    check("alu_fwd_valid",  d->o_fwd_valid, 1);
    check("alu_fwd_result", d->o_fwd_result, 0xCAFE0010);
    check("alu_fwd_dst",    d->o_fwd_dst, 4);
    drain(d);

    // ── A load's value is not forwarded from MEM2 ──
    drive(d, ld(MEM_SZ_WORD, 0, 3), 0x100C, 3, 1);
    tick(d);
    drive_none(d);
    check("load_no_fwd", d->o_fwd_valid, 0);
    drain(d);

    // ── A TLB miss retires at WB carrying the fault ──
    watch_reset();
    drive(d, ld(MEM_SZ_WORD, 0, 4), 0x9000, 4, 1);         // no entry
    tick(d);
    drive_none(d);
    tick(d);
    check("tlbmiss_wb_valid",   d->o_wb_valid, 1);
    check("tlbmiss_fault",      d->o_wb_fault_pending, 1);
    check("tlbmiss_vec",        d->o_wb_fault_vec, VEC_TLB_MISS);
    drain(d);
    check("tlbmiss_no_request", m.requests, 0);

    // The slot in MEM2 takes effect -- moves into WB, parks, writes the L1 --
    // in the one cycle it leaves MEM2. The sequences below hold it there in
    // each of the ways the spine can: behind a parked load, under WB
    // back-pressure, and squashed by a fault commit.

    // ── An ALU op behind a parked load retires once ──
    watch_reset();
    run2(d, ld(MEM_SZ_WORD, 0, 5), 0x1180, 5,
            enc_r(0 /* ADD */, 4), 0x11110000, 4, 1);
    check("behind_alu_retires_once",  m.wb_n[4], 1);
    check("behind_alu_value",         m.wb_value[4], 0x11110000);
    check("behind_alu_load_completes", m.completions, 1);
    check("behind_alu_release_lag",   m.release_lag, 1);   // it reads no lookup

    // ── A store behind a parked load writes the L1 once ──
    watch_reset();
    run2(d, ld(MEM_SZ_WORD, 0, 5), 0x11C0, 5,
            st(MEM_SZ_WORD), 0x100C, 0, 0, 0x55AA55AA);
    check("behind_store_writes_once", m.store_strobes, 1);
    check("behind_store_requests",    m.requests, 2);   // the line, then the write
    run(d, ld(MEM_SZ_WORD, 0, 6), 0x100C, 6, 1);
    check("behind_store_read_back",   m.wb_value[6], 0x55AA55AA);

    // ── A load that misses under WB back-pressure parks when it leaves MEM2 ──
    watch_reset();
    drive(d, ld(MEM_SZ_WORD, 0, 5), 0x1200, 5, 1);
    tick(d);                                   // the load is in MEM2
    drive_none(d);
    d->i_ext_stall = 1; d->eval();
    tick(d);                                   // held: the load is still in MEM2
    check("held_load_not_parked",       d->o_mem_pending, 0);
    check("held_load_no_request",       d->o_dreq_valid, 0);
    d->i_ext_stall = 0; d->eval();             // released: the load leaves MEM2
    tick(d);
    check("held_load_parks_on_release", d->o_mem_pending, 1);
    drain(d);
    check("held_load_one_request",      m.requests, 1);
    check("held_load_completes_once",   m.completions, 1);

    // ── A load squashed in MEM2 by a fault commit does not park ──
    watch_reset();
    drive(d, ld(MEM_SZ_WORD, 0, 5), 0x1240, 5, 1);
    tick(d);                                   // the load is in MEM2
    drive_none(d);
    d->i_bubble = 1; d->eval();
    tick(d);
    d->i_bubble = 0; d->eval();
    check("squashed_load_not_parked",  d->o_mem_pending, 0);
    drain(d);
    check("squashed_load_no_request",  m.requests, 0);
    check("squashed_load_not_retired", m.wb_n[5], 0);
    check("squashed_load_no_complete", m.completions, 0);

    // The slot behind a parked load resolves in the first cycle
    // o_mem_pending is low. Whatever it reads from the L1 then has to be
    // the L1 as it is after the install.

    // ── A load of the line being filled, behind the load that fills it ──
    watch_reset();
    run2(d, ld(MEM_SZ_WORD, 0, 5), 0x1280, 5,
            ld(MEM_SZ_WORD, 0, 6), 0x1284, 6, 1);
    check("same_line_behind_one_request", m.requests, 1);
    check("same_line_behind_one_install", m.installs, 1);
    check("same_line_behind_retires",     m.wb_n[6], 1);
    check("same_line_behind_value",       m.wb_value[6], mem_word(0x2284));
    check("same_line_behind_release_lag", m.release_lag, 2);  // the line shows late

    // ── A store to the line the fill evicts, behind the load that fills ──
    // Three lines of one set: 0x12C0, 0x52C0 and 0x72C0. The first two
    // fill the set; the third evicts the first, the less recently used.
    run(d, ld(MEM_SZ_WORD, 0, 5), 0x12C0, 5, 1);           // into way 0
    run(d, ld(MEM_SZ_WORD, 0, 5), 0x52C0, 5, 1);           // into way 1
    watch_reset();
    run2(d, ld(MEM_SZ_WORD, 0, 5), 0x72C0, 5,
            st(MEM_SZ_WORD), 0x12C4, 0, 0, 0xFEEDFACE);
    check("evicted_store_fill_way",  m.fill_way, 0);
    check("evicted_store_written",   mem_word(0x22C4), 0xFEEDFACE);
    run(d, ld(MEM_SZ_WORD, 0, 6), 0x72C4, 6, 1);
    check("evicted_store_line_hits", m.requests, 2);       // the fill and the write
    check("evicted_store_line_kept", m.wb_value[6], mem_word(0x82C4));

    // ── A load of another set, behind a load that fills ──
    watch_reset();
    run2(d, ld(MEM_SZ_WORD, 0, 5), 0x1300, 5,
            ld(MEM_SZ_WORD, 0, 6), 0x1024, 6, 1);          // hits a line installed above
    check("other_set_behind_one_request", m.requests, 1);
    check("other_set_behind_value",       m.wb_value[6], mem_word(0x2024));
    check("other_set_behind_release_lag", m.release_lag, 1);  // its set is as it was

    // ── A load behind a store reads what the store wrote ──
    // The store installs nothing, so the load resumes with the completion
    // and hits the word the store wrote into the L1.
    watch_reset();
    run2(d, st(MEM_SZ_WORD), 0x100C, 0,
            ld(MEM_SZ_WORD, 0, 6), 0x100C, 6, 1, 0, 0x5EED5EED);
    check("load_behind_store_value",       m.wb_value[6], 0x5EED5EED);
    check("load_behind_store_one_request", m.requests, 1);   // the write
    check("load_behind_store_release_lag", m.release_lag, 1);

    // ── A load behind a store that missed reads it from memory ──
    watch_reset();
    run2(d, st(MEM_SZ_WORD), 0x10E4, 0,
            ld(MEM_SZ_WORD, 0, 6), 0x10E4, 6, 1, 0, 0x7E57DA7A);
    check("load_behind_store_miss_requests", m.requests, 2);  // the write, then the line
    check("load_behind_store_miss_value",    m.cpl_value, 0x7E57DA7A);

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
