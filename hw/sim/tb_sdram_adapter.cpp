// Verilator testbench for sdram_bus_adapter.
//
// Exercises the bus-side handshake against a two-slot behavioral mock
// memory (sdram_adapter_test wrapper).  The mock returns deterministic
// address-encoded data (MOCK_TAG | (addr & ~3)) so a misrouted spec
// response surfaces as a wrong value, not a coincidence.
//
// Test priorities, highest first:
//   (a)  single read / single write smoke
//   (b)  sequential 4-word burst — spec_hit_buffered chain
//   (c)  non-sequential reads — spec mispredict + abandonment
//   (d)  read then write to the same address
//   (e)  bug reproducer: pop+push at tag_count==1 race
//   (f)  spec_hit_in_flight wait path
//   (g)  sustained tag_count==2 across many words
//   (h)  reset mid-burst recovery
//
// Each test prints its own [PASS]/[FAIL] line and increments a global
// failure count.  main() returns non-zero on any failure.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "Vsdram_adapter_test.h"

static int errors = 0;

static uint32_t expected_data(uint32_t addr) {
    return 0xAA000000u | (addr & 0xFFFFFFFCu);
}

static void tick(Vsdram_adapter_test* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vsdram_adapter_test* d) {
    d->i_rst = 1;
    d->i_addr = 0;
    d->i_wdata = 0;
    d->i_byte_en = 0xF;
    d->i_we = 0;
    d->i_re = 0;
    d->i_mock_latency = 4;
    d->i_mock_stall = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
    // One settling cycle so mock_cycle starts ticking before any
    // request lands.
    tick(d);
}

// Issue a single read.  Holds i_re/i_addr until o_busy clears, samples
// o_rdata on that cycle, then deasserts i_re for one cycle (so the
// FSM drops out of PRESENT to IDLE).  Returns 0xFFFFFFFF on timeout.
static uint32_t do_read(Vsdram_adapter_test* d, uint32_t addr,
                        const char* label, int timeout_cyc = 200) {
    d->i_addr = addr;
    d->i_re = 1;
    d->i_we = 0;
    d->eval();

    int waited = 0;
    while (d->o_busy) {
        tick(d);
        if (++waited > timeout_cyc) {
            printf("  [FAIL] %s addr=0x%08X — busy never cleared within %d cyc\n",
                   label, addr, timeout_cyc);
            errors++;
            d->i_re = 0;
            return 0xFFFFFFFFu;
        }
    }
    uint32_t rd = d->o_rdata;
    // Release i_re so FSM returns to IDLE between transactions.
    d->i_re = 0;
    tick(d);
    return rd;
}

// Issue a single write.  Same envelope as do_read but checks no data.
static bool do_write(Vsdram_adapter_test* d, uint32_t addr, uint32_t wdata,
                     uint8_t byte_en, const char* label, int timeout_cyc = 200) {
    d->i_addr = addr;
    d->i_wdata = wdata;
    d->i_byte_en = byte_en;
    d->i_we = 1;
    d->i_re = 0;
    d->eval();

    int waited = 0;
    while (d->o_busy) {
        tick(d);
        if (++waited > timeout_cyc) {
            printf("  [FAIL] %s addr=0x%08X — write busy never cleared within %d cyc\n",
                   label, addr, timeout_cyc);
            errors++;
            d->i_we = 0;
            return false;
        }
    }
    d->i_we = 0;
    tick(d);
    return true;
}

static void check_eq(uint32_t got, uint32_t want, const char* label) {
    if (got == want) {
        printf("  [PASS] %s — 0x%08X\n", label, got);
    } else {
        printf("  [FAIL] %s — got 0x%08X want 0x%08X\n", label, got, want);
        errors++;
    }
}

