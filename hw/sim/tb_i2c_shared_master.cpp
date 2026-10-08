// Verilator testbench for the I2C master several consumers share
//
// Three consumers at the master's ports, and a slave for each of them on
// the bus's two lines. The consumers are models that drive an
// i2c_master's command port as its contract has it: a command
// held until taken, one response to a command, a transaction given up on
// a write not acknowledged or a bus error. Whose a byte is shows on the
// bus: in the address a consumer talks to, in every byte it writes, and
// in every byte its slave sends back.
//
//  * On the bus, a transaction is one consumer's from START to STOP.
//  * Every byte a consumer writes reaches its own slave, once and in
//    order, and every byte it reads is the next its own slave sent.
//  * A response goes to the consumer whose command it answers and to no
//    other.
//  * A consumer waits for one transaction of each of the others at most.
//  * A consumer that gives a transaction up and then asks for nothing
//    more does not keep the others from the master.
//  * A reset part way through a transaction leaves all three served.
//
// The bus's timing is the master's own testbench's to check.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstdarg>
#include <deque>
#include "Vi2c_shared_master_test.h"
#include "i2c_slave.h"

namespace {

Vi2c_shared_master_test* dut;
int pass = 0, fail = 0;
long now = 0;
const char* scenario = "";

void check(bool ok, const char* fmt, ...) {
    if (ok) { pass++; return; }
    if (++fail <= 20) {
        va_list ap; va_start(ap, fmt);
        printf("  FAIL [cycle %ld, %s: ", now, scenario); vprintf(fmt, ap); printf("]\n");
        va_end(ap);
    }
    if (fail >= 100) {
        printf("i2c_shared_master: gave up after %d failures\n", fail);
        exit(1);
    }
}

// For a consumer left waiting: nothing after it would mean anything.
void give_up(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("  FAIL [cycle %ld, %s: ", now, scenario); vprintf(fmt, ap); printf("]\n");
    va_end(ap);
    printf("i2c_shared_master: stopped there, %d checks passed and %d failed before it\n",
           pass, fail);
    exit(1);
}

uint32_t rng = 0x2545f491;
uint32_t rnd(uint32_t n) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return n ? rng % n : 0;
}

const int N = 3;

// A whole transaction at its longest: eight bytes of nine bits at forty
// cycles a bit, and the conditions around them.
const long TRANSACTION = 8 * 9 * 40 + 200;
// A command waits for the transaction in hand and one of each of the
// others, with room to spare.
const long TAKEN_WITHIN = (N + 1) * TRANSACTION;
// A byte, with a START ahead of it and a STOP behind.
const long ANSWERED_WITHIN = 9 * 40 + 400;

// ── The slaves ───────────────────────────────────────────────
// Consumer k talks to the slave at 0x20 + k, and where it wants no
// answer to 0x30 + k, where there is none. It writes bytes k + 1 in the
// high half and a count in the low; its slave sends 8 + k and a count.
I2cSlave slave[N] = {I2cSlave(0x20), I2cSlave(0x21), I2cSlave(0x22)};
int taken_by_slave[N];          // bytes each slave has acknowledged
int queued_by_slave[N];         // bytes each has queued to send

uint8_t written_byte(int k, int n) { return (uint8_t)(((k + 1) << 4) | (n & 0xf)); }
uint8_t sent_byte(int k, int n)    { return (uint8_t)(0x80 | (k << 4) | (n & 0xf)); }

int consumer_at(uint8_t address) {
    if (address >= 0x20 && address < 0x20 + N) return address - 0x20;
    if (address >= 0x30 && address < 0x30 + N) return address - 0x30;
    return -1;
}

void slave_boundary(int k) {
    I2cSlave& s = slave[k];
    for (uint8_t b : s.written) {
        check(b == written_byte(k, taken_by_slave[k]),
              "slave %d was written %02x: consumer %d's next byte is %02x", k, b, k,
              written_byte(k, taken_by_slave[k]));
        taken_by_slave[k]++;
    }
    s.written.clear();
    while (s.to_send.size() < 8)
        s.to_send.push_back(sent_byte(k, queued_by_slave[k]++));
}

// ── The consumers ────────────────────────────────────────────
enum Role { ADDRESS, DATA, READ_BYTE };
struct Cmd {
    Role    role;
    bool    start, stop, nack;
    uint8_t byte;
};

enum Kind {
    WRITE,          // the address, up to three bytes written
    READ,           // the address, up to three bytes read
    WRITE_READ,     // bytes written, a repeated START, bytes read
    ABSENT,         // an address nothing answers: given up there
    REFUSED         // a byte the slave does not acknowledge: given up there
};
enum Eager { NEVER, SOMETIMES, ALWAYS };

