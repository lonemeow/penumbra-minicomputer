// Verilator testbench for penumbra3_store_buffer (16 entries).
//
// Plays the core and the uncore around the buffer and checks it against
// its contract (the module's header):
//   - a store is registered as it enters and offered for posting from the
//     second cycle after; the oldest queued store is offered
//   - a posted store is offered to the bus, oldest first, until the bus
//     master takes it, and frees when its write completes
//   - o_posted while a posted store owes its write; o_empty with nothing
//     entered, queued or posted
//   - o_line_queued for a line any queued store holds, the entering one
//     included, and for no line only posted stores hold
//   - o_full exactly when every entry is taken, counting the entering
//     store, so a store entering while it is low always finds an entry
// Directed cases pin the latencies; a long random run against a reference
// model covers the orderings, simultaneous events and wrap-around.
//
// A failure names the cycle, the step under check, the output, and the
// model's view of the buffer with what the last clock edge did to it.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include "Vpenumbra3_store_buffer.h"
#include "verilated.h"

typedef Vpenumbra3_store_buffer Dut;

static const int ENTRIES = 16;

// ── The reference model ──────────────────────────────────────────
struct Store { uint32_t addr, wdata, byte_en; bool posted; };
struct Model {
    std::deque<Store> ring;     // oldest first: posted ones, then queued
    bool  in_valid = false;     // the input stage
    Store in{};
    bool  sent = false;         // the bus master has taken the oldest posted

    const Store* oldest_queued() const {
        for (const Store& s : ring) if (!s.posted) return &s;
        return nullptr;
    }
    bool any_posted() const { return !ring.empty() && ring.front().posted; }
    bool line_queued(uint32_t line) const {
        if (in_valid && (in.addr >> 4) == line) return true;
        for (const Store& s : ring) if (!s.posted && (s.addr >> 4) == line) return true;
        return false;
    }
    // The clock edge, with this cycle's inputs.
    void step(bool enq, const Store& e, bool posted, bool taken, bool done) {
        if (done) ring.pop_front();
        if (posted)
            for (Store& s : ring) if (!s.posted) { s.posted = true; break; }
        sent = !done && (sent || taken);
        if (in_valid) ring.push_back(in);
        in_valid = enq;
        if (enq) { in = e; in.posted = false; }
    }
};

static Model m;

// ── Reporting ────────────────────────────────────────────────────
static const int MAX_SHOWN = 20;
static int errors = 0, tests = 0;
static int cycle = 0;                      // clock edges since reset
static std::string scene;                  // the step under check
static std::string last_edge = "reset";    // what the last edge's inputs did
static int shown_cycle = -1;               // the model is shown once a cycle

static std::string hex(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%X", v);
    return b;
}

// The model in the contract's terms: an entering store is queued and
// takes an entry, though it is not in the ring yet.
static std::string model_view() {
    int taken = (int)m.ring.size() + (m.in_valid ? 1 : 0);
    int posted = 0, queued = m.in_valid ? 1 : 0;
    for (const Store& s : m.ring) (s.posted ? posted : queued)++;
    std::string queued_at;
    if (const Store* q = m.oldest_queued()) queued_at = "oldest " + hex(q->addr);
    if (m.in_valid) queued_at += (queued_at.empty() ? "" : ", ") + hex(m.in.addr) + " entering";
    std::string v = std::to_string(taken) + "/" + std::to_string(ENTRIES) + " entries taken; ";
    v += "queued " + std::to_string(queued);
    if (!queued_at.empty()) v += " (" + queued_at + ")";
    v += "; posted " + std::to_string(posted);
    if (m.sent) v += " (the oldest's write in flight)";
    return v;
}