// Back-to-back burst — protocol per doc/hardware/bus-protocol.md
// § Burst Transfers: master keeps req (i_re) asserted, changes addr
// after each ack (o_busy↓), slave responds to each address
// independently.  In the adapter this exercises PRESENT→BEGIN, the
// fast path that lets the spec chain hide bus latency.
//
// Sampling timing is the trap: after observing o_busy=0 for word i,
// updating i_addr to the next word, and re-evaluating, o_busy still
// reads 0 (state is still PRESENT — the FSM hasn't seen the posedge
// yet).  Sampling at that moment grabs stale o_rdata for word i-1.
// The fix is to advance one clock between iterations so the FSM
// commits PRESENT→BEGIN with the freshly-set address, then wait for
// the new busy=0.
//
// Returns false on timeout.
static bool do_burst_read(Vsdram_adapter_test* d, const uint32_t* addrs,
                          uint32_t* out_data, int n, const char* label,
                          int timeout_cyc = 500) {
    int waited = 0;
    for (int i = 0; i < n; i++) {
        d->i_addr = addrs[i];
        d->i_re = 1;
        d->i_we = 0;
        d->eval();
        if (i > 0) {
            // PRESENT→BEGIN: address already updated, advance cycle.
            tick(d);
        }
        while (d->o_busy) {
            tick(d);
            if (++waited > timeout_cyc) {
                printf("  [FAIL] %s — burst stalled at word %d (addr=0x%08X)\n",
                       label, i, addrs[i]);
                errors++;
                d->i_re = 0;
                return false;
            }
        }
        out_data[i] = d->o_rdata;
    }
    d->i_re = 0;
    tick(d);
    return true;
}

// Gapped burst — mirrors how cpu_bus_arbiter currently drives the
// adapter: each transaction deasserts i_re for one cycle after the
// ack before re-asserting for the next, forcing PRESENT→IDLE→BEGIN
// instead of the PRESENT→BEGIN fast path.  Same data-correctness
// contract, different traversal of the FSM.  When the arbiter is
// eventually optimized to support true back-to-back, this path stops
// being exercised in production — until then it's our regression
// sentinel for the path the current system actually takes.
static bool do_burst_read_gapped(Vsdram_adapter_test* d, const uint32_t* addrs,
                                 uint32_t* out_data, int n, const char* label,
                                 int timeout_cyc = 500) {
    int waited = 0;
    for (int i = 0; i < n; i++) {
        d->i_addr = addrs[i];
        d->i_re = 1;
        d->i_we = 0;
        d->eval();
        while (d->o_busy) {
            tick(d);
            if (++waited > timeout_cyc) {
                printf("  [FAIL] %s — gapped burst stalled at word %d (addr=0x%08X)\n",
                       label, i, addrs[i]);
                errors++;
                d->i_re = 0;
                return false;
            }
        }
        out_data[i] = d->o_rdata;
        // Arbiter-style: release i_re for one cycle before next request.
        d->i_re = 0;
        tick(d);
    }
    return true;
}

// ────────────────────────────────────────────────────────────
// Test (a) — single read / write smoke
// ────────────────────────────────────────────────────────────
static void test_a_smoke(Vsdram_adapter_test* d) {
    printf("── (a) smoke ──\n");
    reset(d);
    d->i_mock_latency = 3;

    uint32_t r = do_read(d, 0x00001000, "RD 0x1000");
    check_eq(r, expected_data(0x00001000), "(a) read 0x1000");

    bool wok = do_write(d, 0x00002000, 0xDEADBEEF, 0xF, "WR 0x2000");
    if (wok) printf("  [PASS] (a) write 0x2000 accepted\n");
}

// ────────────────────────────────────────────────────────────
// Test (b) — sequential 4-word burst exercises spec_hit_buffered.
//
// Run twice: once with i_re held high across the burst (the bus-
// protocol contract: PRESENT→BEGIN fast path), and once with a
// 1-cycle gap between words (the path cpu_bus_arbiter currently
// drives: PRESENT→IDLE→BEGIN).  Both must return correct data.
// ────────────────────────────────────────────────────────────
static void test_b_sequential_burst(Vsdram_adapter_test* d) {
    printf("── (b) sequential burst (back-to-back) ──\n");
    reset(d);
    // Latency long enough that spec_buffered fills before the
    // next read arrives.  With lat=4, the chain has comfortable
    // slack: first miss costs 4, subsequent words hit buffered.
    d->i_mock_latency = 4;

    const uint32_t addrs[4] = {
        0x00010000, 0x00010004, 0x00010008, 0x0001000C
    };
    uint32_t got[4] = {0,0,0,0};
    if (do_burst_read(d, addrs, got, 4, "(b) burst")) {
        for (int i = 0; i < 4; i++) {
            char lab[40];
            snprintf(lab, sizeof(lab), "(b) burst[%d]", i);
            check_eq(got[i], expected_data(addrs[i]), lab);
        }
    }

    printf("── (b) sequential burst (gapped, arbiter-style) ──\n");
    reset(d);
    d->i_mock_latency = 4;
    const uint32_t addrs2[4] = {
        0x00011000, 0x00011004, 0x00011008, 0x0001100C
    };
    uint32_t got2[4] = {0,0,0,0};
    if (do_burst_read_gapped(d, addrs2, got2, 4, "(b) gapped")) {
        for (int i = 0; i < 4; i++) {
            char lab[40];
            snprintf(lab, sizeof(lab), "(b) gapped[%d]", i);
            check_eq(got2[i], expected_data(addrs2[i]), lab);
        }
    }
}

