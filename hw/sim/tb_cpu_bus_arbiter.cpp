// Verilator testbench for cpu_bus_arbiter
//
// Tests the documented contract (from cpu_bus_arbiter.sv header):
//   FSM/handshake:
//     - IDLE → BUSY on either pending; D wins on simultaneous.
//     - Owner latched at every latch event (IDLE→BUSY or
//       BUSY→BUSY-with-!i_mem_busy).  Owner stable mid-transaction
//       (S_BUSY with i_mem_busy=1).
//     - External bus (o_mem_re/we) is driven only in S_BUSY.
//     - Zero idle bus cycles between back-to-back transactions —
//       the headline property of the A2 redesign.
//
//   Response routing:
//     - The current owner sees o_*_busy=0 on the cycle i_mem_busy
//       drops in S_BUSY; that's when the cache captures rdata.
//     - The non-owner with a pending request sees o_*_busy=1
//       throughout S_BUSY (driven by its own pending bit), so it
//       cannot mis-capture the owner's response (both rdata ports
//       are wired to i_mem_rdata).
//
//   Request-accepted handshake:
//     - o_d_req_accepted / o_i_req_accepted are 1-cycle
//       combinational pulses that fire iff the arbiter is about
//       to latch that owner's request on the next clock edge
//       (either entering S_BUSY from S_IDLE, or re-latching on
//       a busy-drop cycle in S_BUSY).  Mutually exclusive (D
//       wins simultaneous pending).  Tested end-to-end via
//       test_req_accepted_pulse; the in-module SVAs pin down
//       the remaining cases.
//
// The "external bus" is mocked in C++: we drive i_mem_busy /
// i_mem_rdata in response to o_mem_re / o_mem_we and verify the
// arbiter routes the response to the correct port.

#include <cstdio>
#include <cstdint>
#include "Vcpu_bus_arbiter.h"

static int errors = 0, tests = 0;

static void eval_only(Vcpu_bus_arbiter* d) { d->eval(); }

static void tick(Vcpu_bus_arbiter* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vcpu_bus_arbiter* d) {
    d->i_rst = 1;
    d->i_d_addr = 0;
    d->i_d_wdata = 0;
    d->i_d_byte_en = 0;
    d->i_d_we = 0;
    d->i_d_re = 0;
    d->i_i_addr = 0;
    d->i_i_re = 0;
    d->i_mem_rdata = 0;
    d->i_mem_busy = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
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

// Run the external "memory" for one cycle.
//   When o_mem_re or o_mem_we fires (we're in S_BUSY), assert
//   i_mem_busy for `latency` cycles, then drop busy and present
//   `rdata`.  Returns true if a transaction was just served.
//
// The C++ side keeps simple FSM state mirroring a synchronous device.
struct MemMock {
    int       busy_remaining = 0;
    uint32_t  pending_rdata  = 0;
    bool      have_request   = false;
    uint32_t  captured_addr  = 0;
    bool      captured_we    = false;
    uint32_t  captured_wdata = 0;

    void clock_edge(Vcpu_bus_arbiter* d, int latency, uint32_t rdata_for_addr) {
        // Sample bus inputs at the rising edge.
        if (!have_request && (d->o_mem_re || d->o_mem_we)) {
            have_request   = true;
            captured_addr  = d->o_mem_addr;
            captured_we    = d->o_mem_we;
            captured_wdata = d->o_mem_wdata;
            busy_remaining = latency;
            pending_rdata  = rdata_for_addr;
        }
        if (have_request) {
            if (busy_remaining > 0) {
                d->i_mem_busy = 1;
                d->i_mem_rdata = 0xDEAD'BEEFu;  // poison while busy
                busy_remaining--;
            }
            if (busy_remaining == 0) {
                d->i_mem_busy = 0;
                d->i_mem_rdata = pending_rdata;
                have_request = false;
            }
        } else {
            d->i_mem_busy = 0;
            d->i_mem_rdata = 0;
        }
    }
};

// Tick once with the mock present.  Caller sets the rdata that
// the mock should return for whatever new request arrives.
static void tick_with_mem(Vcpu_bus_arbiter* d, MemMock& mem,
                          int latency, uint32_t rdata) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
    mem.clock_edge(d, latency, rdata);
    d->eval();
}

