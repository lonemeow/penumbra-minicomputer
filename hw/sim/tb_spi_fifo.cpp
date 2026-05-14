// Verilator testbench for the Penumbra SPI FIFO
//
// Tests the parameterized synchronous FIFO used by the SPI master's
// TX and RX paths (spi_fifo.sv).  The DUT is instantiated with the
// default DEPTH=512.
//
// Documented contract (from spi_fifo.sv header):
//   - Circular buffer, power-of-2 depth, 8-bit data.
//   - Synchronous write port (i_wr/i_wdata), pop port (i_rd).
//   - o_rdata is valid same cycle as i_rd (combinational read).
//   - Full/empty disambiguated with an extra pointer MSB.
//   - o_level = entries currently stored (0..DEPTH).
//   - i_flush synchronously clears (same effect as i_rst on pointers).
//   - Simultaneous read+write while full is allowed and preserves
//     level (write_valid = i_wr && (!o_full || i_rd)).
//   - FIFO ordering (oldest first out).

#include <cstdio>
#include <cstdint>
#include "Vspi_fifo.h"

// DEPTH parameter (matches spi_fifo.sv default).
static constexpr int DEPTH = 512;

static int errors = 0, tests = 0;

static void tick(Vspi_fifo* d) {
    d->i_clk = 0; d->eval();
    d->i_clk = 1; d->eval();
}

static void reset(Vspi_fifo* d) {
    d->i_rst = 1;
    d->i_wr = 0;
    d->i_wdata = 0;
    d->i_rd = 0;
    d->i_flush = 0;
    tick(d);
    tick(d);
    d->i_rst = 0;
}

// Push one byte.  Holds i_wr high during the rising edge so write_valid
// is sampled correctly, then drops it.
static void push(Vspi_fifo* d, uint8_t byte) {
    d->i_wdata = byte;
    d->i_wr = 1;
    d->i_rd = 0;
    tick(d);
    d->i_wr = 0;
}

// Pop one byte.  o_rdata is combinational, valid the same cycle i_rd
// is asserted.  Sample before the tick.
static uint8_t pop(Vspi_fifo* d) {
    d->i_rd = 1;
    d->i_wr = 0;
    d->eval();
    uint8_t v = d->o_rdata;
    tick(d);
    d->i_rd = 0;
    return v;
}