// ────────────────────────────────────────────────────────────
// Test (c) — non-sequential reads: spec mispredict + abandon
// ────────────────────────────────────────────────────────────
static void test_c_misprediction(Vsdram_adapter_test* d) {
    printf("── (c) mispredict + abandon ──\n");
    reset(d);
    d->i_mock_latency = 4;

    // First read primes spec for A+4.
    uint32_t r1 = do_read(d, 0x00020000, "RD 0x20000");
    check_eq(r1, expected_data(0x00020000), "(c) primary");

    // Second read jumps to a different region — spec for 0x20004
    // becomes a mispredict and must be abandoned.  Its response
    // arrives later and should not corrupt the new read.
    uint32_t r2 = do_read(d, 0x00030000, "RD 0x30000 (mispredict)");
    check_eq(r2, expected_data(0x00030000), "(c) after mispredict");

    // Third read at 0x30004 — new spec chain should be rolling.
    uint32_t r3 = do_read(d, 0x00030004, "RD 0x30004 (new spec)");
    check_eq(r3, expected_data(0x00030004), "(c) new spec chain");
}

// ────────────────────────────────────────────────────────────
// Test (d) — read then write to same address; write must not
// corrupt any buffered spec state lingering from the read.
// ────────────────────────────────────────────────────────────
static void test_d_read_then_write(Vsdram_adapter_test* d) {
    printf("── (d) read then write same address ──\n");
    reset(d);
    d->i_mock_latency = 4;

    uint32_t r1 = do_read(d, 0x00040000, "RD 0x40000");
    check_eq(r1, expected_data(0x00040000), "(d) read primary");

    // Spec for 0x40004 is now in flight or buffered.  A write to
    // the same primary address must travel via push_real_event and
    // abandon the spec (or read it before being clobbered).
    if (!do_write(d, 0x00040000, 0xCAFEBABE, 0xF, "WR 0x40000")) return;
    printf("  [PASS] (d) write committed\n");

    // Follow-up read at 0x40008 — must work even with the abandoned
    // spec response still wandering in the mock pipeline.
    uint32_t r2 = do_read(d, 0x00040008, "RD 0x40008 (post-write)");
    check_eq(r2, expected_data(0x00040008), "(d) read after write");
}

// ────────────────────────────────────────────────────────────
// Test (e) — bug reproducer: tag_count 1 → 1 across pop+push.
//
// With mock latency = 1 cycle:
//   • Cycle k:   cache real-push of A accepted; tag_count 0→1.
//   • Cycle k+1: spec-push of A+4 driven combinationally AND the
//                real response for A returns (latency 1).
//                pop_event=1, push_event=1, tag_count was 1.
//
// Pre-fix: the pop+push branch falls through to the depth-2 shift,
// landing a 0 sentinel in tag_fifo[0] and stranding the spec tag
// at tag_fifo[1].  At the next response, the head tag is 0 →
// the spec response is routed to rdata_latched, corrupting the
// next cache read.  Also, the spec_in_flight bit never clears,
// so the next BEGIN spins on spec_hit_in_flight and the cache
// deadlocks → this test detects the bug via the follow-up read's
// timeout.
//
// SVA #2 (tag_fifo[1]==0 when tag_count<2) also fires.
// ────────────────────────────────────────────────────────────
static void test_e_bug_repro(Vsdram_adapter_test* d) {
    printf("── (e) bug repro: pop+push at tag_count==1 ──\n");
    reset(d);
    d->i_mock_latency = 1;

    // First read: triggers the race in cycle k+1.
    uint32_t r1 = do_read(d, 0x00050000, "RD 0x50000 (race trigger)");
    check_eq(r1, expected_data(0x00050000), "(e) primary read");

    // Follow-up read at the spec'd address.  With the fix, this
    // hits spec_buffered or spec_hit_in_flight and returns the
    // correct data.  Without the fix, spec_in_flight stays asserted
    // forever (the spec response was misrouted to rdata_latched
    // instead of the buffer), so BEGIN spins on spec_hit_in_flight
    // and o_busy never clears — do_read times out and FAILs here.
    uint32_t r2 = do_read(d, 0x00050004, "RD 0x50004 (spec target)");
    check_eq(r2, expected_data(0x00050004), "(e) spec target");

    // Third read: prove the FIFO is still coherent after the race.
    uint32_t r3 = do_read(d, 0x00050100, "RD 0x50100 (post-race)");
    check_eq(r3, expected_data(0x00050100), "(e) post-race coherence");
}

