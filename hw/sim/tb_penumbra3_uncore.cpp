// Verilator testbench for penumbra3_uncore (the gen3 uncore with no L2).
//
// Plays both L1s as requesters on the core-to-uncore port and a bus slave
// at the external bus, and checks the contract in
// doc/internals/penumbra3/uncore-port.md and
// doc/internals/penumbra3/machine.md ("The uncore with no L2"):
//   - what each transaction puts on the bus: a line read's four whole-word
//     reads from the line's address, in order and back to back; a word
//     read, a word write and a cacheable write each one access, with its
//     address and byte lanes as they are; a fetch reading all four lanes
//   - what comes back: the line, or the bus word in word 0; the fault with
//     the done, any of a line read's four; a cacheable write's done with no
//     fault and, when the bus faulted it, a store fault with its address
//     in the same cycle
//   - the handshake: the done never in the cycle a request rises, one done
//     at a time, the data side first when both wait, and a request held
//     through its done never taken twice
//
// The slave acks each access LAT cycles after it is presented and faults
// any address in the 0xFFFE0000 page.

#include <cstdio>
#include <cstdint>
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

// ── The bus slave ────────────────────────────────────────────────
static const int LAT = 2;
struct Beat { uint32_t addr; int we; uint32_t be, wdata; };
static std::vector<Beat> beats;
static int waited;
// Strobe cycles, and the first and last cycle with a strobe: a burst with
// no gap presents an access on every cycle from its first to its last.
static int cycle, strobed, first_strobe = -1, last_strobe = -1;
static int gaps() {
    return first_strobe < 0 ? 0 : (last_strobe - first_strobe + 1) - strobed;
}

static bool     faults(uint32_t a) { return (a >> 16) == 0xFFFE; }
static uint32_t mem(uint32_t a)    { return 0xC0DE0000u ^ (a & ~3u); }

// The slave, for the cycle a clock edge has just begun.
static void bus() {
    d->i_bus_ack = 0; d->i_bus_fault = 0; d->i_bus_rdata = 0;
    cycle++;
    if (d->o_bus_re || d->o_bus_we) {
        strobed++;
        if (first_strobe < 0) first_strobe = cycle;
        last_strobe = cycle;
        if (++waited >= LAT) {
            waited = 0;
            uint32_t a = d->o_bus_addr;
            d->i_bus_ack = 1;
            d->i_bus_fault = faults(a);
            d->i_bus_rdata = mem(a);
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
    d->eval();
}

// ── A requester ──────────────────────────────────────────────────
// What a transaction's done showed, and how it got there.
struct Done {
    int      cycles = -1;       // from the request rising to its done
    int      other = 0;         // the other side's done meanwhile
    int      fault = 0;
    uint32_t line[4] = {0, 0, 0, 0};
    int      store_fault = 0;
    uint32_t store_fault_addr = 0;
    int      store_fault_early = 0;  // a store fault before the done
};

// Raise a request in this cycle and hold it through its done; leaves the
// DUT in the cycle after the done with the request dropped.
static Done hold_to_done(bool dside) {
    Done r;
    for (int c = 0; c < 200; c++) {
        bool done  = dside ? d->o_drsp_done : d->o_irsp_done;
        bool other = dside ? d->o_irsp_done : d->o_drsp_done;
        r.other += other;
        if (d->o_store_fault && !done) r.store_fault_early = 1;
        if (done) {
            r.cycles = c;
            r.fault = d->o_rsp_fault;
            for (int k = 0; k < 4; k++) r.line[k] = d->o_rsp_line[k];
            r.store_fault = d->o_store_fault;
            r.store_fault_addr = d->o_store_fault_addr;
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

static Done ireq(bool cacheable, uint32_t addr) {
    d->i_ireq_valid = 1; d->i_ireq_cacheable = cacheable; d->i_ireq_addr = addr;
    d->eval();
    return hold_to_done(false);
}

static void settle() {
    idle();
    for (int i = 0; i < 4; i++) tick();
    beats.clear();
    strobed = 0; first_strobe = last_strobe = -1;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    d = new Dut;
    idle();
    d->i_rst = 1; tick(); tick(); d->i_rst = 0; d->eval();
    check("rst_idone", d->o_irsp_done, 0);
    check("rst_ddone", d->o_drsp_done, 0);
    check("rst_bus",   d->o_bus_re | d->o_bus_we, 0);
    settle();

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
            snprintf(n, sizeof(n), "dl_line%d", k);
            check(n, r.line[k], mem(0x00005670 + 4 * k));
        }
        check("dl_no_gaps", gaps(), 0);
        check("dl_fault",   r.fault, 0);
        check("dl_other",   r.other, 0);
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

    // ── A word write: one write, as it is ────────────────────────
    {
        Done r = dreq(false, 0x00008001, true, 0x2, 0x0000AB00);
        check("ww_beats", beats.size(), 1);
        check("ww_addr",  beats.size() ? beats[0].addr : 0, 0x00008001);
        check("ww_write", beats.size() ? beats[0].we : 0, 1);
        check("ww_be",    beats.size() ? beats[0].be : 0, 0x2);
        check("ww_data",  beats.size() ? beats[0].wdata : 0, 0x0000AB00);
        check("ww_fault", r.fault, 0);
        check("ww_no_sf", r.store_fault, 0);
        settle();
    }

    // ── A cacheable write: one write, as it is ───────────────────
    {
        Done r = dreq(true, 0x00009004, true, 0xF, 0x12345678);
        check("cw_beats", beats.size(), 1);
        check("cw_addr",  beats.size() ? beats[0].addr : 0, 0x00009004);
        check("cw_data",  beats.size() ? beats[0].wdata : 0, 0x12345678);
        check("cw_fault", r.fault, 0);
        check("cw_no_sf", r.store_fault, 0);
        settle();
    }

    // ── Faults ───────────────────────────────────────────────────
    {
        Done r = dreq(false, 0xFFFE0010, false, 0xF, 0);
        check("fw_read_fault", r.fault, 1);
        settle();
        r = dreq(false, 0xFFFE0020, true, 0xF, 0x55);
        check("fw_write_fault", r.fault, 1);
        check("fw_write_no_sf", r.store_fault, 0);
        settle();
        r = ireq(true, 0xFFFE0030);
        check("fl_line_fault",  r.fault, 1);
        check("fl_line_beats",  beats.size(), 4);    // the whole line still read
        settle();

        // A cacheable write the bus faults: no fault on its done, a store
        // fault with its address in the same cycle, and none before.
        r = dreq(true, 0xFFFE0044, true, 0x3, 0x7777);
        check("sf_done_no_fault", r.fault, 0);
        check("sf_store_fault",   r.store_fault, 1);
        check("sf_addr",          r.store_fault_addr, 0xFFFE0044);
        check("sf_not_early",     r.store_fault_early, 0);
        check("sf_one_cycle",     d->o_store_fault, 0);
        settle();

        // The next transaction carries no fault from it.
        r = dreq(false, 0x0000A000, false, 0xF, 0);
        check("sf_after_clean", r.fault | r.store_fault, 0);
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

    printf("%s: %d/%d checks passed\n", errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
