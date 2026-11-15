// Verilator testbench for penumbra3_uncore.
//
// Plays both L1s as requesters on the core-to-uncore port, a bus slave with
// a memory at the external bus, and software at the L2's cache device, and
// checks the contract in doc/internals/penumbra3/uncore-port.md and
// doc/internals/penumbra3/uncore.md:
//   - with the L2 disabled, what each transaction puts on the bus: a line
//     read's four whole-word reads from the line's address, in order and
//     back to back; a word read and a word write one access each, with
//     its address and byte lanes as they are; a fetch reading all four
//     lanes; what comes back, and the faults
//   - a store entering the buffer and reaching the bus by itself, a bus
//     fault on its write as a store fault with its address
//   - the buffer's order: posted stores ahead of every later bus access,
//     stores written in the order they entered, a line read waiting for a
//     queued store to its line, a word access for the buffer to empty, a
//     fill the L2 serves passing stores still on the bus, and the buffer
//     filling under a slow bus and taking stores again as writes complete
//   - the handshake: the done never in the cycle a request rises, one done
//     a cycle, the data side first when both wait, a request held through
//     its done never taken twice
//   - with the L2 enabled: a miss's line installed and served at i+2 to
//     either side; a store hit's bytes merged into the line, seen by a
//     read rising any cycle after the store entered; a store miss
//     installing nothing; uncacheable accesses never served from the L2; a
//     faulted read installing nothing; the lowest invalid way filled
//     first, then the tree-PLRU victim; two sides' hits back to back; a
//     hit dropped under a miss and served after it
//   - the cache device: INFO, CTRL, STATUS through the walk after reset
//     and after INVAL_ALL, accesses passing through during a walk, and the
//     counters counting each lookup once
//
// The slave acks each access lat cycles after it is presented and faults
// any address in the 0xFFFE0000 page.

#include <cstdio>
#include <cstdint>
#include <map>
#include <vector>
#include "Vpenumbra3_uncore.h"
#include "verilated.h"

typedef Vpenumbra3_uncore Dut;
static Dut* d;

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

// The L2's geometry at the module's defaults, and the cache device's
// registers (penumbra_pkg).
enum { SETS = 1024, WAYS = 4, SET_STRIDE = SETS * 16 };
enum { REG_INFO = 0, REG_CTRL = 1, REG_INVAL_ALL = 2, REG_STATUS = 6,
       REG_READ_HITS = 10, REG_READ_MISSES = 11, REG_WRITE_HITS = 12,
       REG_WRITE_MISSES = 13 };

// ── The bus slave and its memory ─────────────────────────────────
static int lat = 2;
struct Beat { uint32_t addr; int we; uint32_t be, wdata; };
static std::vector<Beat> beats;
static std::map<uint32_t, uint32_t> ram;
static int waited;
// Strobe cycles, and the first and last cycle with a strobe: a burst with
// no gap presents an access on every cycle from its first to its last.
static int cycle, strobed, first_strobe = -1, last_strobe = -1;
static int gaps() {
    return first_strobe < 0 ? 0 : (last_strobe - first_strobe + 1) - strobed;
}

static bool     faults(uint32_t a) { return (a >> 16) == 0xFFFE; }
static uint32_t mem(uint32_t a) {
    a &= ~3u;
    auto it = ram.find(a);
    return it == ram.end() ? (0xC0DE0000u ^ a) : it->second;
}
static void mem_write(uint32_t a, uint32_t be, uint32_t data) {
    uint32_t v = mem(a);
    for (int b = 0; b < 4; b++)
        if (be >> b & 1) v = (v & ~(0xFFu << 8 * b)) | (data & (0xFFu << 8 * b));
    ram[a & ~3u] = v;
}

// Store faults seen, wherever they come.
static int      store_faults;
static uint32_t store_fault_addr;