// ────────────────────────────────────────────────────────────
// Test (f) — spec_hit_in_flight: cache asks for the spec'd
// address before its response has come back.  BEGIN stalls until
// the response arrives and is routed to the buffer; next cycle
// BEGIN takes the spec_hit_buffered branch.
//
// Tune latency to be long enough that the second read arrives
// well before the first response has returned.
// ────────────────────────────────────────────────────────────
static void test_f_in_flight_wait(Vsdram_adapter_test* d) {
    printf("── (f) spec_hit_in_flight ──\n");
    reset(d);
    d->i_mock_latency = 12;

    uint32_t addrs[2] = { 0x00060000, 0x00060004 };
    uint32_t got[2] = {0,0};
    if (!do_burst_read(d, addrs, got, 2, "(f) in-flight wait")) return;

    check_eq(got[0], expected_data(addrs[0]), "(f) primary (cold)");
    check_eq(got[1], expected_data(addrs[1]), "(f) spec (in-flight wait)");
}

// ────────────────────────────────────────────────────────────
// Test (g) — sustained tag_count==2.  Holds 16 sequential reads
// through the chain; with enough latency, both a real and a spec
// are in flight together for most of the run.  Validates the
// depth-2 invariant under continuous use.
// ────────────────────────────────────────────────────────────
static void test_g_sustained_depth2(Vsdram_adapter_test* d) {
    printf("── (g) sustained tag_count==2 ──\n");
    reset(d);
    d->i_mock_latency = 6;

    constexpr int N = 16;
    uint32_t addrs[N];
    uint32_t got[N];
    for (int i = 0; i < N; i++) addrs[i] = 0x00070000u + (i * 4u);
    if (!do_burst_read(d, addrs, got, N, "(g) sustained")) return;

    int word_errors = 0;
    for (int i = 0; i < N; i++) {
        if (got[i] != expected_data(addrs[i])) {
            printf("  [FAIL] (g) word %d — got 0x%08X want 0x%08X\n",
                   i, got[i], expected_data(addrs[i]));
            errors++; word_errors++;
        }
    }
    if (word_errors == 0)
        printf("  [PASS] (g) all %d words correct\n", N);
}

// ────────────────────────────────────────────────────────────
// Test (h) — reset mid-burst.  Issue a read with high latency,
// pulse reset before it completes, then verify the next
// transaction works correctly from a clean state.
// ────────────────────────────────────────────────────────────
static void test_h_reset_mid_burst(Vsdram_adapter_test* d) {
    printf("── (h) reset mid-burst ──\n");
    reset(d);
    d->i_mock_latency = 16;

    // Start a read but interrupt with reset before it completes.
    d->i_addr = 0x00080000;
    d->i_re = 1;
    d->eval();
    // Let the request enter the mock and spec push happen, then
    // assert reset while transactions are still in flight.
    for (int i = 0; i < 4; i++) tick(d);
    d->i_rst = 1;
    d->i_re = 0;
    for (int i = 0; i < 3; i++) tick(d);
    d->i_rst = 0;
    tick(d);

    // After reset, mock_cycle keeps counting (its counter only
    // clears on reset assertion), but the adapter is back to a
    // clean state.  A fresh read must work end-to-end.
    d->i_mock_latency = 4;
    uint32_t r1 = do_read(d, 0x00080100, "RD 0x80100 (post-reset)");
    check_eq(r1, expected_data(0x00080100), "(h) read after reset");

    // And the chain rolls again.
    uint32_t r2 = do_read(d, 0x00080104, "RD 0x80104");
    check_eq(r2, expected_data(0x00080104), "(h) chain continues");
}