struct Consumer {
    int  k = 0;

    // What it is let do.
    int   budget = 0;           // transactions it may still begin
    int   max_gap = 0;          // the longest it rests between two
    Eager eagerness = NEVER;
    std::deque<Kind> forced;    // kinds to use before choosing its own
    bool  expect_error = false; // a bus error is being caused for it

    // The transaction in hand.
    std::deque<Cmd> plan;       // its commands not yet put up
    Kind kind = WRITE;
    bool in_tx = false;
    bool first = false;         // the command up, or due, is the transaction's first
    bool eager = false;         // puts its next command up in the cycle the last is answered
    bool valid = false;         // a command is up
    Cmd  cur{};
    bool awaiting = false;      // a command taken, its response to come
    long since = 0;             // when the command went up, or was taken
    long offer_at = 0;
    long rest_until = 0;
    bool asking = false;        // its transaction's first command is up and not taken
    int  others_begun = 0;      // transactions others have begun while it asked

    // Its account with its slave.
    int  written = 0;           // bytes its slave acknowledged
    int  read = 0;              // bytes read
    int  reads_acked = 0, reads_nacked = 0;
    int  done = 0;              // transactions carried to their end, or given up unanswered
    int  errors = 0;            // transactions given up on a bus error
    uint32_t junk = 0;          // what its port carries with no command up
};
Consumer con[N];

void plan_transaction(Consumer& c, Kind kind) {
    int k = c.k;
    int writes = 0, reads = 0;
    switch (kind) {
        case WRITE:      writes = rnd(4);                       break;
        case READ:       reads  = 1 + rnd(3);                   break;
        case WRITE_READ: writes = 1 + rnd(3); reads = 1 + rnd(3); break;
        case ABSENT:     writes = 1 + rnd(2);                   break;
        case REFUSED:    writes = 1 + rnd(3);                   break;
    }
    uint8_t address = (uint8_t)((kind == ABSENT ? 0x30 : 0x20) + k);
    c.plan.clear();
    if (kind != READ) {
        c.plan.push_back({ADDRESS, true, false, false, (uint8_t)(address << 1)});
        for (int i = 0; i < writes; i++)
            c.plan.push_back({DATA, false, false, false, 0});
    }
    if (reads > 0) {
        c.plan.push_back({ADDRESS, true, false, false, (uint8_t)(address << 1 | 1)});
        for (int i = 0; i < reads; i++)
            c.plan.push_back({READ_BYTE, false, false, i == reads - 1, 0});
    }
    c.plan.back().stop = true;
    c.kind = kind;
    slave[k].ack_data = (kind != REFUSED);
}

void put_up(Consumer& c) {
    c.cur = c.plan.front();
    c.plan.pop_front();
    if (c.cur.role == DATA)
        c.cur.byte = written_byte(c.k, c.written);
    c.valid = true;
    c.since = now;
    if (c.first) {
        c.asking = true;
        c.others_begun = 0;
    }
}

void begin_if_due(Consumer& c) {
    if (c.in_tx || c.budget == 0 || now < c.rest_until)
        return;
    Kind kind;
    if (!c.forced.empty()) {
        kind = c.forced.front();
        c.forced.pop_front();
    } else {
        uint32_t r = rnd(100);
        kind = r < 30 ? WRITE : r < 60 ? READ : r < 85 ? WRITE_READ : r < 93 ? ABSENT : REFUSED;
    }
    plan_transaction(c, kind);
    c.budget--;
    c.in_tx = true;
    c.first = true;
    c.eager = (c.eagerness == ALWAYS) || (c.eagerness == SOMETIMES && rnd(2));
    put_up(c);
}

void end_transaction(Consumer& c, bool on_error) {
    c.plan.clear();
    c.in_tx = false;
    (on_error ? c.errors : c.done)++;
    c.rest_until = now + (c.max_gap ? 1 + rnd(c.max_gap) : 0);
}