// The slave, for the cycle a clock edge has just begun.
static void bus() {
    d->i_bus_ack = 0; d->i_bus_fault = 0; d->i_bus_rdata = 0;
    cycle++;
    if (d->o_store_fault) { store_faults++; store_fault_addr = d->o_store_fault_addr; }
    if (d->o_bus_re || d->o_bus_we) {
        strobed++;
        if (first_strobe < 0) first_strobe = cycle;
        last_strobe = cycle;
        if (++waited >= lat) {
            waited = 0;
            uint32_t a = d->o_bus_addr;
            d->i_bus_ack = 1;
            d->i_bus_fault = faults(a);
            d->i_bus_rdata = mem(a);
            if (d->o_bus_we && !faults(a)) mem_write(a, d->o_bus_byte_en, d->o_bus_wdata);
            beats.push_back({a, d->o_bus_we, d->o_bus_byte_en, d->o_bus_wdata});
        }
    } else {
        waited = 0;
    }
    d->eval();
}

static void tick() {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
    bus();
}

static void idle() {
    d->i_ireq_valid = 0; d->i_ireq_cacheable = 0; d->i_ireq_addr = 0;
    d->i_dreq_valid = 0; d->i_dreq_cacheable = 0; d->i_dreq_addr = 0;
    d->i_dreq_we = 0; d->i_dreq_byte_en = 0; d->i_dreq_wdata = 0;
    d->i_store_enq = 0; d->i_store_addr = 0; d->i_store_wdata = 0; d->i_store_byte_en = 0;
    d->eval();
}

// ── Software at the cache device ─────────────────────────────────
static void sys_write(int reg, uint32_t v) {
    d->i_sys_reg = reg; d->i_sys_wdata = v; d->i_sys_we = 1; d->eval();
    tick();
    d->i_sys_we = 0; d->eval();
}
static uint32_t sys_read(int reg) {
    d->i_sys_reg = reg; d->i_sys_re = 1; d->eval();
    tick();
    d->i_sys_re = 0; d->eval();
    return d->o_sys_rdata;                  // a cycle after the read
}

// ── A requester ──────────────────────────────────────────────────
// What a transaction's done showed, and how it got there.
struct Done {
    int      cycles = -1;       // from the request rising to its done
    int      other = 0;         // the other side's done meanwhile
    int      fault = 0;
    uint32_t line[4] = {0, 0, 0, 0};
    int      store_fault_early = 0;  // a store fault up to the done
    size_t   beats_at_done = 0;      // bus accesses completed by the done
};

// Raise a request in this cycle and hold it through its done; leaves the
// DUT in the cycle after the done with the request dropped.
static Done hold_to_done(bool dside) {
    Done r;
    for (int c = 0; c < 300; c++) {
        bool done  = dside ? d->o_drsp_done : d->o_irsp_done;
        bool other = dside ? d->o_irsp_done : d->o_drsp_done;
        r.other += other;
        if (d->o_store_fault) r.store_fault_early = 1;
        if (done) {
            r.cycles = c;
            r.fault = d->o_rsp_fault;
            for (int k = 0; k < 4; k++) r.line[k] = d->o_rsp_line[k];
            r.beats_at_done = beats.size();
            tick();
            idle();
            return r;
        }
        tick();
    }
    return r;
}

static Done dreq(bool cacheable, uint32_t addr, bool we, uint32_t be, uint32_t wdata) {
    d->i_dreq_valid = 1; d->i_dreq_cacheable = cacheable; d->i_dreq_addr = addr;
    d->i_dreq_we = we; d->i_dreq_byte_en = be; d->i_dreq_wdata = wdata;
    d->eval();
    return hold_to_done(true);
}
static Done dread(uint32_t addr)  { return dreq(true, addr, false, 0xF, 0); }

// A cacheable store: one cycle on the store port, once the buffer has room.
// It completes there; the uncore writes it in the background.
static void store(uint32_t addr, uint32_t be, uint32_t v) {
    for (int c = 0; c < 2000 && d->o_store_full; c++) tick();
    d->i_store_enq = 1; d->i_store_addr = addr; d->i_store_byte_en = be; d->i_store_wdata = v;
    d->eval();
    tick();
    d->i_store_enq = 0; d->eval();
}

static Done ireq(bool cacheable, uint32_t addr) {
    d->i_ireq_valid = 1; d->i_ireq_cacheable = cacheable; d->i_ireq_addr = addr;
    d->eval();
    return hold_to_done(false);
}