// ────────────────────────────────────────────────────────────
// Test (i) — long sustained sequential read.  Pulls 128 words
// from a single page, which is much more spec-chain churn than
// the 4-word bursts of tests (b)/(g).  If the spec chain ever
// loses synchronization with the address stream (e.g. spec_addr
// drifts ahead of i_addr by some unintended amount), the data
// correctness check at every word catches it.  Matches the source
// side of a kernel page-copy memcpy.
// ────────────────────────────────────────────────────────────
static void test_i_long_sequential_read(Vsdram_adapter_test* d) {
    printf("── (i) long sustained sequential read (128 words) ──\n");
    reset(d);
    d->i_mock_latency = 4;

    const int N = 128;
    uint32_t addrs[N];
    uint32_t got[N];
    for (int i = 0; i < N; i++) addrs[i] = 0x00100000u + 4u * (uint32_t)i;

    // do_burst_read_gapped's default 500-cycle budget is for short
    // bursts; 128 gapped words at latency=4 needs ≈ 2000 cycles.
    if (!do_burst_read_gapped(d, addrs, got, N, "(i) long", 4000)) return;
    for (int i = 0; i < N; i++) {
        if (got[i] != expected_data(addrs[i])) {
            char lab[40];
            snprintf(lab, sizeof(lab), "(i) word[%d]", i);
            check_eq(got[i], expected_data(addrs[i]), lab);
            return;
        }
    }
    printf("  [PASS] (i) 128 sequential words all correct\n");
}

// ────────────────────────────────────────────────────────────
// Test (j) — long sustained sequential write.  Mirrors kernel
// pmap_zero_page: 128 writes to consecutive addresses, no reads,
// no data verification (the mock doesn't model writes), but any
// timeout, SVA fire, or tag FIFO desync would surface here.
// ────────────────────────────────────────────────────────────
static void test_j_long_sequential_write(Vsdram_adapter_test* d) {
    printf("── (j) long sustained sequential write (128 words) ──\n");
    reset(d);
    d->i_mock_latency = 4;

    for (int i = 0; i < 128; i++) {
        uint32_t addr = 0x00200000u + 4u * (uint32_t)i;
        if (!do_write(d, addr, 0xDEADBEEFu, 0xF, "(j) STW")) return;
    }
    printf("  [PASS] (j) 128 sequential writes completed\n");
}

// ────────────────────────────────────────────────────────────
// Test (k) — interleaved memcpy pattern.  Alternates LDW from
// page A with STW to page B for many word pairs — the bus traffic
// shape kernel pmap_copy_page emits during COW.  Each LDW pushes
// a spec for A+4, each STW abandons any in-flight spec.  The
// spec_in_flight / spec_buffered / spec_abandoned state churns
// rapidly; if the abandonment-and-reissue interaction has a
// residual race (the kind the original bug-repro test (e) caught
// for a different code path), the LDW data correctness check
// catches it here.
// ────────────────────────────────────────────────────────────
static void test_k_memcpy_interleave(Vsdram_adapter_test* d) {
    printf("── (k) memcpy LDW/STW interleave (64 pairs) ──\n");
    reset(d);
    d->i_mock_latency = 4;

    const int N = 64;
    const uint32_t SRC = 0x00300000u;
    const uint32_t DST = 0x00400000u;

    for (int i = 0; i < N; i++) {
        uint32_t src = SRC + 4u * (uint32_t)i;
        uint32_t dst = DST + 4u * (uint32_t)i;
        uint32_t got = do_read(d, src, "(k) LDW src");
        if (got != expected_data(src)) {
            char lab[40];
            snprintf(lab, sizeof(lab), "(k) src[%d]", i);
            check_eq(got, expected_data(src), lab);
            return;
        }
        if (!do_write(d, dst, got, 0xF, "(k) STW dst")) return;
    }
    printf("  [PASS] (k) %d LDW/STW pairs interleaved cleanly\n", N);
}