// Simultaneous push and pop.  Tests the back-pressure case where the
// producer keeps writing while the consumer drains one slot per cycle.
static uint8_t push_pop(Vspi_fifo* d, uint8_t byte) {
    d->i_wdata = byte;
    d->i_wr = 1;
    d->i_rd = 1;
    d->eval();
    uint8_t v = d->o_rdata;
    tick(d);
    d->i_wr = 0;
    d->i_rd = 0;
    return v;
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

// ══════════════════════════════════════════════════════════════
// Tests
// ══════════════════════════════════════════════════════════════

static void test_reset_state(Vspi_fifo* d) {
    printf("── Reset: empty, level=0, not full ──\n");
    reset(d);
    check_bool("reset.empty", d->o_empty, true);
    check_bool("reset.not_full", d->o_full, false);
    check("reset.level_zero", d->o_level, 0u);
}

static void test_push_one(Vspi_fifo* d) {
    printf("── Push one byte: not empty, level=1 ──\n");
    reset(d);

    push(d, 0x42);
    check_bool("push.not_empty", d->o_empty, false);
    check_bool("push.not_full", d->o_full, false);
    check("push.level_one", d->o_level, 1u);
}

static void test_push_pop_round_trip(Vspi_fifo* d) {
    printf("── Push then pop: data preserved, back to empty ──\n");
    reset(d);

    push(d, 0xA5);
    uint8_t got = pop(d);
    check("push_pop.data", got, 0xA5u);
    check_bool("push_pop.empty_again", d->o_empty, true);
    check("push_pop.level_zero", d->o_level, 0u);
}

static void test_fifo_ordering(Vspi_fifo* d) {
    printf("── FIFO ordering: oldest pops first ──\n");
    reset(d);

    for (uint8_t i = 1; i <= 8; i++)
        push(d, i);
    check("ordering.level", d->o_level, 8u);

    for (uint8_t i = 1; i <= 8; i++) {
        uint8_t got = pop(d);
        char name[32];
        snprintf(name, sizeof(name), "ordering.byte_%u", (unsigned)i);
        check(name, got, i);
    }
    check_bool("ordering.empty_after", d->o_empty, true);
}

static void test_fill_to_full(Vspi_fifo* d) {
    printf("── Fill to DEPTH: o_full asserts, level=DEPTH ──\n");
    reset(d);

    for (int i = 0; i < DEPTH; i++)
        push(d, (uint8_t)(i & 0xFF));

    check_bool("fill.full", d->o_full, true);
    check_bool("fill.not_empty", d->o_empty, false);
    check("fill.level_eq_depth", d->o_level, (uint32_t)DEPTH);
}

static void test_push_while_full_is_dropped(Vspi_fifo* d) {
    printf("── Push while full (no concurrent read) is dropped ──\n");
    reset(d);

    // Fill with a known pattern.
    for (int i = 0; i < DEPTH; i++)
        push(d, (uint8_t)i);

    // Attempt to push more — these must NOT enter the FIFO.
    // (write_valid = i_wr && (!o_full || i_rd); without i_rd, full
    // blocks the write.)
    for (int i = 0; i < 4; i++)
        push(d, 0xFF);

    check_bool("dropped.still_full", d->o_full, true);
    check("dropped.level_unchanged", d->o_level, (uint32_t)DEPTH);

    // First byte popped should still be the original byte 0, not 0xFF.
    uint8_t got = pop(d);
    check("dropped.head_unchanged", got, 0u);
}

static void test_simultaneous_rd_wr_when_full(Vspi_fifo* d) {
    printf("── Simultaneous read+write when full: level unchanged, data flows ──\n");
    reset(d);

    // Fill with [0, 1, 2, ..., DEPTH-1].
    for (int i = 0; i < DEPTH; i++)
        push(d, (uint8_t)i);
    check_bool("simul.preload_full", d->o_full, true);

    // Concurrent push+pop while full.  The contract says write_valid
    // is allowed (i_rd makes room in the same cycle).  Push 0xAA;
    // expect the head (byte 0) to come out, and the new byte to land
    // at the tail.
    uint8_t got = push_pop(d, 0xAAu);
    check("simul.head_pops_first", got, 0u);
    check("simul.level_unchanged", d->o_level, (uint32_t)DEPTH);
    check_bool("simul.still_full", d->o_full, true);

    // Drain everything; the last byte out should be 0xAA (the one we
    // just pushed during the simultaneous rd+wr cycle).
    uint8_t last = 0;
    for (int i = 0; i < DEPTH; i++)
        last = pop(d);
    check("simul.tail_byte_was_pushed", last, 0xAAu);
    check_bool("simul.empty_after", d->o_empty, true);
}

static void test_simultaneous_rd_wr_when_partial(Vspi_fifo* d) {
    printf("── Simultaneous read+write when partially filled: level unchanged ──\n");
    reset(d);

    for (uint8_t i = 1; i <= 4; i++)
        push(d, i);
    check("partial.level_4", d->o_level, 4u);

    uint8_t got = push_pop(d, 0xBBu);
    check("partial.popped_head", got, 1u);
    check("partial.level_unchanged", d->o_level, 4u);

    // Drain and verify ordering: [2, 3, 4, 0xBB].
    check("partial.drain_2",  pop(d), 2u);
    check("partial.drain_3",  pop(d), 3u);
    check("partial.drain_4",  pop(d), 4u);
    check("partial.drain_bb", pop(d), 0xBBu);
}

static void test_pop_from_empty_is_noop(Vspi_fifo* d) {
    printf("── Pop while empty does nothing ──\n");
    reset(d);

    // read_valid = i_rd && !o_empty; on empty, pop should not advance
    // rd_ptr.  o_rdata returns mem[0] which is undefined-but-stable;
    // we don't assert on data, only that level stays at 0.
    d->i_rd = 1;
    d->eval();
    tick(d);
    d->i_rd = 0;

    check("empty_pop.level_zero", d->o_level, 0u);
    check_bool("empty_pop.still_empty", d->o_empty, true);
}

static void test_flush_clears_state(Vspi_fifo* d) {
    printf("── i_flush synchronously empties the FIFO ──\n");
    reset(d);

    for (uint8_t i = 0; i < 16; i++)
        push(d, i);
    check("flush.level_16_before", d->o_level, 16u);

    // Assert flush for one cycle.
    d->i_flush = 1;
    d->i_wr = 0;
    d->i_rd = 0;
    tick(d);
    d->i_flush = 0;

    check_bool("flush.empty_after", d->o_empty, true);
    check_bool("flush.not_full_after", d->o_full, false);
    check("flush.level_zero", d->o_level, 0u);

    // Pushing after flush should land at index 0 again, in order.
    push(d, 0xC1);
    push(d, 0xC2);
    check("flush.post_byte_0", pop(d), 0xC1u);
    check("flush.post_byte_1", pop(d), 0xC2u);
}

static void test_wraparound(Vspi_fifo* d) {
    printf("── Pointer wraparound: cycle past DEPTH preserves ordering ──\n");
    reset(d);

    // Cycle DEPTH+10 bytes through, popping each one in turn so the
    // pointers wrap around but the FIFO stays near-empty.  This
    // exercises the extra-MSB full/empty disambiguation.
    for (int i = 0; i < DEPTH + 10; i++) {
        uint8_t b = (uint8_t)(i & 0xFF);
        push(d, b);
        uint8_t got = pop(d);
        if (got != b) {
            char name[40];
            snprintf(name, sizeof(name), "wrap.byte_%d", i);
            check(name, got, b);
            return;
        }
    }
    tests++;  // single aggregate check for the loop
    check_bool("wrap.all_round_trips_match", true, true);
    check_bool("wrap.empty_at_end", d->o_empty, true);
}

// ══════════════════════════════════════════════════════════════

int main() {
    Vspi_fifo* d = new Vspi_fifo;

    printf("── SPI FIFO Unit Tests ──\n\n");

    test_reset_state(d);
    test_push_one(d);
    test_push_pop_round_trip(d);
    test_fifo_ordering(d);
    test_fill_to_full(d);
    test_push_while_full_is_dropped(d);
    test_simultaneous_rd_wr_when_full(d);
    test_simultaneous_rd_wr_when_partial(d);
    test_pop_from_empty_is_noop(d);
    test_flush_clears_state(d);
    test_wraparound(d);

    printf("\nspi_fifo: %d/%d tests passed\n", tests - errors, tests);
    if (errors > 0)
        printf("  *** %d FAILED ***\n", errors);

    delete d;
    return errors ? 1 : 0;
}