void on_response(Consumer& c, uint8_t rdata, bool nack, bool error) {
    check(c.awaiting, "consumer %d is given a response with no command of its own in flight", c.k);
    if (!c.awaiting)
        return;
    c.awaiting = false;

    bool over = c.cur.stop;
    if (error) {
        check(c.expect_error, "consumer %d is told of a bus error that nothing caused", c.k);
        over = true;
    } else if (c.cur.role == READ_BYTE) {
        check(rdata == sent_byte(c.k, c.read),
              "consumer %d read %02x: the next byte its slave sent is %02x", c.k, rdata,
              sent_byte(c.k, c.read));
        c.read++;
        (c.cur.nack ? c.reads_nacked : c.reads_acked)++;
    } else {
        bool refused = (c.cur.role == ADDRESS) ? (c.kind == ABSENT) : (c.kind == REFUSED);
        check(nack == refused, "consumer %d's %s byte %02x was %s", c.k,
              c.cur.role == ADDRESS ? "address" : "data", c.cur.byte,
              nack ? "not acknowledged, and its slave takes it"
                   : "acknowledged, and nothing takes it");
        if (nack)
            over = true;
        else if (c.cur.role == DATA)
            c.written++;
    }

    if (over) {
        end_transaction(c, error);
        if (c.eager)
            begin_if_due(c);
    } else if (c.eager) {
        put_up(c);
    } else {
        c.offer_at = now + 1 + rnd(3);
    }
}

void on_taken(Consumer& c) {
    c.valid = false;
    c.awaiting = true;
    c.since = now;
    c.junk = rnd(1u << 12);
    if (!c.first)
        return;
    c.first = false;
    c.asking = false;
    for (Consumer& o : con) {
        if (o.k == c.k || !o.asking)
            continue;
        o.others_begun++;
        check(o.others_begun <= N - 1,
              "consumer %d is still asking, and the other %d have begun %d transactions since: "
              "they have one turn each", o.k, N - 1, o.others_begun);
    }
}

void drive_ports() {
    uint32_t valid = 0, start = 0, stop = 0, read = 0, nack = 0, wdata = 0;
    for (const Consumer& c : con) {
        int k = c.k;
        if (c.valid) {
            valid |= 1u << k;
            start |= (uint32_t)c.cur.start << k;
            stop  |= (uint32_t)c.cur.stop << k;
            read  |= (uint32_t)(c.cur.role == READ_BYTE) << k;
            nack  |= (uint32_t)c.cur.nack << k;
            wdata |= (uint32_t)(c.cur.role == READ_BYTE ? (c.junk & 0xff) : c.cur.byte) << (8 * k);
        } else {
            start |= (c.junk & 1) << k;
            stop  |= ((c.junk >> 1) & 1) << k;
            read  |= ((c.junk >> 2) & 1) << k;
            nack  |= ((c.junk >> 3) & 1) << k;
            wdata |= ((c.junk >> 4) & 0xff) << (8 * k);
        }
    }
    dut->i_cmd_valid = valid;
    dut->i_cmd_start = start;
    dut->i_cmd_stop  = stop;
    dut->i_cmd_read  = read;
    dut->i_cmd_nack  = nack;
    dut->i_cmd_wdata = wdata;
}

// ── The bus ──────────────────────────────────────────────────
bool hold_scl = false, hold_sda = false;    // the testbench's own grip on a line

// What the bus carries, read as any device on it would: whose address
// each transaction opens with, and that all in it is that consumer's.
struct Monitor {
    bool    open = false;
    int     owner = -1;
    bool    address_next = false;
    bool    writing = false;
    int     bits = 0;
    uint8_t byte = 0;
    bool    scl_was = true, sda_was = true;
    bool    blind = false;      // the lines are not the master's to speak for just now
};
Monitor mon;
int transactions_seen[N];

void monitor_byte() {
    if (mon.address_next) {
        int who = consumer_at(mon.byte >> 1);
        check(who >= 0, "the address %02x on the bus is no consumer's", mon.byte >> 1);
        if (mon.owner < 0) {
            mon.owner = who;
        } else {
            check(who == mon.owner,
                  "one transaction on the bus carries consumer %d's address and then consumer %d's",
                  mon.owner, who);
        }
        mon.writing = !(mon.byte & 1);
        mon.address_next = false;
    } else if (mon.owner >= 0) {
        uint8_t mark = mon.writing ? (uint8_t)(mon.owner + 1) : (uint8_t)(8 + mon.owner);
        check((mon.byte >> 4) == mark,
              "the byte %02x %s in consumer %d's transaction on the bus is not that consumer's",
              mon.byte, mon.writing ? "written" : "read", mon.owner);
    }
}