static void report(const std::string& what, const std::string& got, const std::string& exp) {
    if (errors < MAX_SHOWN) {
        printf("  FAIL cycle %d, %s: %s = %s, expected %s\n",
               cycle, scene.c_str(), what.c_str(), got.c_str(), exp.c_str());
        if (shown_cycle != cycle) {
            printf("       model: %s\n", model_view().c_str());
            printf("       last edge: %s\n", last_edge.c_str());
            shown_cycle = cycle;
        }
    }
    errors++;
}
static void check_flag(const std::string& what, bool got, bool exp) {
    tests++;
    if (got != exp) report(what, got ? "1" : "0", exp ? "1" : "0");
}
static void check_word(const std::string& what, uint32_t got, uint32_t exp) {
    tests++;
    if (got != exp) report(what, hex(got), hex(exp));
}

// ── Driving ──────────────────────────────────────────────────────
static void tick(Dut* d) {
    d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval();
    cycle++;
}

static std::string describe(bool enq, const Store& e, bool posted, bool taken, bool done) {
    std::string s;
    auto add = [&](const std::string& x) { s += (s.empty() ? "" : ", ") + x; };
    if (enq)    add("a store to " + hex(e.addr) + " entered");
    if (posted) add("the uncore posted");
    if (taken)  add("the bus master took");
    if (done)   add("a write completed");
    return s.empty() ? "nothing" : s;
}

static void drive(Dut* d, bool enq, const Store& e, bool posted, bool taken, bool done) {
    d->i_enq = enq; d->i_enq_addr = e.addr; d->i_enq_wdata = e.wdata; d->i_enq_byte_en = e.byte_en;
    d->i_posted = posted; d->i_bus_taken = taken; d->i_bus_done = done;
    d->eval();
}
static void step(Dut* d, bool enq, const Store& e, bool posted, bool taken, bool done) {
    drive(d, enq, e, posted, taken, done);
    tick(d);
    m.step(enq, e, posted, taken, done);
    last_edge = describe(enq, e, posted, taken, done);
    drive(d, false, Store{}, false, false, false);
}

// Every output against the model, and the line search at `line`.
static void check_all(Dut* d, uint32_t line) {
    d->i_line = line; d->eval();
    const Store* q = m.oldest_queued();
    check_flag("o_post_valid", d->o_post_valid, q != nullptr);
    if (q) {
        check_word("o_post_addr", d->o_post_addr, q->addr & ~3u);
        check_word("o_post_wdata", d->o_post_wdata, q->wdata);
        check_word("o_post_byte_en", d->o_post_byte_en, q->byte_en);
    }
    bool bus = m.any_posted() && !m.sent;
    check_flag("o_bus_valid", d->o_bus_valid, bus);
    if (bus) {
        const Store& h = m.ring.front();
        check_word("o_bus_addr", d->o_bus_addr, h.addr & ~3u);
        check_word("o_bus_wdata", d->o_bus_wdata, h.wdata);
        check_word("o_bus_byte_en", d->o_bus_byte_en, h.byte_en);
    }
    check_flag("o_posted", d->o_posted, m.any_posted());
    check_flag("o_empty", d->o_empty, m.ring.empty() && !m.in_valid);
    check_flag("o_full", d->o_full, (int)m.ring.size() + (m.in_valid ? 1 : 0) == ENTRIES);
    check_flag("o_line_queued for line " + hex(line << 4), d->o_line_queued, m.line_queued(line));
}

