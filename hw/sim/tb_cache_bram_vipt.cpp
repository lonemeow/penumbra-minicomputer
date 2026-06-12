// Verilator testbench for cache_bram_vipt (gen2 BRAM-backed VIPT L1).
//
// Drives the front side per the module's launch/resolve contract and
// emulates the whole back side: a single-beat sync-bus device (busy for a
// configurable number of cycles, then data/commit on the drop cycle) and
// the fill sequencer (engages on a `cacheable && re` line request, streams
// the line through the fill port, fill_done on the last beat).
//
// Checks, in order:
//   - INFO geometry readback
//   - disabled cache: reads/writes pass through as single beats, and a
//     forwarded read never presents cacheable=1 (that bit pattern is the
//     arbiter's line-mode select)
//   - miss -> line request (line-aligned) -> fill -> serve, with
//     drop-equals-valid data timing
//   - hits: same word, same line, and back-to-back streaming (launch
//     overlapping the previous resolve, the I-side fetch pattern)
//   - associativity: NUM_WAYS conflicting lines all resident at once
//   - replacement: invalid-first in index order, then tree-PLRU victim
//     (pins the victim_way() policy)
//   - write-through: every store reaches the back side with its byte_en;
//     a write hit updates the local copy, a write miss allocates nothing
//   - uncacheable accesses bypass the cache and snoop nothing (the
//     documented no-alias software contract)
//   - INVAL_ALL: one settle cycle, then every previously-hot line misses
//   - i_fault: the slot is inert (no busy, no traffic, no state change)
//   - VIPT: tag compare is physical (remapped vaddr hits by paddr)
//   - requested-word capture when it arrives on the fill_done beat
//   - zero-latency single-beat completion (write hit resolving same-cycle)
//   - perfctr totals match an independently-kept tally
//
// The module's own SVA assertions run under --assert and guard the
// internal invariants (fill-port discipline, reset clear, busy/launch
// exclusion) throughout.

#include <cstdio>
#include <cstdint>
#include <map>
#include "Vcache_bram_vipt.h"
#include "verilated.h"

// Geometry — must match the module's default parameters.
static const int      LINE_WORDS = 4;
static const uint32_t LINE_BYTES = 16;

// Unified cache sysreg map (penumbra_pkg.sv).
static const int REG_INFO = 0, REG_CTRL = 1, REG_INVAL_ALL = 2;
static const int REG_READ_HITS = 10, REG_READ_MISSES = 11;
static const int REG_WRITE_HITS = 12, REG_WRITE_MISSES = 13;

static Vcache_bram_vipt* dut;
static int errors = 0, tests = 0;

static void check(const char* n, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) {
        printf("  FAIL [%s]: got 0x%08X, expected 0x%08X\n", n, got, exp);
        errors++;
    }
}

// ── Back-side model ─────────────────────────────────────────────
// Word-addressed backing store + single-beat device + fill sequencer.
static std::map<uint32_t, uint32_t> mem;     // key = paddr >> 2

static uint32_t mem_rd(uint32_t paddr) {
    auto it = mem.find(paddr >> 2);
    return it == mem.end() ? 0 : it->second;
}

static int  sb_lat = 1;          // single-beat busy cycles before completion
static bool sb_pending = false;
static int  sb_wait = 0;
static int  sb_reads = 0, sb_writes = 0;
static uint32_t last_wr_addr = 0, last_wr_data = 0;
static uint8_t  last_wr_be = 0;

static bool     fill_active = false;
static int      fill_idx = 0, fill_gap = 0;
static int      fill_gap_cfg = 1;            // idle cycles between fill beats
static uint32_t fill_base = 0;
static int      fills_served = 0;
static uint32_t last_fill_base = 0;

