// Verilator testbench for penumbra3_predecode.
//
// Each Format B condition class (B, BL, Bcc), forward and backward
// offsets at both ends of the range, a return (JMP R13) against other
// JMPs, JALR, and words that are no control transfer at all.

#include <cstdio>
#include <cstdint>
#include "Vpenumbra3_predecode.h"
#include "verilated.h"

typedef Vpenumbra3_predecode Dut;

static int errors = 0, tests = 0;
static void check(const char* n, uint32_t g, uint32_t e) {
    tests++;
    if (g != e) { printf("  FAIL [%s]: got 0x%X, expected 0x%X\n", n, g, e); errors++; }
}

enum { COND_AL = 0x0, COND_NE = 0x2, COND_LE = 0xE, COND_BL = 0xF };
enum { OP_L_ADDI = 0x3, OP_L_JMP = 0xB, OP_L_JALR = 0xC };

// Format B: [31:30]=11, cond [29:26], word offset [25:4] from the branch.
static uint32_t enc_b(uint32_t cond, int32_t words) {
    return (3u << 30) | ((cond & 0xF) << 26) | (((uint32_t)words & 0x3FFFFF) << 4);
}
// Format L: [31:30]=01, op [29:26], Rd [25:22].
static uint32_t enc_l(uint32_t op, uint32_t rd) {
    return (1u << 30) | ((op & 0xF) << 26) | ((rd & 0xF) << 22);
}

struct Expect { bool direct, cond, call, ret; };

static void run(Dut* d, const char* name, uint32_t word, uint32_t pc,
                Expect e, uint32_t target = 0) {
    d->i_word = word; d->i_pc = pc; d->eval();
    char n[64];
    snprintf(n, sizeof n, "%s_direct", name); check(n, d->o_direct, e.direct);
    snprintf(n, sizeof n, "%s_cond", name);   check(n, d->o_conditional, e.cond);
    snprintf(n, sizeof n, "%s_call", name);   check(n, d->o_call, e.call);
    snprintf(n, sizeof n, "%s_return", name); check(n, d->o_return, e.ret);
    if (e.direct) {
        snprintf(n, sizeof n, "%s_target", name); check(n, d->o_target, target);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Dut* d = new Dut;
    const uint32_t PC = 0x00402000;

    run(d, "b",      enc_b(COND_AL, 3),  PC, {1, 0, 0, 0}, PC + 12);
    run(d, "bl",     enc_b(COND_BL, -2), PC, {1, 0, 1, 0}, PC - 8);
    run(d, "bne",    enc_b(COND_NE, -1), PC, {1, 1, 0, 0}, PC - 4);
    run(d, "ble",    enc_b(COND_LE, 0),  PC, {1, 1, 0, 0}, PC);
    run(d, "b_max",  enc_b(COND_AL, 0x1FFFFF),  PC, {1, 0, 0, 0}, PC + 0x7FFFFC);
    run(d, "b_min",  enc_b(COND_AL, -0x200000), PC, {1, 0, 0, 0}, PC - 0x800000);
    run(d, "b_wrap", enc_b(COND_AL, 1), 0xFFFFFFFC, {1, 0, 0, 0}, 0x00000000);
    run(d, "ret",    enc_l(OP_L_JMP, 13),  PC, {0, 0, 0, 1});
    run(d, "jmp_r5", enc_l(OP_L_JMP, 5),   PC, {0, 0, 0, 0});
    run(d, "jalr",   enc_l(OP_L_JALR, 7),  PC, {0, 0, 1, 0});
    run(d, "jalr13", enc_l(OP_L_JALR, 13), PC, {0, 0, 1, 0});
    run(d, "addi13", enc_l(OP_L_ADDI, 13), PC, {0, 0, 0, 0});
    run(d, "fmt_r",  0x00000000,           PC, {0, 0, 0, 0});
    run(d, "fmt_m",  (2u << 30) | (13u << 22), PC, {0, 0, 0, 0});

    printf("penumbra3_predecode: %d/%d checks passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);
    delete d;
    return errors ? 1 : 0;
}