void monitor(bool scl, bool sda) {
    if (mon.blind) {
        mon.scl_was = scl; mon.sda_was = sda;
        return;
    }
    if (scl && mon.scl_was && sda != mon.sda_was) {
        if (!sda) {
            // A START, or a repeated one inside a transaction.
            if (!mon.open) {
                mon.open = true;
                mon.owner = -1;
            }
            mon.address_next = true;
            mon.bits = 0;
        } else {
            if (mon.open && mon.owner >= 0)
                transactions_seen[mon.owner]++;
            mon.open = false;
        }
    } else if (scl && !mon.scl_was && mon.open) {
        if (mon.bits == 8) {
            mon.bits = 0;       // the acknowledge
        } else {
            mon.byte = (uint8_t)((mon.byte << 1) | sda);
            if (++mon.bits == 8)
                monitor_byte();
        }
    }
    mon.scl_was = scl; mon.sda_was = sda;
}

void tick() {
    drive_ports();
    dut->i_clk = 0; dut->eval();

    // Responses first: a consumer may answer one with its next command
    // in the same cycle.
    uint32_t rsp = dut->o_rsp_valid;
    if (rsp)
        check((rsp & (rsp - 1)) == 0, "one response is given to more than one consumer (%x)", rsp);
    for (Consumer& c : con) {
        if ((rsp >> c.k) & 1)
            on_response(c, dut->o_rsp_rdata, dut->o_rsp_nack, dut->o_rsp_error);
    }
    drive_ports();
    dut->eval();

    bool taken[N];
    for (const Consumer& c : con)
        taken[c.k] = c.valid && ((dut->o_cmd_ready >> c.k) & 1);

    dut->i_clk = 1; dut->eval();

    for (I2cSlave& s : slave)
        s.tick();
    bool scl = dut->o_scl && !hold_scl;
    bool sda = dut->o_sda && !hold_sda;
    for (const I2cSlave& s : slave)
        sda = sda && !s.drive_low;
    dut->i_scl = scl; dut->i_sda = sda;
    monitor(scl, sda);
    for (I2cSlave& s : slave)
        s.observe(scl, sda);

    for (Consumer& c : con) {
        if (taken[c.k])
            on_taken(c);
        if (c.in_tx && !c.valid && !c.awaiting && now >= c.offer_at)
            put_up(c);
        begin_if_due(c);

        if (c.valid && now - c.since > TAKEN_WITHIN)
            give_up("consumer %d has had a command up for %ld cycles and the master has not "
                    "taken it", c.k, now - c.since);
        if (c.awaiting && now - c.since > ANSWERED_WITHIN)
            give_up("consumer %d's command has had no response in %ld cycles", c.k, now - c.since);
    }
    now++;
}

void run(long cycles) {
    for (long i = 0; i < cycles; i++)
        tick();
}

// The board's reset lasts 10 ms at least: 40000 cycles of this clock.
const long BOARD_RESET = 40000;

void reset() {
    for (Consumer& c : con) {
        int k = c.k;
        c = Consumer();
        c.k = k;
    }
    mon.blind = true;
    dut->i_rst = 1;
    run(BOARD_RESET);
    dut->i_rst = 0;
    hold_scl = hold_sda = false;
    for (int k = 0; k < N; k++) {
        slave[k] = I2cSlave((uint8_t)(0x20 + k));
        slave[k].on_boundary = [k] { slave_boundary(k); };
        taken_by_slave[k] = queued_by_slave[k] = 0;
        transactions_seen[k] = 0;
    }
    mon = Monitor();
}

void let(int k, int transactions, int max_gap, Eager eagerness) {
    con[k].budget = transactions;
    con[k].max_gap = max_gap;
    con[k].eagerness = eagerness;
    con[k].rest_until = now;
}

bool all_idle() {
    for (const Consumer& c : con) {
        if (c.budget > 0 || c.in_tx)
            return false;
    }
    return true;
}

// Until every consumer has done what it was let do.
void run_out() {
    long limit = now + 4000000;
    while (!all_idle()) {
        tick();
        if (now > limit)
            give_up("the consumers did not finish in four million cycles");
    }
}