// ────────────────────────────────────────────────────────────
// Test (l) — varying latency under sustained sequential reads.
// The bug class we keep chasing fires at specific timing
// alignments; sweeping the mock latency across a sustained read
// burst exercises the spec chain under every per-request response-
// arrival phase that a real-system fill would see.  If any
// latency value desyncs the FIFO or spec state, the per-word check
// catches it.
// ────────────────────────────────────────────────────────────
static void test_l_latency_sweep(Vsdram_adapter_test* d) {
    printf("── (l) sustained reads under latency sweep ──\n");
    for (uint8_t lat = 1; lat <= 8; lat++) {
        reset(d);
        d->i_mock_latency = lat;

        const int N = 16;
        uint32_t addrs[N];
        uint32_t got[N];
        for (int i = 0; i < N; i++) addrs[i] = 0x00500000u + 0x1000u * lat + 4u * (uint32_t)i;

        bool ok = do_burst_read_gapped(d, addrs, got, N, "(l) sweep");
        if (!ok) { printf("  [FAIL] (l) timeout at latency=%u\n", lat); continue; }

        bool clean = true;
        for (int i = 0; i < N; i++) {
            if (got[i] != expected_data(addrs[i])) {
                char lab[48];
                snprintf(lab, sizeof(lab), "(l) lat=%u word[%d]", lat, i);
                check_eq(got[i], expected_data(addrs[i]), lab);
                clean = false;
                break;
            }
        }
        if (clean) printf("  [PASS] (l) latency=%u: %d words clean\n", lat, N);
    }
}