// Runs once per cycle during the clock-low phase, after the DUT has
// evaluated its outputs for this cycle's front-side inputs.
static void backside_drive() {
    dut->i_fill_we   = 0;
    dut->i_fill_done = 0;
    dut->i_mem_busy  = 0;

    if (fill_active) {
        // The line request must stay asserted for the whole transaction.
        check("fill_req_held", dut->o_mem_re && dut->o_mem_cacheable, 1);
        if (fill_gap > 0) {
            fill_gap--;
        } else {
            uint32_t addr = fill_base + 4u * (uint32_t)fill_idx;
            dut->i_fill_we    = 1;
            dut->i_fill_word  = fill_idx & (LINE_WORDS - 1);
            dut->i_fill_wdata = mem_rd(addr);
            if (fill_idx == LINE_WORDS - 1) {
                dut->i_fill_done = 1;
                fill_active = false;
                fills_served++;
            } else {
                fill_idx++;
                fill_gap = fill_gap_cfg;
            }
        }
        return;
    }

    if (dut->o_mem_re && dut->o_mem_cacheable) {
        // Line request: engage the sequencer; first beat next cycle.
        check("fill_base_aligned", dut->o_mem_addr & (LINE_BYTES - 1), 0);
        fill_active    = true;
        fill_base      = dut->o_mem_addr;
        last_fill_base = dut->o_mem_addr;
        fill_idx       = 0;
        fill_gap       = 0;
        return;
    }

    if (dut->o_mem_re || dut->o_mem_we) {
        if (!sb_pending) { sb_pending = true; sb_wait = sb_lat; }
        if (sb_wait > 0) {
            dut->i_mem_busy = 1;
            sb_wait--;
        } else {
            // Completion cycle: busy low, data valid / write committed.
            if (dut->o_mem_re) {
                dut->i_mem_rdata = mem_rd(dut->o_mem_addr);
                sb_reads++;
            } else {
                uint32_t w   = dut->o_mem_addr >> 2;
                uint32_t old = mem.count(w) ? mem[w] : 0;
                for (int l = 0; l < 4; l++)
                    if (dut->o_mem_byte_en & (1u << l)) {
                        uint32_t m = 0xFFu << (8 * l);
                        old = (old & ~m) | (dut->o_mem_wdata & m);
                    }
                mem[w] = old;
                last_wr_addr = dut->o_mem_addr;
                last_wr_data = dut->o_mem_wdata;
                last_wr_be   = dut->o_mem_byte_en;
                sb_writes++;
            }
            sb_pending = false;
        }
    } else {
        sb_pending = false;
    }
}

// ── Clocking helpers ────────────────────────────────────────────
// Low phase: front inputs already set; DUT evaluates, back side responds,
// DUT evaluates again. Sample outputs after cycle_eval(), then clock.
static void cycle_eval() {
    dut->eval();
    backside_drive();
    dut->eval();
}
static void clock_edge() {
    dut->i_clk = 1; dut->eval();
    dut->i_clk = 0; dut->eval();
}

static void front_clear() {
    dut->i_en = 0; dut->i_re = 0; dut->i_we = 0; dut->i_fault = 0;
    dut->i_vaddr = 0; dut->i_paddr = 0; dut->i_cacheable = 0;
    dut->i_wdata = 0; dut->i_byte_en = 0;
}

static void idle_cycle() {
    cycle_eval();
    clock_edge();
}

// ── Sysreg helpers ──────────────────────────────────────────────
static void sys_write(int reg, uint32_t val) {
    dut->i_sys_reg = reg; dut->i_sys_wdata = val; dut->i_sys_we = 1;
    idle_cycle();
    dut->i_sys_we = 0;
    idle_cycle();                     // settle (registered side effects apply)
}
static uint32_t sys_read(int reg) {
    dut->i_sys_reg = reg;
    dut->eval();                      // combinational read
    return dut->o_sys_rdata;
}

// ── Front-side access driver (the consumer contract) ───────────
// Launch with i_en for one cycle, then assert the request with the MMU
// verdict from the resolve cycle until the completion cycle (busy-drop /
// serve), sampling o_rdata on the cycle busy is low. Deasserts the
// request the cycle after completion.
static uint32_t rd_data = 0;
static int      wait_cycles = 0;

static bool access(uint32_t vaddr, uint32_t paddr, bool re, bool we,
                   uint32_t wdata, uint8_t be, bool cacheable, bool fault) {
    // Launch (cycle T)
    dut->i_vaddr = vaddr; dut->i_en = 1;
    dut->i_re = 0; dut->i_we = 0;
    idle_cycle();
    dut->i_en = 0;

    // Resolve (T+1 onward)
    dut->i_paddr = paddr; dut->i_cacheable = cacheable; dut->i_fault = fault;
    dut->i_re = re; dut->i_we = we;
    dut->i_wdata = wdata; dut->i_byte_en = be;

    wait_cycles = 0;
    for (int i = 0; i < 200; i++) {
        cycle_eval();
        if (!dut->o_busy) {
            rd_data = dut->o_rdata;       // drop-equals-valid
            clock_edge();                 // completion edge
            front_clear();
            idle_cycle();                 // request deasserted, one idle cycle
            return true;
        }
        wait_cycles++;
        clock_edge();
    }
    printf("  FAIL [access timeout] vaddr=0x%08X\n", vaddr);
    errors++; tests++;
    front_clear();
    return false;
}