// With the bus at rest: each consumer's account against its slave's and
// against the bus's.
void tally() {
    run(300);
    check(dut->i_scl && dut->i_sda, "the bus is not let go (SCL %d, SDA %d)", dut->i_scl,
          dut->i_sda);
    check(!mon.open, "a transaction is left open on the bus");
    for (int k = 0; k < N; k++) {
        const Consumer& c = con[k];
        check(slave[k].st == I2cSlave::IDLE, "slave %d is left part way through a transfer", k);
        check(taken_by_slave[k] == c.written,
              "slave %d took %d bytes, and consumer %d had %d acknowledged", k, taken_by_slave[k],
              k, c.written);
        check(slave[k].acks == c.reads_acked && slave[k].nacks == c.reads_nacked,
              "slave %d had %d bytes acknowledged and %d not; consumer %d asked for %d and %d", k,
              slave[k].acks, slave[k].nacks, k, c.reads_acked, c.reads_nacked);
        check(transactions_seen[k] == c.done,
              "the bus carried %d transactions of consumer %d, which counts %d",
              transactions_seen[k], k, c.done);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    dut = new Vi2c_shared_master_test;
    dut->i_scl = 1; dut->i_sda = 1;
    for (int k = 0; k < N; k++)
        con[k].k = k;

    // Two that ask while the master is still freeing the bus.
    scenario = "asked while the master frees the bus";
    reset();
    con[2].forced = {WRITE_READ};
    let(2, 1, 0, NEVER);
    run(4);
    con[0].forced = {WRITE_READ};
    let(0, 1, 0, NEVER);
    run_out();
    tally();

    scenario = "nobody asking";
    reset();
    run(5000);
    tally();

    for (int k = 0; k < N; k++) {
        static const char* const names[N] = {"consumer 0 alone", "consumer 1 alone",
                                             "consumer 2 alone"};
        scenario = names[k];
        let(k, 25, 200, SOMETIMES);
        run_out();
        tally();
    }

    // A consumer gives a transaction up and asks for nothing more.
    for (int quitter = 0; quitter < N; quitter++) {
        for (Kind kind : {ABSENT, REFUSED}) {
            static const char* const names[2] = {
                "the others, behind a consumer whose slave did not answer",
                "the others, behind a consumer whose byte was refused"};
            scenario = names[kind == REFUSED];
            con[quitter].forced = {kind};
            let(quitter, 1, 0, NEVER);
            run_out();
            for (int k = 0; k < N; k++) {
                if (k != quitter)
                    let(k, 3, 50, SOMETIMES);
            }
            run_out();
            tally();
        }
    }

    // A bus error at a START: the data line is held low.
    scenario = "the others, behind a consumer whose START met a held data line";
    reset();
    run(600);           // the master frees the bus first, and finds it free
    hold_sda = true;
    run(50);
    con[1].expect_error = true;
    con[1].forced = {WRITE_READ};
    let(1, 1, 0, NEVER);
    run_out();
    check(con[1].errors == 1, "consumer 1 was told of %d bus errors, not one", con[1].errors);
    hold_sda = false;
    run(200);
    let(0, 3, 50, SOMETIMES);
    let(2, 3, 50, SOMETIMES);
    run_out();
    tally();

    // The clock held under a transaction that is open.
    scenario = "the others, behind a consumer whose transaction met a held clock";
    reset();
    con[2].forced = {WRITE_READ};
    let(2, 1, 0, NEVER);
    {
        // Until its address has been answered and the next byte is due.
        long limit = now + TAKEN_WITHIN;
        while ((con[2].first || con[2].awaiting || !con[2].in_tx) && now < limit)
            tick();
    }
    con[2].expect_error = true;
    hold_scl = true;
    mon.blind = true;
    run_out();
    check(con[2].errors == 1, "consumer 2 was told of %d bus errors, not one", con[2].errors);
    hold_scl = false;
    run(200);
    mon = Monitor();
    let(0, 3, 50, SOMETIMES);
    let(1, 3, 50, SOMETIMES);
    run_out();
    check(con[0].done == 3 && con[1].done == 3,
          "consumers 0 and 1 carried %d and %d transactions through, not three each", con[0].done,
          con[1].done);

    scenario = "all three coming and going";
    reset();
    for (int k = 0; k < N; k++)
        let(k, 300, 3000, SOMETIMES);
    run_out();
    tally();

    scenario = "all three without a pause";
    for (int k = 0; k < N; k++)
        let(k, 150, 0, ALWAYS);
    run_out();
    tally();

    // A reset with a transaction open.
    scenario = "behind a reset part way through a transaction";
    reset();
    for (int k = 0; k < N; k++)
        let(k, 50, 20, SOMETIMES);
    {
        long limit = now + 40 * TRANSACTION;
        while (now < limit) {
            tick();
            int done = con[0].done + con[1].done + con[2].done;
            if (done >= 7 && mon.open && mon.bits == 4)
                break;
        }
        check(mon.open, "no transaction was open to reset under");
    }
    reset();
    for (int k = 0; k < N; k++)
        let(k, 20, 100, SOMETIMES);
    run_out();
    tally();

    printf("i2c_shared_master: %d/%d tests passed\n", pass, pass + fail);
    if (fail > 0)
        printf("  *** %d FAILED ***\n", fail);

    delete dut;
    return (fail > 0) ? 1 : 0;
}