// Let the buffer drain: run until the bus has been quiet for a while.
static void drain() {
    idle();
    for (int quiet = 0, c = 0; quiet < 16 && c < 5000; c++) {
        tick();
        quiet = (d->o_bus_re || d->o_bus_we) ? 0 : quiet + 1;
    }
}

// Drain, then forget the bus's history.
static void settle() {
    drain();
    beats.clear();
    strobed = 0; first_strobe = last_strobe = -1;
    store_faults = 0;
}

static void check_line(const char* tag, const Done& r, uint32_t line_addr) {
    for (int k = 0; k < 4; k++) {
        char n[48];
        snprintf(n, sizeof(n), "%s_line%d", tag, k);
        check(n, r.line[k], mem(line_addr + 4 * k));
    }
}

static uint32_t counter(int reg) { return sys_read(reg); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    d = new Dut;
    idle();
    d->i_sys_we = 0; d->i_sys_re = 0;
    d->i_rst = 1; tick(); tick(); d->i_rst = 0; d->eval();
    check("rst_idone", d->o_irsp_done, 0);
    check("rst_ddone", d->o_drsp_done, 0);
    check("rst_bus",   d->o_bus_re | d->o_bus_we, 0);
    check("rst_walking", sys_read(REG_STATUS), 1);     // the walk after reset
    check("rst_ctrl",    sys_read(REG_CTRL), 0);
    settle();

    // ═══ The L2 disabled: every access passes through ═══════════

    // ── A fetch's word read: one read, all four lanes ────────────
    {
        Done r = ireq(false, 0x00001234);
        check("iw_done",   r.cycles > 0, 1);       // never in the cycle it rose
        check("iw_beats",  beats.size(), 1);
        check("iw_addr",   beats.size() ? beats[0].addr : 0, 0x00001234);
        check("iw_read",   beats.size() ? beats[0].we : 1, 0);
        check("iw_be",     beats.size() ? beats[0].be : 0, 0xF);
        check("iw_word",   r.line[0], mem(0x00001234));
        check("iw_fault",  r.fault, 0);
        check("iw_other",  r.other, 0);
        settle();
    }

    // ── A line read: four whole words from the line, back to back ─
    {
        Done r = dreq(true, 0x0000567C, false, 0x2, 0);   // a byte load mid-line
        check("dl_beats", beats.size(), 4);
        for (int k = 0; k < 4 && k < (int)beats.size(); k++) {
            char n[32];
            snprintf(n, sizeof(n), "dl_addr%d", k);
            check(n, beats[k].addr, 0x00005670 + 4 * k);
            snprintf(n, sizeof(n), "dl_be%d", k);
            check(n, beats[k].be, 0xF);
        }
        check_line("dl", r, 0x00005670);
        check("dl_no_gaps", gaps(), 0);
        check("dl_fault",   r.fault, 0);
        check("dl_other",   r.other, 0);
        settle();
        r = dread(0x00005670);                      // disabled: no line kept
        check("dl_again_beats", beats.size(), 4);
        settle();
    }

    // ── A fetch's line read ──────────────────────────────────────
    {
        Done r = ireq(true, 0x00006008);
        check("il_beats", beats.size(), 4);
        check("il_addr0", beats.size() ? beats[0].addr : 0, 0x00006000);
        check("il_line2", r.line[2], mem(0x00006008));
        settle();
    }

    // ── A word read on the data side keeps its lanes ─────────────
    {
        Done r = dreq(false, 0x00007002, false, 0xC, 0);   // a halfword load
        check("dw_beats", beats.size(), 1);
        check("dw_addr",  beats.size() ? beats[0].addr : 0, 0x00007002);
        check("dw_be",    beats.size() ? beats[0].be : 0, 0xC);
        check("dw_word",  r.line[0], mem(0x00007002));
        settle();
    }

    // ── A word write: one write, as it is, done once the bus took it ─
    {
        Done r = dreq(false, 0x00008001, true, 0x2, 0x0000AB00);
        check("ww_beats_at_done", r.beats_at_done, 1);
        check("ww_addr",  beats.size() ? beats[0].addr : 0, 0x00008001);
        check("ww_write", beats.size() ? beats[0].we : 0, 1);
        check("ww_be",    beats.size() ? beats[0].be : 0, 0x2);
        check("ww_data",  beats.size() ? beats[0].wdata : 0, 0x0000AB00);
        check("ww_fault", r.fault, 0);
        settle();
        check("ww_no_sf", store_faults, 0);
    }

    // ── A store: entered, then written by itself ─────────────────
    {
        store(0x00009004, 0xF, 0x12345678);
        check("cw_entered_unwritten", beats.size(), 0);
        for (int i = 0; i < 16; i++) tick();
        check("cw_beats", beats.size(), 1);
        check("cw_addr",  beats.size() ? beats[0].addr : 0, 0x00009004);
        check("cw_write", beats.size() ? beats[0].we : 0, 1);
        check("cw_data",  beats.size() ? beats[0].wdata : 0, 0x12345678);
        check("cw_no_sf", store_faults, 0);
        settle();
    }

    // ── Faults ───────────────────────────────────────────────────
    {
        Done r = dreq(false, 0xFFFE0010, false, 0xF, 0);
        check("fw_read_fault", r.fault, 1);
        settle();
        r = dreq(false, 0xFFFE0020, true, 0xF, 0x55);
        check("fw_write_fault", r.fault, 1);
        settle();
        check("fw_write_no_sf", store_faults, 0);
        r = ireq(true, 0xFFFE0030);
        check("fl_line_fault",  r.fault, 1);
        check("fl_line_beats",  beats.size(), 4);    // the whole line still read
        settle();

        // A store the bus faults: a store fault with its address, once.
        store(0xFFFE0044, 0x3, 0x7777);
        for (int i = 0; i < 16; i++) tick();
        check("sf_store_fault",   store_faults, 1);
        check("sf_addr",          store_fault_addr, 0xFFFE0044);
        settle();

        // The next transaction carries no fault from it.
        r = dreq(false, 0x0000A000, false, 0xF, 0);
        check("sf_after_clean", r.fault, 0);
        settle();
        check("sf_after_no_sf", store_faults, 0);
    }

    // ── Stores go ahead of later bus accesses ────────────────────
    {
        store(0x0000E004, 0x3, 0x0000BEEF);
        Done r = dreq(false, 0x0000E004, false, 0xF, 0);    // a word read of it
        check("ord_write_first", beats.size() >= 1 ? beats[0].we : 0, 1);
        check("ord_read_second", beats.size() >= 2 ? beats[1].we : 1, 0);
        check("ord_read_sees",   r.line[0], mem(0x0000E004));
        check("ord_read_value",  r.line[0] & 0xFFFF, 0xBEEF);
        settle();

        // A line read follows it as well.
        store(0x0000E108, 0xF, 0x11112222);
        Done l = ireq(true, 0x0000E100);
        check("ord_line_after",  beats.size() >= 1 ? beats[0].we : 0, 1);
        check("ord_line_sees",   l.line[2], 0x11112222);
        settle();

        // Stores entering back to back are written in that order, and a word
        // read behind them reaches the bus after the last.
        for (int k = 0; k < 6; k++) store(0x0000E200 + 4 * k, 0xF, 0xAAAA0000 + k);
        Done w = dreq(false, 0x0000E300, false, 0xF, 0);
        check("ord_six_then_read", beats.size(), 7);
        for (int k = 0; k < 6 && k < (int)beats.size(); k++) {
            char n[32];
            snprintf(n, sizeof(n), "ord_six_addr%d", k);
            check(n, beats[k].addr, 0x0000E200 + 4 * k);
            snprintf(n, sizeof(n), "ord_six_data%d", k);
            check(n, beats[k].wdata, 0xAAAA0000 + k);
        }
        check("ord_read_last", beats.size() == 7 ? beats[6].we : 1, 0);
        check("ord_read_clean", w.fault, 0);
        settle();
    }

    // ── A slow bus fills the buffer; it takes stores as writes end ─
    {
        const int N = 16;
        lat = 30;
        for (int k = 0; k < N; k++) {
            char n[32];
            snprintf(n, sizeof(n), "full_room%d", k);
            check(n, d->o_store_full, 0);
            store(0x0000F000 + 4 * k, 0xF, 0xF0F00000 + k);
        }
        check("full_after_16", d->o_store_full, 1);
        store(0x0000F000 + 4 * N, 0xF, 0xF0F00000 + N);   // waits for a write to end
        check("full_waited", beats.size() >= 1, 1);
        drain();
        lat = 2;
        check("full_all_written", beats.size(), N + 1);
        bool in_order = beats.size() == N + 1;
        for (int k = 0; k <= N && in_order; k++)
            in_order = beats[k].addr == 0x0000F000u + 4 * k && beats[k].wdata == 0xF0F00000u + k;
        check("full_in_order", in_order, 1);
        settle();
    }

    // ── Both sides waiting: the data side first ──────────────────
    {
        d->i_ireq_valid = 1; d->i_ireq_cacheable = false; d->i_ireq_addr = 0x0000B000;
        d->i_dreq_valid = 1; d->i_dreq_cacheable = false; d->i_dreq_addr = 0x0000C000;
        d->i_dreq_we = 0; d->i_dreq_byte_en = 0xF; d->eval();
        int ddone_at = -1, idone_at = -1, both = 0;
        for (int c = 0; c < 100 && idone_at < 0; c++) {
            if (d->o_drsp_done && d->o_irsp_done) both = 1;
            if (d->o_drsp_done) ddone_at = c;
            if (d->o_irsp_done) idone_at = c;
            tick();
            if (ddone_at == c) { d->i_dreq_valid = 0; d->eval(); }
        }
        idle();
        check("both_d_first",  beats.size() >= 1 ? beats[0].addr : 0, 0x0000C000);
        check("both_i_second", beats.size() >= 2 ? beats[1].addr : 0, 0x0000B000);
        check("both_order",    ddone_at >= 0 && idone_at > ddone_at, 1);
        check("both_one_done", both, 0);
        check("both_two_reads", beats.size(), 2);
        settle();
    }

    // ── A request held through its done is taken once ────────────
    // The request is still up in its done's cycle, as the port requires.
    // The uncore must not take it again there: the cycle after the done
    // starts no bus access.
    {
        d->i_dreq_valid = 1; d->i_dreq_cacheable = false; d->i_dreq_addr = 0x0000D000;
        d->i_dreq_we = 0; d->i_dreq_byte_en = 0xF; d->eval();
        int c = 0;
        while (!d->o_drsp_done && c < 100) { tick(); c++; }
        check("held_one_access", beats.size(), 1);
        tick();                                  // the cycle after the done
        check("held_not_retaken", d->o_bus_re | d->o_bus_we, 0);
        idle();
        settle();
    }

    // ═══ The cache device ════════════════════════════════════════
    {
        uint32_t info = (0u << 26) | (WAYS << 21) | (SETS << 6) | 4;
        check("info", sys_read(REG_INFO), info);
        int c = 0;
        while (sys_read(REG_STATUS) && c < 2 * SETS) c++;
        check("walk_ends", sys_read(REG_STATUS), 0);
        sys_write(REG_CTRL, 1);
        check("ctrl_on", sys_read(REG_CTRL), 1);
        // Nothing counted while the L2 was disabled.
        check("ctr_quiet_rh", counter(REG_READ_HITS), 0);
        check("ctr_quiet_rm", counter(REG_READ_MISSES), 0);
        check("ctr_quiet_wh", counter(REG_WRITE_HITS), 0);
        check("ctr_quiet_wm", counter(REG_WRITE_MISSES), 0);
        settle();
    }

    // ═══ The L2 enabled ══════════════════════════════════════════

    // ── A miss installs; the line then hits, from either side ────
    {
        const uint32_t A = 0x00020040;
        Done r = dread(A + 8);
        check("miss_beats", beats.size(), 4);
        check_line("miss", r, A);
        settle();
        r = dread(A + 4);
        check("hit_no_beats", beats.size(), 0);
        check("hit_at",       r.cycles, 2);
        check("hit_fault",    r.fault, 0);
        check_line("hit", r, A);
        r = ireq(true, A);
        check("hit_fetch_no_beats", beats.size(), 0);
        check("hit_fetch_at",       r.cycles, 2);
        check_line("hit_fetch", r, A);
        check("ctr_rm1", counter(REG_READ_MISSES), 1);
        check("ctr_rh2", counter(REG_READ_HITS), 2);
        settle();

        // An uncacheable read of the line goes to the bus, and counts
        // nothing.
        r = dreq(false, A + 4, false, 0xF, 0);
        check("uc_of_cached_beats", beats.size(), 1);
        check("ctr_uc_quiet", counter(REG_READ_MISSES) + counter(REG_READ_HITS), 3);
        settle();
    }

    // ── A store hit: its bytes merged into the line ──────────────
    {
        const uint32_t A = 0x00020040;
        uint32_t old1 = mem(A + 4);
        store(A + 6, 0xC, 0x5A5A0000);                // the upper halfword of word 1
        Done r = dread(A);                            // straight after: a hit
        bool no_read = true;
        for (const Beat& b : beats) if (!b.we) no_read = false;
        check("wh_read_hits",  no_read, 1);
        check("wh_word1", r.line[1], (old1 & 0x0000FFFF) | 0x5A5A0000);
        check("wh_word0", r.line[0], mem(A));
        settle();
        check("wh_written_through", mem(A + 4), (old1 & 0x0000FFFF) | 0x5A5A0000);
        r = dread(A + 4);
        check("wh_line_matches_mem", r.line[1], mem(A + 4));
        check("ctr_wh1", counter(REG_WRITE_HITS), 1);
        settle();
    }

    // ── A store miss installs nothing ────────────────────────────
    {
        const uint32_t B = 0x00030080;
        store(B, 0xF, 0x0BADF00D);
        settle();
        Done r = dread(B);
        check("wm_then_miss", beats.size(), 4);
        check("wm_line_sees", r.line[0], 0x0BADF00D);
        check("ctr_wm1", counter(REG_WRITE_MISSES), 1);
        settle();
    }

    // ── A faulted line read installs nothing ─────────────────────
    {
        Done r = dread(0xFFFE0100);
        check("fault_miss", r.fault, 1);
        settle();
        r = dread(0xFFFE0100);
        check("fault_not_kept", beats.size(), 4);
        settle();
    }

    // ── Replacement: invalid ways first, then tree-PLRU ──────────
    // Five lines in one set. Four fill the ways; a hit on the first makes
    // the third the least recently used, so the fifth evicts it.
    {
        const uint32_t S = 0x00041230;
        uint32_t L[5];
        for (int k = 0; k < 5; k++) L[k] = S + k * SET_STRIDE;
        for (int k = 0; k < 4; k++) dread(L[k]);
        settle();
        for (int k = 0; k < 4; k++) {
            dread(L[k]);
            char n[32]; snprintf(n, sizeof(n), "fill_way%d_kept", k);
            check(n, beats.size(), 0);
        }
        dread(L[0]);                                  // ways in use 0,1,2,3, then 0
        settle();
        dread(L[4]);                                  // evicts L[2]
        settle();
        const int kept[4] = {0, 1, 3, 4};
        for (int k : kept) {
            dread(L[k]);
            char n[32]; snprintf(n, sizeof(n), "plru_kept%d", k);
            check(n, beats.size(), 0);
        }
        dread(L[2]);
        check("plru_evicted2", beats.size(), 4);
        settle();
    }

    // ── Two sides' hits back to back; a hit dropped under a miss ─
    {
        const uint32_t A = 0x00020040, C = 0x00050000;
        d->i_dreq_valid = 1; d->i_dreq_cacheable = 1; d->i_dreq_addr = A;
        d->i_dreq_we = 0; d->i_dreq_byte_en = 0xF;
        d->i_ireq_valid = 1; d->i_ireq_cacheable = 1; d->i_ireq_addr = A + 4;
        d->eval();
        int ddone_at = -1, idone_at = -1;
        for (int c = 0; c < 20 && idone_at < 0; c++) {
            if (d->o_drsp_done) ddone_at = c;
            if (d->o_irsp_done) idone_at = c;
            tick();
            if (ddone_at == c) { d->i_dreq_valid = 0; d->eval(); }
        }
        idle();
        check("b2b_d_at", ddone_at, 2);
        check("b2b_i_at", idone_at, 3);
        check("b2b_no_bus", beats.size(), 0);
        settle();

        // The data side misses; the fetch side's hit waits out the miss.
        // The hit dropped under the miss counts once, when it is served.
        uint32_t rh = counter(REG_READ_HITS), rm = counter(REG_READ_MISSES);
        d->i_dreq_valid = 1; d->i_dreq_cacheable = 1; d->i_dreq_addr = C;
        d->i_ireq_valid = 1; d->i_ireq_cacheable = 1; d->i_ireq_addr = A;
        d->eval();
        ddone_at = idone_at = -1;
        int both = 0;
        for (int c = 0; c < 100 && idone_at < 0; c++) {
            if (d->o_drsp_done && d->o_irsp_done) both = 1;
            if (d->o_drsp_done) ddone_at = c;
            if (d->o_irsp_done) idone_at = c;
            tick();
            if (ddone_at == c) { d->i_dreq_valid = 0; d->eval(); }
        }
        idle();
        check("hum_miss_first", ddone_at >= 0 && idone_at > ddone_at, 1);
        check("hum_one_done",   both, 0);
        check("hum_one_line",   beats.size(), 4);
        check("hum_hits_once",   counter(REG_READ_HITS) - rh, 1);
        check("hum_misses_once", counter(REG_READ_MISSES) - rm, 1);
        settle();

        // A fetch of a store's line rising any cycle from the store's entry
        // on reads the store's byte: the line search holds it back while the
        // store is queued, and issue waits out the post at the verdict and
        // its write at the update stage.
        for (int k = 0; k < 10; k++) {
            uint32_t old0 = mem(A), b = 0x10 + k;
            d->i_store_enq = 1; d->i_store_addr = A; d->i_store_byte_en = 0x1;
            d->i_store_wdata = b;
            d->eval();
            idone_at = -1;
            uint32_t iword = 0;
            for (int c = 0; c < 60 && idone_at < 0; c++) {
                if (c == k) {
                    d->i_ireq_valid = 1; d->i_ireq_cacheable = 1; d->i_ireq_addr = A;
                    d->eval();
                }
                if (d->o_irsp_done) { idone_at = c; iword = d->o_rsp_line[0]; }
                tick();
                d->i_store_enq = 0; d->eval();
            }
            idle();
            char n[40];
            snprintf(n, sizeof(n), "upd_fetch_merged_at%d", k);
            check(n, iword, (old0 & ~0xFFu) | b);
            settle();
        }

        // A fill the L2 serves passes stores still waiting for the bus.
        lat = 10;
        for (int k = 0; k < 4; k++) store(0x00060000 + 16 * k, 0xF, 0x6000 + k);
        Done r = dread(A);
        check("pass_hit_fast",    r.cycles >= 0 && r.cycles <= 4, 1);
        check("pass_stores_left", r.beats_at_done < 4, 1);
        check_line("pass", r, A);
        drain();
        lat = 2;
        check("pass_all_written", beats.size(), 4);
        settle();
    }

    // ── INVAL_ALL: busy through its walk, nothing kept meanwhile ─
    {
        const uint32_t A = 0x00020040;
        uint32_t rh = counter(REG_READ_HITS), rm = counter(REG_READ_MISSES);
        sys_write(REG_INVAL_ALL, 0);
        check("inval_busy", sys_read(REG_STATUS), 1);
        Done r = dread(A);                            // during the walk
        check("inval_walk_passes", beats.size(), 4);
        check_line("inval_walk", r, A);
        settle();
        r = dread(A);                                 // still nothing kept
        check("inval_walk_no_install", beats.size(), 4);
        check("inval_walk_uncounted", counter(REG_READ_HITS) - rh
                                    + counter(REG_READ_MISSES) - rm, 0);
        int c = 0;
        while (sys_read(REG_STATUS) && c < 2 * SETS) c++;
        check("inval_ends", sys_read(REG_STATUS), 0);
        settle();
        r = dread(A);
        check("inval_then_miss", beats.size(), 4);
        settle();
        r = dread(A);
        check("inval_then_hit", beats.size(), 0);
        settle();
    }

    // ── Disabling stops the hits ─────────────────────────────────
    {
        sys_write(REG_CTRL, 0);
        dread(0x00020040);
        check("off_passes", beats.size(), 4);
        settle();
    }

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