static Store store(uint32_t addr, uint32_t wdata, uint32_t byte_en) {
    return Store{addr, wdata, byte_en, false};
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* d = new Dut;

    drive(d, false, Store{}, false, false, false);
    d->i_rst = 1; tick(d); tick(d); d->i_rst = 0; d->eval();
    cycle = 0;
    scene = "after reset";
    check_all(d, 0);

    // ── One store through every state ────────────────────────────
    const Store a = store(0x00012344, 0xA1A2A3A4, 0x6);
    step(d, true, a, false, false, false);
    scene = "store A in the input stage";            // not offered yet
    check_all(d, a.addr >> 4);

    step(d, false, Store{}, false, false, false);
    scene = "store A queued in the ring";            // offered
    check_all(d, a.addr >> 4);

    step(d, false, Store{}, true, false, false);
    scene = "store A posted";                        // on the bus, line not queued
    check_all(d, a.addr >> 4);

    step(d, false, Store{}, false, true, false);
    scene = "store A taken by the bus master";
    check_all(d, 0);

    step(d, false, Store{}, false, false, false);
    scene = "store A's write in flight";
    check_all(d, 0);

    step(d, false, Store{}, false, false, true);
    scene = "store A's write done";                  // empty again
    check_all(d, 0);

    // ── Order: posts and bus writes oldest first ─────────────────
    const Store s[3] = { store(0x100, 1, 0xF), store(0x204, 2, 0x1), store(0x308, 3, 0x8) };
    for (const Store& x : s) step(d, true, x, false, false, false);
    step(d, false, Store{}, false, false, false);
    for (int i = 0; i < 3; i++) {
        scene = "posting three stores, store " + std::to_string(i) + "'s turn";
        check_word("o_post_addr (oldest queued first)", d->o_post_addr, s[i].addr);
        step(d, false, Store{}, true, false, false);
    }
    for (int i = 0; i < 3; i++) {
        scene = "writing three stores, store " + std::to_string(i) + "'s turn";
        check_word("o_bus_addr (oldest posted first)", d->o_bus_addr, s[i].addr);
        step(d, false, Store{}, false, true, false);
        step(d, false, Store{}, false, false, true);
    }
    scene = "three stores written";
    check_all(d, 0);

    // ── Full, exactly ────────────────────────────────────────────
    for (int i = 0; i < ENTRIES; i++) {
        scene = "filling, " + std::to_string(i) + " stores in";
        check_flag("o_full (room for one more)", d->o_full, 0);
        step(d, true, store(0x1000 + 4 * i, i, 0xF), false, false, false);
    }
    scene = "filled, the last store in the input stage";
    check_all(d, 0x100);

    // A post frees nothing; a completed write frees an entry.
    step(d, false, Store{}, true, false, false);
    scene = "filled, one store posted";
    check_flag("o_full (a post frees nothing)", d->o_full, 1);
    step(d, false, Store{}, false, true, false);
    step(d, false, Store{}, false, false, true);
    scene = "filled, one write completed";
    check_all(d, 0x100);

    // An entry entering as one frees keeps the buffer one short of full.
    step(d, false, Store{}, true, false, false);
    step(d, false, Store{}, false, true, false);
    step(d, true, store(0x2000, 0x55, 0xF), false, false, true);
    scene = "one short of full, a store entering as a write completed";
    check_all(d, 0x200);

    // Drain everything left.
    scene = "draining";
    for (int guard = 0; guard < 200 && !(m.ring.empty() && !m.in_valid); guard++) {
        bool posted = d->o_post_valid, taken = d->o_bus_valid, done = m.sent;
        step(d, false, Store{}, posted, taken, done);
    }
    scene = "drained";
    check_all(d, 0);

    // ── Random against the model ─────────────────────────────────
    // Lines from a small pool, so stores share lines and the search sees
    // queued, posted and absent lines alike. The run stops at its first
    // failing cycle: past it the buffer and the model have parted, and
    // every later difference repeats the first.
    scene = "random run";
    srand(1);
    for (int c = 0; c < 200000; c++) {
        bool enq    = !d->o_full && (rand() % 100) < 55;
        bool posted = d->o_post_valid && (rand() % 100) < 45;
        bool taken  = d->o_bus_valid && (rand() % 100) < 60;
        bool done   = m.sent && (rand() % 100) < 35;
        uint32_t line = 0x4000 + (rand() % 12);
        Store e = store((line << 4) | ((rand() % 4) << 2), (uint32_t)rand(), 1 + rand() % 15);
        uint32_t probe = 0x4000 + (rand() % 12);
        int before = errors;
        check_all(d, probe);
        if (errors != before) {
            printf("  (the random run stops at its first failing cycle)\n");
            break;
        }
        step(d, enq, e, posted, taken, done);
    }

    if (errors > MAX_SHOWN) printf("  (only the first %d failures shown)\n", MAX_SHOWN);
    printf("%s: %d/%d checks passed\n",
           errors ? "FAIL" : "PASS", tests - errors, tests);
    delete d;
    return errors ? 1 : 0;
}