// ══════════════════════════════════════════════════════════════
// Tests
// ══════════════════════════════════════════════════════════════

static void test_single_d_read(Vcpu_bus_arbiter* d) {
    printf("── Single D read: request, response, deliver ──\n");
    reset(d);
    MemMock mem;

    // Issue D read at address 0x100.
    d->i_d_addr = 0x100;
    d->i_d_re   = 1;
    d->eval();

    // The arbiter should assert o_d_busy immediately (pick_d=1, S_IDLE).
    check_bool("d_read.busy_high_on_request", d->o_d_busy, true);
    check_bool("d_read.i_busy_low_idle", d->o_i_busy, false);

    // Drive ticks; mock memory has 2-cycle latency, returns 0xCAFEBABE.
    int safety = 0;
    while (d->o_d_busy && safety++ < 20) {
        tick_with_mem(d, mem, 2, 0xCAFE'BABEu);
    }

    check_bool("d_read.eventually_done", d->o_d_busy, false);
    check("d_read.rdata", d->o_d_rdata, 0xCAFE'BABEu);

    // Drop the request.
    d->i_d_re = 0;
    tick_with_mem(d, mem, 2, 0);
}

static void test_single_i_read(Vcpu_bus_arbiter* d) {
    printf("── Single I read: request, response, deliver ──\n");
    reset(d);
    MemMock mem;

    d->i_i_addr = 0x200;
    d->i_i_re   = 1;
    d->eval();

    check_bool("i_read.busy_high_on_request", d->o_i_busy, true);
    check_bool("i_read.d_busy_low_idle", d->o_d_busy, false);

    int safety = 0;
    while (d->o_i_busy && safety++ < 20) {
        tick_with_mem(d, mem, 3, 0x1234'5678u);
    }

    check_bool("i_read.eventually_done", d->o_i_busy, false);
    check("i_read.rdata", d->o_i_rdata, 0x1234'5678u);

    d->i_i_re = 0;
    tick_with_mem(d, mem, 0, 0);
}

static void test_d_priority_on_simultaneous(Vcpu_bus_arbiter* d) {
    printf("── Simultaneous D+I requests: D wins, I waits ──\n");
    reset(d);
    MemMock mem;

    // Raise both requests at the exact same cycle.
    d->i_d_addr = 0x400;
    d->i_d_re   = 1;
    d->i_i_addr = 0x800;
    d->i_i_re   = 1;
    d->eval();

    // S_IDLE busy mux: pick_d=1, pick_i = pending_i & !pending_d = 0.
    // D sees busy=1 (about to be picked), I sees busy=0 in S_IDLE
    // (its pick_i=0).  But the moment we enter S_BUSY the I-port
    // observes (owner==1 || pending_i) → busy=1.  We assert the
    // post-pick view by ticking once.
    tick_with_mem(d, mem, 3, 0xAAAA'BBBBu);

    // Now in S_BUSY with owner=D=0.  Both should be busy.
    check_bool("simult.d_busy_in_busy", d->o_d_busy, true);
    check_bool("simult.i_busy_in_busy_with_pending", d->o_i_busy, true);

    // Run D to completion.
    int safety = 0;
    while (d->o_d_busy && safety++ < 20) {
        tick_with_mem(d, mem, 3, 0xAAAA'BBBBu);
    }
    check("simult.d_got_data", d->o_d_rdata, 0xAAAA'BBBBu);

    // Drop D request; I should still be pending.
    d->i_d_re = 0;
    check_bool("simult.i_still_busy_after_d_done", d->o_i_busy, true);

    // Now run I to completion, returning a different value.
    safety = 0;
    while (d->o_i_busy && safety++ < 20) {
        tick_with_mem(d, mem, 3, 0xCCCC'DDDDu);
    }
    check("simult.i_got_data", d->o_i_rdata, 0xCCCC'DDDDu);

    d->i_i_re = 0;
    tick_with_mem(d, mem, 0, 0);
}

static void test_d_write_passes_addr_and_data(Vcpu_bus_arbiter* d) {
    printf("── D write: addr/wdata/byte_en latched and delivered ──\n");
    reset(d);
    MemMock mem;

    d->i_d_addr    = 0x300;
    d->i_d_wdata   = 0xFEEDF00Du;
    d->i_d_byte_en = 0b1010;
    d->i_d_we      = 1;
    d->eval();

    // Tick into S_BUSY.
    tick_with_mem(d, mem, 1, 0);

    // The mock should have captured the write request.
    check_bool("d_write.captured", mem.have_request || mem.captured_we, true);
    check("d_write.captured_addr",  mem.captured_addr,  0x300u);
    check("d_write.captured_wdata", mem.captured_wdata, 0xFEEDF00Du);
    check_bool("d_write.was_write", mem.captured_we, true);

    // Drain.
    int safety = 0;
    while (d->o_d_busy && safety++ < 20) {
        tick_with_mem(d, mem, 1, 0);
    }
    d->i_d_we = 0;
    tick_with_mem(d, mem, 0, 0);
}

static void test_bus_quiet_outside_busy(Vcpu_bus_arbiter* d) {
    printf("── External bus is quiet outside S_BUSY ──\n");
    reset(d);

    // No requests anywhere — bus must stay idle.
    for (int i = 0; i < 10; i++) {
        tick(d);
        check_bool("bus_quiet.no_re", d->o_mem_re, false);
        check_bool("bus_quiet.no_we", d->o_mem_we, false);
    }
}

static void test_back_to_back_same_port(Vcpu_bus_arbiter* d) {
    printf("── Back-to-back D reads each get their own response ──\n");
    reset(d);
    MemMock mem;

    // First request.
    d->i_d_addr = 0x10;
    d->i_d_re   = 1;
    d->eval();

    int safety = 0;
    while (d->o_d_busy && safety++ < 20) {
        tick_with_mem(d, mem, 2, 0x1111'1111u);
    }
    check("b2b.first_rdata", d->o_d_rdata, 0x1111'1111u);

    // Two independent requests with i_re dropped in between — the
    // pattern uncached MMIO accesses use.  The pipelined burst path
    // (cache S_FILL holding i_re=1 across word boundaries) is
    // exercised by test_back_to_back_zero_dead_cycles.
    d->i_d_addr = 0x20;
    d->eval();
    d->i_d_re = 0;
    tick_with_mem(d, mem, 0, 0);
    d->i_d_addr = 0x20;
    d->i_d_re   = 1;
    d->eval();

    safety = 0;
    while (d->o_d_busy && safety++ < 20) {
        tick_with_mem(d, mem, 2, 0x2222'2222u);
    }
    check("b2b.second_rdata", d->o_d_rdata, 0x2222'2222u);

    d->i_d_re = 0;
    tick_with_mem(d, mem, 0, 0);
}

static void test_non_owner_exclusion(Vcpu_bus_arbiter* d) {
    printf("── Non-owner with pending request never latches owner's data ──\n");
    reset(d);
    MemMock mem;

    // Start D transaction; partway through, raise an I request.  The
    // I-side should observe busy=1 the entire time D's transaction
    // is in flight, and only see its own busy drop when it becomes
    // the owner after D completes.
    d->i_d_addr = 0x100;
    d->i_d_re   = 1;
    d->eval();
    tick_with_mem(d, mem, 4, 0xDDDD'DDDDu);
    // D is latched and req_re is now held by the arbiter's flop.
    // Drop the cache-side re so the arbiter doesn't back-to-back
    // re-latch D when its response arrives (this is the new
    // arbiter's default behaviour); we want a single D transaction
    // followed by a handoff to I.
    d->i_d_re = 0;
    d->eval();

    // Raise I request mid-transaction.
    d->i_i_addr = 0x200;
    d->i_i_re   = 1;
    d->eval();

    // I must observe busy=1 throughout D's transaction.  Walk every
    // cycle until D completes.
    int safety = 0;
    while (d->o_d_busy && safety++ < 20) {
        check_bool("non_owner.i_busy_during_d", d->o_i_busy, true);
        tick_with_mem(d, mem, 4, 0xDDDD'DDDDu);
    }

    // D done; rdata is D's response (combinational from i_mem_rdata
    // on the busy-drop cycle).
    check("non_owner.d_got_DDDD", d->o_d_rdata, 0xDDDD'DDDDu);

    // I has been picked at the same edge D's transaction ended (the
    // arbiter back-to-backs into I via the BUSY→BUSY handoff path).
    // Drain I; its data must be the *I*-port response.
    safety = 0;
    while (d->o_i_busy && safety++ < 20) {
        tick_with_mem(d, mem, 4, 0xEEEE'EEEEu);
    }
    check("non_owner.i_got_EEEE_not_DDDD", d->o_i_rdata, 0xEEEE'EEEEu);

    d->i_i_re = 0;
    tick_with_mem(d, mem, 0, 0);
}

static void test_req_accepted_pulse(Vcpu_bus_arbiter* d) {
    printf("── req_accepted: 1-cycle pulse on each owner latch ──\n");
    reset(d);
    MemMock mem;

    // (1) Idle: neither pulse fires.
    for (int i = 0; i < 3; i++) {
        check_bool("req_acc.idle_d_low", d->o_d_req_accepted, false);
        check_bool("req_acc.idle_i_low", d->o_i_req_accepted, false);
        tick_with_mem(d, mem, 2, 0);
    }

    // (2) D request: pulse fires combinationally in the same cycle.
    d->i_d_addr = 0x100;
    d->i_d_re   = 1;
    d->eval();
    check_bool("req_acc.d_pulse_on_request",  d->o_d_req_accepted, true);
    check_bool("req_acc.i_quiet_when_d_picks", d->o_i_req_accepted, false);

    // (3) After the latching edge the FSM is in S_BUSY — pulse drops.
    tick_with_mem(d, mem, 3, 0xCAFE0001u);
    check_bool("req_acc.d_pulse_drops_in_busy", d->o_d_req_accepted, false);

    // (4) Pulse stays low for the rest of the transaction.
    int safety = 0;
    while (d->o_d_busy && safety++ < 20) {
        check_bool("req_acc.d_pulse_low_during_txn", d->o_d_req_accepted, false);
        tick_with_mem(d, mem, 3, 0xCAFE0001u);
    }
    d->i_d_re = 0;
    tick_with_mem(d, mem, 0, 0);

    // (5) Simultaneous D+I: only D's pulse fires (D wins by priority).
    d->i_d_addr = 0x400;
    d->i_d_re   = 1;
    d->i_i_addr = 0x800;
    d->i_i_re   = 1;
    d->eval();
    check_bool("req_acc.simult_d_pulses",  d->o_d_req_accepted, true);
    check_bool("req_acc.simult_i_no_pulse", d->o_i_req_accepted, false);

    // Tick to enter S_BUSY for D; drop D's re so the arbiter
    // hands off to I on D's busy-drop rather than back-to-backing
    // into a second D transaction.
    tick_with_mem(d, mem, 2, 0xAAAA0001u);
    d->i_d_re = 0;
    d->eval();

    // Drain D's transaction.  The loop exits on the cycle D's
    // response arrives.
    int safety2 = 0;
    while (d->o_d_busy && safety2++ < 20) {
        tick_with_mem(d, mem, 2, 0xAAAA0001u);
    }

    // (6) Same cycle the loop exits: the arbiter sees !i_mem_busy
    //     with pending_i (and !pending_d after our re drop), so
    //     latch_event fires for I — I's pulse is high *this cycle*.
    //     This is the BUSY→BUSY handoff path (no IDLE in between).
    check_bool("req_acc.i_pulse_on_handoff",  d->o_i_req_accepted, true);
    check_bool("req_acc.d_quiet_when_i_picks", d->o_d_req_accepted, false);

    // Drain I.
    int safety3 = 0;
    while (d->o_i_busy && safety3++ < 20) {
        tick_with_mem(d, mem, 2, 0xBBBB0001u);
    }
    d->i_i_re = 0;
    tick_with_mem(d, mem, 0, 0);
}

static void test_back_to_back_zero_dead_cycles(Vcpu_bus_arbiter* d) {
    printf("── Back-to-back D burst: zero idle cycles between transactions ──\n");
    reset(d);
    MemMock mem;

    // Mimic the cache's S_FILL burst pattern: hold i_d_re=1, walk
    // i_d_addr on each req_accepted pulse.  Measure the total cycle
    // count and verify o_mem_re stays high across word boundaries.
    const int N_WORDS      = 4;
    const int MEM_LATENCY  = 2;
    const uint32_t BASE    = 0x2000;

    d->i_d_addr = BASE;
    d->i_d_re   = 1;
    d->eval();

    int req_count       = 0;
    int resp_count      = 0;
    int idle_re_cycles  = 0;
    bool burst_started  = false;
    int  cycle          = 0;

    while (resp_count < N_WORDS && cycle < 50) {
        // Cache-side: on every req_accepted, advance to the next addr.
        // re is dropped only AFTER the tick that latches the final
        // word, so the latch event for word N-1 isn't dropped by the
        // re-deassertion racing the edge.
        if (d->o_d_req_accepted) {
            req_count++;
            if (req_count < N_WORDS) {
                d->i_d_addr = BASE + (uint32_t)req_count * 4;
                d->eval();
            }
        }

        // Response capture: on busy drop with an outstanding request,
        // count it.
        if (!d->i_mem_busy && resp_count < req_count)
            resp_count++;

        // Track: once the burst has started (state has driven the bus
        // at least once) o_mem_re should remain high until the last
        // word's response arrives.
        if (d->o_mem_re)                          burst_started   = true;
        if (burst_started && resp_count < N_WORDS && !d->o_mem_re)
            idle_re_cycles++;

        tick_with_mem(d, mem, MEM_LATENCY, 0xCAFE0000u | (uint32_t)resp_count);
        cycle++;

        // Drop re after the tick that latched the final word.
        if (req_count == N_WORDS && d->i_d_re) {
            d->i_d_re = 0;
            d->eval();
        }
    }

    check("b2b.all_words_completed", (uint32_t)resp_count, (uint32_t)N_WORDS);
    check("b2b.no_idle_re_during_burst", (uint32_t)idle_re_cycles, 0u);
    // With back-to-back, each word costs MEM_LATENCY+1 cycles after
    // the initial pick.  Old arbiter (S_DONE + IDLE pick) added 2
    // dead cycles per word.  4 words at lat=2: new ≤ 12 cycles, old
    // would need ≥ 17.  10 leaves room for the test wrapping but
    // still rules out the old behaviour.
    check_bool("b2b.within_back_to_back_budget", cycle <= 12, true);
    printf("    [info] 4 back-to-back transactions completed in %d cycles\n", cycle);
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vcpu_bus_arbiter* d = new Vcpu_bus_arbiter;

    printf("── CPU Bus Arbiter Unit Tests ──\n\n");

    test_single_d_read(d);
    test_single_i_read(d);
    test_d_priority_on_simultaneous(d);
    test_d_write_passes_addr_and_data(d);
    test_bus_quiet_outside_busy(d);
    test_back_to_back_same_port(d);
    test_non_owner_exclusion(d);
    test_req_accepted_pulse(d);
    test_back_to_back_zero_dead_cycles(d);

    printf("\ncpu_bus_arbiter: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return errors ? 1 : 0;
}
