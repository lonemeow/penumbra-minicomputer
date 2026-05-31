// Verilator testbench for the Penumbra MUL/DIV peer unit (divmul).
//
// Increment 1 covers MULTIPLY only (signed ALU_MUL and unsigned ALU_MULU):
//   - drives the i_start / o_busy handshake,
//   - waits out the 32-cycle iteration,
//   - checks both result halves and the Z/N flags against a C reference.
//
// Expected products are computed in C with the correct signedness so the
// testbench validates the RTL, not hand-arithmetic.

#include <cstdio>
#include <cstdint>
#include "Vdivmul.h"

// Op encodings — must match penumbra_pkg::ALU_*.
enum { OP_MUL = 13, OP_MULU = 14, OP_DIV = 15, OP_DIVU = 16 };

static int errors = 0, tests = 0;

static void tick(Vdivmul* d) { d->i_clk = 0; d->eval(); d->i_clk = 1; d->eval(); }

// Run one multiply to completion; return the 64-bit result via lo/hi.
static void run_mul(Vdivmul* d, uint8_t op, uint32_t a, uint32_t b,
                    uint32_t* lo, uint32_t* hi) {
    d->i_a = a; d->i_b = b; d->i_rdh = 0; d->i_op = op; d->i_start = 1;
    tick(d);                 // posedge latches operands, state → ITER
    d->i_start = 0;
    int guard = 0;
    while (d->o_busy && guard++ < 100) tick(d);
    *lo = d->o_result_lo;
    *hi = d->o_result_hi;
}

static void check_mul(Vdivmul* d, const char* name, uint8_t op,
                      uint32_t a, uint32_t b, uint64_t expected) {
    tests++;
    uint32_t lo, hi;
    run_mul(d, op, a, b, &lo, &hi);
    uint64_t got = ((uint64_t)hi << 32) | lo;

    int exp_z = (lo == 0) ? 1 : 0;
    int exp_n = (lo >> 31) & 1;

    int fail = 0;
    if (got != expected) {
        printf("  FAIL [%s] result: got 0x%016llX, expected 0x%016llX\n",
               name, (unsigned long long)got, (unsigned long long)expected);
        fail = 1;
    }
    if (d->o_busy != 0) {
        printf("  FAIL [%s] o_busy still high after completion\n", name);
        fail = 1;
    }
    if (d->o_flag_z != exp_z) {
        printf("  FAIL [%s] Z: got %d, expected %d\n", name, d->o_flag_z, exp_z);
        fail = 1;
    }
    if (d->o_flag_n != exp_n) {
        printf("  FAIL [%s] N: got %d, expected %d\n", name, d->o_flag_n, exp_n);
        fail = 1;
    }
    if (fail) errors++;
}

