/*
 * librt.c — Compiler runtime builtins for bare-metal benchmarks
 *
 * Penumbra has no hardware MUL/DIV, so clang emits calls to these
 * functions for multiply/divide/modulo operations.
 */

typedef unsigned int uint32_t;

static inline void __divmodsi4(uint32_t n, uint32_t d,
                               uint32_t *qp, uint32_t *rp) {
    uint32_t q = 0;
    uint32_t r = 0;

    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (1U << i);
        }
    }
    if (qp) *qp = q;
    if (rp) *rp = r;
}

uint32_t __mulsi3(uint32_t a, uint32_t b) {
    uint32_t result = 0;
    while (a) {
        if (a & 1)
            result += b;
        a >>= 1;
        b <<= 1;
    }
    return result;
}

uint32_t __udivsi3(uint32_t n, uint32_t d) {
    uint32_t q;
    if (d == 0) return 0;
    __divmodsi4(n, d, &q, 0);
    return q;
}

uint32_t __umodsi3(uint32_t n, uint32_t d) {
    uint32_t r;
    if (d == 0) return 0;
    __divmodsi4(n, d, 0, &r);
    return r;
}

int __divsi3(int n, int d) {
    if (d == 0) return 0;
    int neg = 0;
    uint32_t un = (uint32_t)n;
    uint32_t ud = (uint32_t)d;
    if (n < 0) { neg = !neg; un = (uint32_t)(-n); }
    if (d < 0) { neg = !neg; ud = (uint32_t)(-d); }
    uint32_t q;
    __divmodsi4(un, ud, &q, 0);
    return neg ? -(int)q : (int)q;
}

int __modsi3(int n, int d) {
    if (d == 0) return 0;
    uint32_t un = (uint32_t)(n < 0 ? -n : n);
    uint32_t ud = (uint32_t)(d < 0 ? -d : d);
    uint32_t r;
    __divmodsi4(un, ud, 0, &r);
    return n < 0 ? -(int)r : (int)r;
}
