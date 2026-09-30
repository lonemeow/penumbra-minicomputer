// Verilator testbench for penumbra3_return_stack (DEPTH 8).
//
// Pops return pushes in LIFO order; pushing past the depth overwrites the
// oldest slot and the innermost returns stay right; the ring never refuses,
// so popping past the pushes keeps predicting from whatever the slots hold;
// a push after pops reuses the slot the pops stepped back over.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_return_stack.h"
#include "verilated.h"

typedef Vpenumbra3_return_stack Dut;

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}
static void tick(Dut* d) {
    d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval();
}
static uint32_t ret_addr(int k) { return 0x1000 + 0x10 * k; }
static void push(Dut* d, uint32_t addr) {
    d->i_push = 1; d->i_push_addr = addr; tick(d); d->i_push = 0; d->eval();
}
// The top a pop predicts, then the pop itself.
static uint32_t pop(Dut* d) {
    d->eval();
    uint32_t top = d->o_top;
    d->i_pop = 1; tick(d); d->i_pop = 0; d->eval();
    return top;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* d = new Dut;
    d->i_push = 0; d->i_pop = 0;
    d->i_rst = 1; tick(d); tick(d); d->i_rst = 0; d->eval();

    // ── LIFO ──
    for (int k = 1; k <= 3; k++) push(d, ret_addr(k));
    check("lifo_3", pop(d), ret_addr(3));
    check("lifo_2", pop(d), ret_addr(2));
    push(d, ret_addr(4));
    check("reuse_4", pop(d), ret_addr(4));
    check("lifo_1", pop(d), ret_addr(1));

    // ── Nine pushes into eight slots: the ninth overwrites the first ──
    for (int k = 11; k <= 19; k++) push(d, ret_addr(k));
    for (int k = 19; k >= 12; k--) check("overflow_inner", pop(d), ret_addr(k));
    check("overflow_wraps", pop(d), ret_addr(19));

    // ── The low address bits are not kept ──
    push(d, 0x2003);
    check("aligned", pop(d), 0x2000);

    printf("penumbra3_return_stack: %d/%d checks passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);
    delete d;
    return errors ? 1 : 0;
}