// ────────────────────────────────────────────────────────────
// Test (m) — spec response coincident with push_real_event.
//
// Under the synchronous mock's deterministic timing, the spec
// response after an LDW normally lands while the FSM is in PRESENT
// (the FSM transitions WAIT_RSP→PRESENT on the real response, and
// spec response lands one cycle later — never in BEGIN, so
// push_real_event can't coincide with it in a single LDW).  Real
// hardware doesn't have this clean serialization: the dual-clock
// CDC's phase-dependent latency makes the response land in any
// state the FSM happens to be in.  To exercise the coincident case
// here, we exploit a different path: an LDW chain leaves a fresh
// spec for A+8 in flight at the moment `do_read(A+4)` returns —
// it was pushed on do_read's final tick.  With N=2 that spec's
// response is exactly one cycle from arriving, so we can time a
// fresh request from IDLE to land state=BEGIN on the response
// cycle and trigger the race deterministically.
//
// Cycle alignment (relative to mock_cycle right after do_read(A+4)
// returns; call it K_push):
//   • slot_q[0]={A+8, ready_at = K_push+N-1}.
//   • Cycle K_push: state=IDLE, mock_cycle=K_push, complete_now=0.
//     We assert i_re=1, addr=B (B != A+8) and eval.
//   • Tick #1 → edge K_push+1: FSM IDLE→BEGIN.  Post-edge:
//     state=BEGIN, mock_cycle=K_push+1=ready_at, complete_now=1
//     combinationally.  push_real_event=1 AND spec_arriving_now=1.
//     This is the race cycle.
//   • Tick #2 → edge K_push+2: NBAs commit.  Post-fix:
//     spec_buffered=0 (FSM's unconditional clear wins NBA last-
//     write-wins).  Pre-fix: spec_buffered=1 with stale data.
//
// The SVA at the bottom of sdram_bus_adapter.sv catches the bad
// state directly; this test additionally proves the testbench
// reaches the path and that follow-up LDW for B returns fresh data
// from the mock (not the stale spec data that would have been
// buffered).
// ────────────────────────────────────────────────────────────
static void test_m_resp_arrival_coincident_push(Vsdram_adapter_test* d) {
    printf("── (m) spec response coincident with push_real_event ──\n");
    reset(d);

    // N=2 is the only latency where the alignment fits without
    // padding ticks (response arrives exactly one cycle after
    // do_read(A+4) returns).  Other latencies would need N-2 pad
    // ticks between do_read and the i_re assertion.
    const uint8_t N = 2;
    d->i_mock_latency = N;
    d->i_mock_stall = 0;

    const uint32_t A = 0x00900000u;
    const uint32_t B = 0x00910000u;  // != A+8 (would alias the spec)

    // Prime: LDW A populates spec buffer for A+4.
    uint32_t r1 = do_read(d, A, "(m) LDW A");
    check_eq(r1, expected_data(A), "(m) LDW A primary");

    // Consume the buffer with LDW A+4.  This takes the
    // spec_hit_buffered branch, which queues a new spec for A+8.
    // do_read's final tick (which drops i_re, transitioning
    // PRESENT→IDLE) is also the cycle the queued spec push fires
    // — so when this returns we have:
    //   state=IDLE, spec_in_flight=1, spec_addr=A+8,
    //   slot_q[0].ready_at = K_push + N - 1 = K_push + 1 (for N=2)
    // i.e. the spec response is exactly one tick away.
    uint32_t r2 = do_read(d, A + 4, "(m) LDW A+4 (consume buffer)");
    check_eq(r2, expected_data(A + 4), "(m) LDW A+4 hit buffered");

    // Sanity: spec must be in flight (not buffered, not idle) for
    // the alignment to work.  If it's not in flight, do_read's
    // timing changed or N is not 2.
    if (!d->o_dbg_spec_in_flight) {
        printf("  [FAIL] (m) spec not in flight after do_read(A+4) — "
               "test prerequisite not met\n");
        errors++;
        return;
    }

    // Assert i_re=1, addr=B before the next edge.  This is the
    // alignment trigger: at edge K_push+1 the FSM commits
    // IDLE→BEGIN AND mock_cycle advances to ready_at.  The cycle
    // after that edge sees state=BEGIN with complete_now=1, which
    // is the race cycle.
    d->i_addr = B;
    d->i_re = 1;
    d->i_we = 0;
    d->eval();

    tick(d);  // edge K_push+1: state IDLE→BEGIN, response arrives
    // Between ticks: this is the race cycle.  push_real_event and
    // spec_arriving_now must both be high combinationally.  If
    // either isn't, the alignment is wrong and the test below would
    // pass for unrelated reasons.
    bool got_coincidence =
        d->o_dbg_push_real_event && d->o_dbg_spec_arriving_now;
    if (!got_coincidence) {
        printf("  [FAIL] (m) race cycle alignment missed — "
               "push_real_event=%u spec_arriving_now=%u\n",
               (unsigned)d->o_dbg_push_real_event,
               (unsigned)d->o_dbg_spec_arriving_now);
        errors++;
        d->i_re = 0;
        return;
    }
    printf("  [PASS] (m) race cycle reached "
           "(push_real_event && spec_arriving_now both high)\n");

    tick(d);  // edge K_push+2: race NBAs commit

    // The SVA inside sdram_bus_adapter.sv fires if spec_buffered=1
    // here (post-fix should be 0).  Verilator runs SVAs as $error
    // and increments its assert count; this is the same path that
    // the regression sentinel uses.  We also check the debug port
    // directly so this test prints a recognizable PASS/FAIL line.
    if (d->o_dbg_spec_buffered) {
        printf("  [FAIL] (m) spec_buffered=1 after coincident cycle — "
               "stale spec (addr=0x%08X)\n", d->o_dbg_spec_addr);
        errors++;
    } else {
        printf("  [PASS] (m) spec_buffered cleared at coincident cycle\n");
    }

    // Drain the LDW B already in progress and verify fresh data.
    // If the bug were present and B happened to alias spec_addr,
    // this would return stale spec data instead of MOCK_TAG|B.
    // With B != A+8 the data check is a sanity check on the
    // post-race FSM, not the race itself.
    int waited = 0;
    while (d->o_busy) {
        tick(d);
        if (++waited > 200) {
            printf("  [FAIL] (m) busy never cleared after race cycle\n");
            errors++;
            d->i_re = 0;
            return;
        }
    }
    uint32_t got_b = d->o_rdata;
    d->i_re = 0;
    tick(d);
    check_eq(got_b, expected_data(B), "(m) LDW B post-coincident-cycle");
}

// ────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Vsdram_adapter_test* d = new Vsdram_adapter_test;

    printf("══ sdram_bus_adapter unit test ══\n");

    test_a_smoke(d);
    test_b_sequential_burst(d);
    test_c_misprediction(d);
    test_d_read_then_write(d);
    test_e_bug_repro(d);
    test_f_in_flight_wait(d);
    test_g_sustained_depth2(d);
    test_h_reset_mid_burst(d);
    test_i_long_sequential_read(d);
    test_j_long_sequential_write(d);
    test_k_memcpy_interleave(d);
    test_l_latency_sweep(d);
    test_m_resp_arrival_coincident_push(d);

    delete d;

    if (errors == 0) {
        printf("══ ALL PASS ══\n");
        return 0;
    } else {
        printf("══ %d FAILURE(S) ══\n", errors);
        return 1;
    }
}