static uint64_t umul(uint32_t a, uint32_t b) {
    return (uint64_t)a * (uint64_t)b;
}
static uint64_t smul(uint32_t a, uint32_t b) {
    return (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
}

// Run one divide to completion; return quotient (lo) and remainder (hi).
static void run_div(Vdivmul* d, uint8_t op, uint32_t rdh, uint32_t a, uint32_t b,
                    uint32_t* lo, uint32_t* hi) {
    d->i_a = a; d->i_b = b; d->i_rdh = rdh; d->i_op = op; d->i_start = 1;
    tick(d);                 // posedge latches operands, state → ITER
    d->i_start = 0;
    int guard = 0;
    while (d->o_busy && guard++ < 100) tick(d);
    *lo = d->o_result_lo;
    *hi = d->o_result_hi;
}

// Check a non-faulting unsigned divide. Dividend = {rdh, a}, divisor = b;
// the C reference computes quotient/remainder (inputs chosen so q fits 32 bits).
static void check_divu(Vdivmul* d, const char* name,
                       uint32_t rdh, uint32_t a, uint32_t b) {
    tests++;
    uint64_t n   = ((uint64_t)rdh << 32) | a;
    uint32_t q   = (uint32_t)(n / b);
    uint32_t rem = (uint32_t)(n % b);

    uint32_t lo, hi;
    run_div(d, OP_DIVU, rdh, a, b, &lo, &hi);

    int exp_z = (q == 0) ? 1 : 0;
    int exp_n = (q >> 31) & 1;

    int fail = 0;
    if (lo != q) {
        printf("  FAIL [%s] quotient: got 0x%08X, expected 0x%08X\n", name, lo, q);
        fail = 1;
    }
    if (hi != rem) {
        printf("  FAIL [%s] remainder: got 0x%08X, expected 0x%08X\n", name, hi, rem);
        fail = 1;
    }
    if (d->o_fault != 0) {
        printf("  FAIL [%s] o_fault asserted on a valid divide\n", name);
        fail = 1;
    }
    if (d->o_flag_z != exp_z) {
        printf("  FAIL [%s] Z: got %d, expected %d\n", name, d->o_flag_z, exp_z);
        fail = 1;
    }
    if (d->o_flag_n != exp_n) {
        printf("  FAIL [%s] N: got %d, expected %d\n", name, d->o_flag_n, exp_n);
        fail = 1;
    }
    if (fail) errors++;
}

// Check a non-faulting signed divide (32/32, i_rdh ignored). Expected via the
// C int32 truncating-division reference.
static void check_sdiv(Vdivmul* d, const char* name, uint32_t a, uint32_t b) {
    tests++;
    int32_t  q   = (int32_t)a / (int32_t)b;
    int32_t  rmd = (int32_t)a % (int32_t)b;
    uint32_t lo, hi;
    run_div(d, OP_DIV, 0, a, b, &lo, &hi);

    int exp_z = (q == 0) ? 1 : 0;
    int exp_n = ((uint32_t)q >> 31) & 1;

    int fail = 0;
    if (lo != (uint32_t)q) {
        printf("  FAIL [%s] quotient: got 0x%08X, expected 0x%08X\n", name, lo, (uint32_t)q);
        fail = 1;
    }
    if (hi != (uint32_t)rmd) {
        printf("  FAIL [%s] remainder: got 0x%08X, expected 0x%08X\n", name, hi, (uint32_t)rmd);
        fail = 1;
    }
    if (d->o_fault != 0) {
        printf("  FAIL [%s] o_fault asserted on a valid divide\n", name);
        fail = 1;
    }
    if (d->o_flag_z != exp_z) {
        printf("  FAIL [%s] Z: got %d, expected %d\n", name, d->o_flag_z, exp_z);
        fail = 1;
    }
    if (d->o_flag_n != exp_n) {
        printf("  FAIL [%s] N: got %d, expected %d\n", name, d->o_flag_n, exp_n);
        fail = 1;
    }
    if (fail) errors++;
}

// Signed divide with explicit expected values — for INT_MIN / -1, which is UB
// in C and can't go through the int reference. Hardware must NOT fault here.
static void check_sdiv_exp(Vdivmul* d, const char* name, uint32_t a, uint32_t b,
                           uint32_t exp_q, uint32_t exp_r) {
    tests++;
    uint32_t lo, hi;
    run_div(d, OP_DIV, 0, a, b, &lo, &hi);
    int fail = 0;
    if (lo != exp_q) {
        printf("  FAIL [%s] quotient: got 0x%08X, expected 0x%08X\n", name, lo, exp_q);
        fail = 1;
    }
    if (hi != exp_r) {
        printf("  FAIL [%s] remainder: got 0x%08X, expected 0x%08X\n", name, hi, exp_r);
        fail = 1;
    }
    if (d->o_fault != 0) {
        printf("  FAIL [%s] o_fault asserted — INT_MIN/-1 must not trap\n", name);
        fail = 1;
    }
    if (fail) errors++;
}

// Check that a faulting divide asserts o_fault and never iterates (o_busy low).
static void check_div_fault(Vdivmul* d, const char* name, uint8_t op,
                            uint32_t rdh, uint32_t a, uint32_t b) {
    tests++;
    d->i_a = a; d->i_b = b; d->i_rdh = rdh; d->i_op = op; d->i_start = 1;
    tick(d);                 // posedge: faulting div latches fault_q, stays IDLE
    d->i_start = 0;

    int fail = 0;
    if (d->o_fault != 1) {
        printf("  FAIL [%s] o_fault not asserted\n", name);
        fail = 1;
    }
    if (d->o_busy != 0) {
        printf("  FAIL [%s] o_busy asserted — faulting divide must not iterate\n", name);
        fail = 1;
    }
    tick(d);                 // confirm it stays idle (no spurious iteration)
    if (d->o_busy != 0) {
        printf("  FAIL [%s] o_busy asserted one cycle later\n", name);
        fail = 1;
    }
    if (fail) errors++;
}

int main() {
    Vdivmul* d = new Vdivmul;

    // Reset.
    d->i_rst = 1; d->i_start = 0; d->i_a = 0; d->i_b = 0; d->i_rdh = 0; d->i_op = 0;
    tick(d);
    d->i_rst = 0;
    tick(d);

    // ── Unsigned multiply (MULU) ─────────────────────────────────
    check_mul(d, "mulu_3x5",        OP_MULU, 3, 5,                   umul(3, 5));
    check_mul(d, "mulu_zero",       OP_MULU, 0, 0x12345,             umul(0, 0x12345));
    check_mul(d, "mulu_by_zero",    OP_MULU, 0xDEADBEEF, 0,          umul(0xDEADBEEF, 0));
    check_mul(d, "mulu_max_x_max",  OP_MULU, 0xFFFFFFFF, 0xFFFFFFFF, umul(0xFFFFFFFF, 0xFFFFFFFF));
    check_mul(d, "mulu_64k_x_64k",  OP_MULU, 0x10000, 0x10000,       umul(0x10000, 0x10000));
    check_mul(d, "mulu_carry_heavy",OP_MULU, 0xDEADBEEF, 0x12345678, umul(0xDEADBEEF, 0x12345678));
    check_mul(d, "mulu_one",        OP_MULU, 1, 0xABCDEF01,          umul(1, 0xABCDEF01));

    // ── Signed multiply (MUL) ────────────────────────────────────
    check_mul(d, "mul_pos",         OP_MUL, 6, 7,                    smul(6, 7));
    check_mul(d, "mul_neg_pos",     OP_MUL, 0xFFFFFFFD, 5,           smul(0xFFFFFFFD, 5));   // -3 * 5
    check_mul(d, "mul_neg_neg",     OP_MUL, 0xFFFFFFFC, 0xFFFFFFFA,  smul(0xFFFFFFFC, 0xFFFFFFFA)); // -4 * -6
    check_mul(d, "mul_pos_neg",     OP_MUL, 7, 0xFFFFFFFF,           smul(7, 0xFFFFFFFF));   // 7 * -1
    check_mul(d, "mul_intmin_x1",   OP_MUL, 0x80000000, 1,           smul(0x80000000, 1));
    check_mul(d, "mul_intmin_sq",   OP_MUL, 0x80000000, 0x80000000,  smul(0x80000000, 0x80000000));
    check_mul(d, "mul_big_neg",     OP_MUL, 0x80000000, 0x7FFFFFFF,  smul(0x80000000, 0x7FFFFFFF));

    // ── Unsigned divide (DIVU), plain 32/32 (rdh = 0) ────────────
    check_divu(d, "divu_17_5",      0, 17, 5);            // 3 r 2
    check_divu(d, "divu_exact",     0, 100, 10);          // 10 r 0
    check_divu(d, "divu_rem",       0, 7, 3);             // 2 r 1
    check_divu(d, "divu_zero_num",  0, 0, 7);             // 0 r 0
    check_divu(d, "divu_by_one",    0, 0xFFFFFFFF, 1);    // max r 0
    check_divu(d, "divu_max_max",   0, 0xFFFFFFFF, 0xFFFFFFFF); // 1 r 0
    check_divu(d, "divu_lt_divisor",0, 3, 5);             // 0 r 3 (quotient 0 → Z=1)
    check_divu(d, "divu_big",       0, 0xDEADBEEF, 0x1234);

    // ── Unsigned divide (DIVU), narrowing 64/32 (rdh < divisor) ──
    check_divu(d, "divu_narrow_a",  2, 0x00000000, 3);    // 0x2_00000000 / 3
    check_divu(d, "divu_narrow_b",  1, 0xFFFFFFFF, 2);    // 0x1_FFFFFFFF / 2
    check_divu(d, "divu_narrow_max",0xFFFFFFFE, 0xFFFFFFFF, 0xFFFFFFFF); // hi just under divisor

    // ── Signed divide (DIV), 32/32, i_rdh ignored ───────────────
    check_sdiv(d, "sdiv_pos_pos",   17, 5);               //  3 r  2
    check_sdiv(d, "sdiv_neg_pos",   (uint32_t)-17, 5);    // -3 r -2
    check_sdiv(d, "sdiv_pos_neg",   17, (uint32_t)-5);    // -3 r  2
    check_sdiv(d, "sdiv_neg_neg",   (uint32_t)-17, (uint32_t)-5); // 3 r -2
    check_sdiv(d, "sdiv_exact_neg", (uint32_t)-100, 10);  // -10 r 0
    check_sdiv(d, "sdiv_lt",        (uint32_t)-3, 5);     //  0 r -3
    check_sdiv(d, "sdiv_big",       0x7FFFFFFF, 3);
    check_sdiv(d, "sdiv_minby2",    0x80000000, 2);       // INT_MIN / 2
    check_sdiv(d, "sdiv_neg_pos2",  (uint32_t)-100, 7);   // -14 r -2

    // i_rdh must be ignored for signed divide: a nonzero rdh changes nothing.
    {
        uint32_t lo, hi;
        run_div(d, OP_DIV, 0xDEADBEEF, (uint32_t)-17, 5, &lo, &hi);  // -17 / 5 = -3 r -2
        tests++;
        if (lo != (uint32_t)-3 || hi != (uint32_t)-2 || d->o_fault != 0) {
            printf("  FAIL [sdiv_rdh_ignored] got q=0x%08X r=0x%08X fault=%d "
                   "(rdh must not affect signed divide)\n", lo, hi, (int)d->o_fault);
            errors++;
        }
    }

    // INT_MIN / -1 overflow: UB in C; hardware returns INT_MIN, no trap.
    check_sdiv_exp(d, "sdiv_intmin_m1", 0x80000000, (uint32_t)-1, 0x80000000, 0);

    // ── Divide faults ────────────────────────────────────────────
    check_div_fault(d, "div0_plain",     OP_DIVU, 0, 42, 0);  // unsigned divide by zero
    check_div_fault(d, "div0_narrow",    OP_DIVU, 1, 0, 0);   // unsigned divide by zero (64/32)
    check_div_fault(d, "ovf_hi_gt",      OP_DIVU, 5, 0, 3);   // rdh > divisor
    check_div_fault(d, "ovf_hi_eq",      OP_DIVU, 3, 0, 3);   // rdh == divisor (q won't fit)
    check_div_fault(d, "sdiv_div0",      OP_DIV,  0, 42, 0);  // signed divide by zero

    printf("divmul: %d/%d tests passed\n", tests - errors, tests);
    if (errors) printf("  *** %d FAILED ***\n", errors);
    delete d;
    return errors ? 1 : 0;
}
