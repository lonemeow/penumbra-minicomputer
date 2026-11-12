// Verilator testbench for penumbra3_direction_table (1K entries).
//
// A trained counter reads back at every PC that indexes it, the word
// address selecting; a read lands the cycle after its fetch launches and
// holds while no fetch launches; a training write in the read's cycle leaves
// a read of another entry alone and gives a read of its own entry the
// counter it writes.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_direction_table.h"
#include "verilated.h"

typedef Vpenumbra3_direction_table Dut;

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}
static void tick(Dut* d) {
    d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval();
}
static uint32_t index_of(uint32_t pc) { return (pc >> 2) & 0x3FF; }
static void train(Dut* d, uint32_t pc, uint32_t counter) {
    d->i_train_en = 1; d->i_train_index = index_of(pc); d->i_train_counter = counter;
    tick(d); d->i_train_en = 0;
}
static uint32_t read(Dut* d, uint32_t pc) {
    d->i_read_en = 1; d->i_fetch_pc = pc; tick(d); d->i_read_en = 0; d->eval();
    return d->o_counter;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* d = new Dut;
    d->i_read_en = 0; d->i_train_en = 0;

    train(d, 0x00400100, 3);
    train(d, 0x00400104, 1);
    check("read_trained",  read(d, 0x00400100), 3);
    check("read_neighbor", read(d, 0x00400104), 1);
    check("read_alias",    read(d, 0x00400100 + 4096), 3);   // same index, 4 KB on

    // No launch: the counter holds.
    d->i_fetch_pc = 0x00400104; tick(d); d->eval();
    check("hold", d->o_counter, 3);

    // A training write to another entry leaves the read in its cycle alone.
    d->i_read_en = 1; d->i_fetch_pc = 0x00400104;
    d->i_train_en = 1; d->i_train_index = index_of(0x00400100); d->i_train_counter = 0;
    tick(d); d->i_read_en = 0; d->i_train_en = 0; d->eval();
    check("train_other_read",   d->o_counter, 1);
    check("train_other_landed", read(d, 0x00400100), 0);

    // A training write to the read's own entry gives the read the counter
    // it writes, held while no fetch launches.
    d->i_read_en = 1; d->i_fetch_pc = 0x00400104;
    d->i_train_en = 1; d->i_train_index = index_of(0x00400104); d->i_train_counter = 2;
    tick(d); d->i_read_en = 0; d->i_train_en = 0; d->eval();
    check("train_same_read", d->o_counter, 2);
    tick(d); d->eval();
    check("train_same_hold", d->o_counter, 2);
    check("train_same_landed", read(d, 0x00400104), 2);
    // The next read takes the block RAM's result again.
    check("after_bypass", read(d, 0x00400100), 0);

    printf("penumbra3_direction_table: %d/%d checks passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);
    delete d;
    return errors ? 1 : 0;
}