static uint32_t read_at(uint32_t addr, bool cacheable = true) {
    access(addr, addr, true, false, 0, 0, cacheable, false);
    return rd_data;
}
static uint32_t read_remap(uint32_t vaddr, uint32_t paddr) {
    access(vaddr, paddr, true, false, 0, 0, true, false);
    return rd_data;
}
static void write_at(uint32_t addr, uint32_t data, uint8_t be,
                     bool cacheable = true) {
    access(addr, addr, false, true, data, be, cacheable, false);
}

// Expected perfctr tallies, kept by hand alongside each access below.
static uint32_t exp_rhit = 0, exp_rmiss = 0, exp_whit = 0, exp_wmiss = 0;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    dut = new Vcache_bram_vipt;

    front_clear();
    dut->i_sys_we = 0; dut->i_sys_reg = 0; dut->i_sys_wdata = 0;
    dut->i_mem_busy = 0; dut->i_mem_rdata = 0;
    dut->i_fill_we = 0; dut->i_fill_done = 0;
    dut->i_fill_word = 0; dut->i_fill_wdata = 0;
    dut->i_clk = 0;

    // Reset, held 2 cycles (testbench convention).
    dut->i_rst = 1;
    idle_cycle(); idle_cycle();
    dut->i_rst = 0;
    idle_cycle();

    // Backing-store init. Address groups are chosen set-disjoint so the
    // test phases don't evict each other: set = vaddr[9:4] at the default
    // geometry (64 sets, 16 B lines, 1 KB ways).
    for (uint32_t a = 0x2100; a < 0x2110; a += 4) mem[a >> 2] = 0x11110000 | a;
    for (uint32_t k = 0; k < 6; k++)                          // set 0 conflicts
        for (uint32_t a = 0x4000 + k * 0x400; a < 0x4010 + k * 0x400; a += 4)
            mem[a >> 2] = 0x22220000 | a;
    mem[0x1000 >> 2] = 0x12345678;                            // pass-through
    mem[0x6100 >> 2] = 0x0BADF00D;                            // write-miss line
    for (uint32_t a = 0x13210; a < 0x13220; a += 4) mem[a >> 2] = 0x33330000 | a;
    for (uint32_t a = 0x7100; a < 0x7110; a += 4)  mem[a >> 2] = 0x44440000 | a;
    for (uint32_t a = 0x7200; a < 0x7210; a += 4)  mem[a >> 2] = 0x44440000 | a;
    mem[0x8000 >> 2] = 0x55AA55AA;

    // ── INFO geometry: VIPT, WT/WnA, 4 ways, 64 sets, 4-word lines ──
    check("info", sys_read(REG_INFO),
          (1u << 26) | (4u << 21) | (64u << 6) | 4u);
    check("ctrl_reset_disabled", sys_read(REG_CTRL), 0);

    // ── Disabled: pass-through single beat (and never cacheable=1+re,
    //    which would engage the sequencer / trip the fill assertions) ──
    check("disabled_pt_read", read_at(0x1000, true), 0x12345678);
    check("disabled_pt_sb",   sb_reads, 1);
    check("disabled_no_fill", fills_served, 0);
    write_at(0x1000, 0xA0A0A0A0, 0xF, true);
    check("disabled_pt_write", sb_writes, 1);
    check("disabled_pt_wdata", mem_rd(0x1000), 0xA0A0A0A0);

    // ── Enable, then miss -> fill -> serve ──────────────────────
    sys_write(REG_CTRL, 1);
    check("ctrl_enabled", sys_read(REG_CTRL), 1);

    check("miss_fill_data", read_at(0x2104), 0x11110000 | 0x2104);
    exp_rmiss++;
    check("miss_fill_count", fills_served, 1);
    check("miss_fill_base", last_fill_base, 0x2100);
    check("miss_waited", wait_cycles > 0, 1);

    // ── Hits: same word, same line, zero wait, no traffic ───────
    int sb_r = sb_reads, fs = fills_served;
    check("hit_same_word", read_at(0x2104), 0x11110000 | 0x2104);
    exp_rhit++;
    check("hit_zero_wait", wait_cycles, 0);
    check("hit_same_line", read_at(0x210C), 0x11110000 | 0x210C);
    exp_rhit++;
    check("hit_no_traffic", sb_reads == sb_r && fills_served == fs, 1);

    // ── Streaming: launch the next fetch during this one's resolve ──
    dut->i_vaddr = 0x2104; dut->i_en = 1;
    idle_cycle();
    dut->i_vaddr = 0x2108; dut->i_en = 1;          // launch B during A's resolve
    dut->i_paddr = 0x2104; dut->i_cacheable = 1; dut->i_re = 1;
    cycle_eval();
    check("stream_a_ready", dut->o_busy, 0);
    check("stream_a_data", dut->o_rdata, 0x11110000 | 0x2104);
    exp_rhit++;
    clock_edge();
    dut->i_en = 0;
    dut->i_paddr = 0x2108;                          // B resolves
    cycle_eval();
    check("stream_b_ready", dut->o_busy, 0);
    check("stream_b_data", dut->o_rdata, 0x11110000 | 0x2108);
    exp_rhit++;
    clock_edge();
    front_clear();
    idle_cycle();

    // ── Associativity: 4 conflicting lines (set 0) all resident ──
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t a = 0x4000 + k * 0x400;
        check("assoc_fill", read_at(a), 0x22220000 | a);
        exp_rmiss++;
    }
    sb_r = sb_reads; fs = fills_served;
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t a = 0x4000 + k * 0x400;
        check("assoc_hit", read_at(a), 0x22220000 | a);
        exp_rhit++;
    }
    check("assoc_no_traffic", sb_reads == sb_r && fills_served == fs, 1);

    // ── Replacement: invalid-first is exhausted, tree-PLRU picks now.
    // After the fill/hit sequence above the tree points at way 0
    // (0x4000). The 5th conflict evicts it; a touch of 0x4400 then
    // steers the tree to way 2 (0x4800) for the 6th conflict.
    check("evict_5th_fill", read_at(0x5000), 0x22220000 | 0x5000);
    exp_rmiss++;
    check("evict_touch", read_at(0x4400), 0x22220000 | 0x4400);
    exp_rhit++;
    check("evict_6th_fill", read_at(0x5400), 0x22220000 | 0x5400);
    exp_rmiss++;

    fs = fills_served;
    check("evict_way3_live", read_at(0x4C00), 0x22220000 | 0x4C00);
    exp_rhit++;
    check("evict_way1_live", read_at(0x4400), 0x22220000 | 0x4400);
    exp_rhit++;
    check("evict_no_refill", fills_served, fs);
    check("evict_victim2_gone", read_at(0x4800), 0x22220000 | 0x4800);
    exp_rmiss++;                                    // evicted by the 6th line
    check("evict_victim2_refilled", fills_served, fs + 1);
    check("evict_victim0_gone", read_at(0x4000), 0x22220000 | 0x4000);
    exp_rmiss++;                                    // evicted by the 5th line
    check("evict_victim0_refilled", fills_served, fs + 2);

    // ── Write-through, write hit: downstream beat + local update ──
    int sb_w = sb_writes;
    write_at(0x2104, 0xA5A5A5A5, 0xF);
    exp_whit++;
    check("wt_hit_sb", sb_writes, sb_w + 1);
    check("wt_hit_addr", last_wr_addr, 0x2104);
    check("wt_hit_be", last_wr_be, 0xF);
    check("wt_hit_mem", mem_rd(0x2104), 0xA5A5A5A5);
    check("wt_hit_local", read_at(0x2104), 0xA5A5A5A5);
    exp_rhit++;

    // Sub-word: only lane 2 of the local copy and memory may change
    // (word at 0x2108 starts as 0x11112108; lane 2 becomes 0xCC).
    write_at(0x2108, 0x00CC0000, 0x4);
    exp_whit++;
    check("wt_sub_be", last_wr_be, 0x4);
    check("wt_sub_local", read_at(0x2108), 0x11CC2108);
    exp_rhit++;
    check("wt_sub_mem", mem_rd(0x2108), 0x11CC2108);

    // ── Write miss: no allocate — the later read must fill ──────
    sb_w = sb_writes; fs = fills_served;
    write_at(0x6100, 0xC001D00D, 0xF);
    exp_wmiss++;
    check("wna_sb", sb_writes, sb_w + 1);
    check("wna_no_fill", fills_served, fs);
    check("wna_read_fills", read_at(0x6100), 0xC001D00D);
    exp_rmiss++;
    check("wna_fill_count", fills_served, fs + 1);

    // ── Uncacheable: bypass, and no snoop of a cached copy ──────
    // 0x2104 is cached (=0xA5A5A5A5). An uncacheable write updates only
    // memory; the cached copy legitimately goes stale (the documented
    // no-alias contract) until INVAL_ALL below resyncs it.
    sb_w = sb_writes;
    write_at(0x2104, 0x0FF0F00F, 0xF, false);
    check("unc_write_sb", sb_writes, sb_w + 1);
    check("unc_write_mem", mem_rd(0x2104), 0x0FF0F00F);
    sb_r = sb_reads;
    check("unc_read_device", read_at(0x2104, false), 0x0FF0F00F);
    check("unc_read_sb", sb_reads, sb_r + 1);
    check("unc_no_snoop_stale_hit", read_at(0x2104, true), 0xA5A5A5A5);
    exp_rhit++;

    // ── INVAL_ALL: one settle cycle, then everything misses ─────
    sys_write(REG_INVAL_ALL, 1);
    fs = fills_served;
    check("inval_refetch", read_at(0x2104), 0x0FF0F00F);   // fresh from memory
    exp_rmiss++;
    check("inval_other_set", read_at(0x4400), 0x22220000 | 0x4400);
    exp_rmiss++;
    check("inval_fill_count", fills_served, fs + 2);

    // ── Faulting slot is inert ──────────────────────────────────
    sb_r = sb_reads; sb_w = sb_writes; fs = fills_served;
    access(0x7100, 0x7100, true, false, 0, 0, true, true);    // faulting read
    check("fault_read_no_wait", wait_cycles, 0);
    access(0x7100, 0x7100, false, true, 0xBAD, 0xF, true, true); // faulting write
    check("fault_write_no_wait", wait_cycles, 0);
    check("fault_no_traffic",
          sb_reads == sb_r && sb_writes == sb_w && fills_served == fs, 1);
    check("fault_no_install", read_at(0x7100), 0x44440000 | 0x7100);
    exp_rmiss++;                                   // still a miss: nothing installed

    // ── VIPT: physical tag compare under a non-identity mapping ──
    fill_gap_cfg = 0;                               // back-to-back beats this time
    check("vipt_remap_fill", read_remap(0x00003210, 0x00013210),
          0x33330000 | 0x13210);
    exp_rmiss++;
    check("vipt_remap_base", last_fill_base, 0x13210 & ~(LINE_BYTES - 1));
    check("vipt_remap_hit", read_remap(0x00003210, 0x00013210),
          0x33330000 | 0x13210);
    exp_rhit++;
    // A different vpage, same paddr: must hit — the tag is physical.
    check("vipt_other_vpage_hits", read_remap(0x00073214, 0x00013214),
          0x33330000 | 0x13214);
    exp_rhit++;
    fill_gap_cfg = 1;

    // ── Requested word arriving on the fill_done beat ───────────
    // A fresh line (0x7200) so this access really fills; the requested
    // word is the line's last, whose fill beat coincides with fill_done.
    check("last_word_capture", read_at(0x720C), 0x44440000 | 0x720C);
    exp_rmiss++;

    // ── Zero-latency single beat: completion on the resolve cycle ──
    sb_lat = 0;
    check("lat0_unc_read", read_at(0x8000, false), 0x55AA55AA);
    check("lat0_unc_wait", wait_cycles, 0);
    write_at(0x710C, 0xFEEDFACE, 0xF);             // hit; wr_done at resolve
    exp_whit++;
    check("lat0_wr_wait", wait_cycles, 0);
    check("lat0_wr_local", read_at(0x710C), 0xFEEDFACE);
    exp_rhit++;
    sb_lat = 1;

    // ── Disable again: cached lines are bypassed ────────────────
    sys_write(REG_CTRL, 0);
    sb_r = sb_reads;
    check("redisabled_pt", read_at(0x710C, true), 0xFEEDFACE);
    check("redisabled_sb", sb_reads, sb_r + 1);
    sys_write(REG_CTRL, 1);

    // ── Perfctrs vs the hand tally ──────────────────────────────
    check("perf_read_hits",    sys_read(REG_READ_HITS),    exp_rhit);
    check("perf_read_misses",  sys_read(REG_READ_MISSES),  exp_rmiss);
    check("perf_write_hits",   sys_read(REG_WRITE_HITS),   exp_whit);
    check("perf_write_misses", sys_read(REG_WRITE_MISSES), exp_wmiss);

    printf("cache_bram_vipt: %d tests, %d errors\n", tests, errors);
    dut->final();
    delete dut;
    return errors ? 1 : 0;
}
